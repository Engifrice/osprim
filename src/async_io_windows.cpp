/*****************************************************************/ /**
 * @file   async_io_windows.cpp
 * @brief  Async I/O on Windows: an I/O completion port per queue.
 *
 * - Attaching binds the file to the port (CreateIoCompletionPort, key
 *   KEY_IO). The blocking calls of file_windows.cpp keep working on it:
 *   their events carry the low bit, so no packet reaches the port.
 * - An operation's OVERLAPPED lives in async_io_op::native, with no event:
 *   the kernel queues a packet when it completes. Successes during
 *   submission queue one too (FILE_SKIP_COMPLETION_PORT_ON_SUCCESS is never
 *   set: it would change the file object for every user).
 * - Failures during submission queue nothing: the library posts the packet
 *   itself (KEY_FAILED, the Win32 error in OVERLAPPED::Internal), so every
 *   operation completes through the port.
 * - Results come from GetOverlappedResult (no system call: it reads the
 *   OVERLAPPED), which maps the NTSTATUS to a Win32 error as file_read_at()
 *   reports it. End of file is OK with 0 bytes, ERROR_OPERATION_ABORTED is
 *   CANCELED.
 * - Wakes are packets with KEY_WAKE, coalesced by a pending flag cleared
 *   by the owner after it takes the packet: a wake sent in between finds
 *   the flag still set and is covered by the wait that is returning.
 * - GetQueuedCompletionStatusEx takes milliseconds: the remaining time is
 *   rounded up, and a timeout is reported only once the awake clock has
 *   reached the deadline (otherwise an early return, as wait()).
 *********************************************************************/
#include "internal.hpp"

#if !OSPRIM_OS_WINDOWS
  #error "async_io_windows.cpp is for Windows only"
#endif

#include "nt.hpp"
#include "win32.hpp"

#include <atomic>
#include <new>

namespace osprim
{
  inline namespace v0_1
  {
    namespace
    {
      template<class R>
      status<R> fail(R r, DWORD err) noexcept
      {
        return {r, native_error_kind::NATIVE_ERROR_WIN32, static_cast<int32_t>(err)};
      }

      template<class R>
      status<R> fail(R r) noexcept
      {
        return {r, native_error_kind::NATIVE_ERROR_NONE, 0};
      }

      /// @brief Packets from the kernel, for attached files.
      constexpr ULONG_PTR KEY_IO = 0;
      /// @brief Posted by submission: the operation failed to start, the
      /// Win32 error is in OVERLAPPED::Internal.
      constexpr ULONG_PTR KEY_FAILED = 1;
      /// @brief Posted by async_io_queue_wake().
      constexpr ULONG_PTR KEY_WAKE = 2;

      /// @brief FileModeInformation flag of handles opened for synchronous
      /// I/O, alongside NT_FILE_SYNCHRONOUS_IO_NONALERT.
      constexpr ULONG NT_FILE_SYNCHRONOUS_IO_ALERT = 0x00000010;

      /// @brief Packets taken per GetQueuedCompletionStatusEx call.
      constexpr ULONG ENTRY_BATCH = 64;

      /// @brief The backend state in async_io_queue::native.
      struct iocp_queue
      {
        HANDLE port;
        uint64_t id;
        uint32_t capacity;
        /// @brief Submitted and not yet returned by a wait (owner only)
        uint32_t in_flight;
        /// @brief 1 while a KEY_WAKE packet is queued
        std::atomic<uint32_t> wake_pending;
      };

      static_assert(sizeof(OVERLAPPED) <= ASYNC_IO_OP_NATIVE_SIZE);
      static_assert(alignof(OVERLAPPED) <= 8);

      iocp_queue* state(async_io_queue& q) noexcept
      {
        return detail::async_io_state<iocp_queue>(q);
      }

      OVERLAPPED* overlapped_of(async_io_op& op) noexcept
      {
        return std::launder(reinterpret_cast<OVERLAPPED*>(op.native));
      }

      HANDLE as_handle(native_file_t n) noexcept
      {
        return static_cast<HANDLE>(n);
      }

      bool is_io_error(DWORD e) noexcept
      {
        return e == ERROR_CRC || e == ERROR_IO_DEVICE || e == ERROR_READ_FAULT
               || e == ERROR_WRITE_FAULT || e == ERROR_GEN_FAILURE
               || e == ERROR_SECTOR_NOT_FOUND || e == ERROR_DEVICE_HARDWARE_ERROR
               || e == ERROR_NOT_READY;
      }

      bool is_no_space(DWORD e) noexcept
      {
        return e == ERROR_DISK_FULL || e == ERROR_HANDLE_DISK_FULL
               || e == ERROR_DISK_QUOTA_EXCEEDED;
      }

      /// @brief The result of an operation that failed with `e`.
      status<async_io_op_result> op_error(DWORD e) noexcept
      {
        using R = async_io_op_result;
        if (e == ERROR_OPERATION_ABORTED)
          return fail(R::ASYNC_IO_OP_CANCELED, e);
        if (is_no_space(e))
          return fail(R::ASYNC_IO_OP_NO_SPACE, e);
        if (e == ERROR_FILE_TOO_LARGE)
          return fail(R::ASYNC_IO_OP_FILE_TOO_LARGE, e);
        if (is_io_error(e))
          return fail(R::ASYNC_IO_OP_IO_ERROR, e);
        if (e == ERROR_INVALID_PARAMETER)
          return fail(R::ASYNC_IO_OP_MISALIGNED, e);
        return fail(R::ASYNC_IO_OP_SYSTEM_ERROR, e);
      }

      /// @brief Writes the result of an operation taken from the port.
      void complete(async_io_op& op, ULONG_PTR key, DWORD bytes) noexcept
      {
        OVERLAPPED* ov = overlapped_of(op);
        op.result      = {};
        op.transferred = 0;
        DWORD e        = 0;
        if (key == KEY_FAILED)
          e = static_cast<DWORD>(ov->Internal);
        else
        {
          DWORD n = 0;
          if (::GetOverlappedResult(as_handle(op.file.native), ov, &n, FALSE))
          {
            op.transferred = n;
            return;
          }
          e              = ::GetLastError();
          op.transferred = bytes; // what a cancelled transfer moved
        }
        if (e == ERROR_HANDLE_EOF)
        {
          op.transferred = 0; // reading at or past the end: OK, 0 bytes
          return;
        }
        op.result = op_error(e);
      }

      /// @brief Milliseconds from now until `deadline`, rounded up.
      /// @return false if the deadline has passed
      bool timeout_ms(awake_instant deadline, DWORD* out) noexcept
      {
        if (deadline.is_forever())
        {
          *out = INFINITE;
          return true;
        }
        const awake_instant now = awake_now();
        if (now >= deadline)
          return false;
        const int64_t remaining = deadline.ns - now.ns; // > 0, no overflow
        const int64_t ms = remaining / 1000000 + (remaining % 1000000 != 0 ? 1 : 0);
        // INFINITE is 0xFFFFFFFF: stay just below (about 49.7 days)
        *out = ms >= static_cast<int64_t>(INFINITE) ? INFINITE - 1
                                                    : static_cast<DWORD>(ms);
        return true;
      }
    } // namespace

    namespace detail
    {
      status<async_io_queue_create_result> platform_async_io_create(
          uint32_t capacity, uint64_t id, async_io_queue& q) noexcept
      {
        const HANDLE port =
            ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
        if (port == nullptr)
          return fail(
              async_io_queue_create_result::ASYNC_IO_QUEUE_CREATE_SYSTEM_ERROR,
              ::GetLastError());
        iocp_queue* s = new (q.native) iocp_queue();
        s->port       = port;
        s->id         = id;
        s->capacity   = capacity;
        s->in_flight  = 0;
        s->wake_pending.store(0, std::memory_order_relaxed);
        return {};
      }

      void platform_async_io_destroy(async_io_queue& q) noexcept
      {
        iocp_queue* s = state(q);
        ::CloseHandle(s->port); // a queued wake packet goes with it
        s->~iocp_queue();
      }

      uint64_t platform_async_io_id(async_io_queue& q) noexcept
      {
        return state(q)->id;
      }

      uint32_t platform_async_io_in_flight(async_io_queue& q) noexcept
      {
        return state(q)->in_flight;
      }

      status<async_io_attach_result> platform_async_io_attach(
          async_io_queue& q, native_file_t file) noexcept
      {
        using R        = async_io_attach_result;
        const HANDLE h = as_handle(file);
        if (g_nt.query_information_file != nullptr)
        {
          ULONG mode = 0;
          nt_io_status_block iosb{};
          if (g_nt.query_information_file(
                  h, &iosb, &mode, sizeof(mode), NT_FILE_MODE_INFORMATION)
                  >= 0
              && (mode
                  & (NT_FILE_SYNCHRONOUS_IO_ALERT | NT_FILE_SYNCHRONOUS_IO_NONALERT))
                     != 0)
            return fail(R::ASYNC_IO_ATTACH_NOT_OVERLAPPED);
        }
        if (::CreateIoCompletionPort(h, state(q)->port, KEY_IO, 0) == nullptr)
          return fail(R::ASYNC_IO_ATTACH_SYSTEM_ERROR, ::GetLastError());
        return {};
      }

      status<async_io_submit_result> platform_async_io_submit(
          async_io_queue& q, async_io_op* const* ops, size_t count,
          size_t* submitted) noexcept
      {
        iocp_queue* s = state(q);
        size_t i      = 0;
        for (; i < count && s->in_flight < s->capacity; ++i)
        {
          async_io_op& op    = *ops[i];
          OVERLAPPED* ov     = new (op.native) OVERLAPPED{};
          ov->Offset         = static_cast<DWORD>(op.offset);
          ov->OffsetHigh     = static_cast<DWORD>(op.offset >> 32);
          const HANDLE h     = as_handle(op.file.native);
          const BOOL started = op.kind == async_io_op_kind::ASYNC_IO_OP_READ
                                   ? ::ReadFile(h, op.buffer, op.size, nullptr, ov)
                                   : ::WriteFile(h, op.buffer, op.size, nullptr, ov);
          if (!started)
          {
            const DWORD e = ::GetLastError();
            if (e != ERROR_IO_PENDING)
            {
              // Failed before starting: no packet was queued
              ov->Internal = e;
              if (!::PostQueuedCompletionStatus(s->port, 0, KEY_FAILED, ov))
              {
                *submitted = i;
                return fail(
                    async_io_submit_result::ASYNC_IO_SUBMIT_SYSTEM_ERROR,
                    ::GetLastError());
              }
            }
          }
          ++s->in_flight;
        }
        *submitted = i;
        return {};
      }

      status<async_io_wait_result> platform_async_io_wait(
          async_io_queue& q, async_io_op** out, size_t capacity, size_t min_count,
          awake_instant deadline, size_t* count) noexcept
      {
        using R       = async_io_wait_result;
        iocp_queue* s = state(q);
        OVERLAPPED_ENTRY entries[ENTRY_BATCH];
        size_t n   = 0;
        bool woken = false;
        for (;;)
        {
          const bool need_more = n < min_count && !woken;
          DWORD wait_ms        = 0;
          if (need_more && !timeout_ms(deadline, &wait_ms))
          {
            *count = n;
            return {
                R::ASYNC_IO_WAIT_TIMED_OUT, native_error_kind::NATIVE_ERROR_NONE, 0};
          }
          const size_t room = capacity - n;
          if (room == 0)
            break;
          const ULONG ask =
              room > ENTRY_BATCH ? ENTRY_BATCH : static_cast<ULONG>(room);
          ULONG removed = 0;
          if (!::GetQueuedCompletionStatusEx(
                  s->port, entries, ask, &removed, wait_ms, FALSE))
          {
            const DWORD e = ::GetLastError();
            if (e != WAIT_TIMEOUT)
            {
              *count = n;
              return fail(R::ASYNC_IO_WAIT_SYSTEM_ERROR, e);
            }
            if (need_more && !deadline.is_forever() && awake_now() >= deadline)
            {
              *count = n;
              return {
                  R::ASYNC_IO_WAIT_TIMED_OUT, native_error_kind::NATIVE_ERROR_NONE,
                  0};
            }
            break; // nothing ready, or an early timer: an early return
          }
          for (ULONG k = 0; k < removed; ++k)
          {
            const OVERLAPPED_ENTRY& entry = entries[k];
            if (entry.lpCompletionKey == KEY_WAKE)
            {
              // Acquire: a wake merged into this one (it found the flag
              // set) happened before, and its caller's stores are visible
              s->wake_pending.exchange(0, std::memory_order_acq_rel);
              woken = true;
              continue;
            }
            async_io_op* op = async_io_op_from_native(entry.lpOverlapped);
            complete(*op, entry.lpCompletionKey, entry.dwNumberOfBytesTransferred);
            --s->in_flight;
            out[n++] = op;
          }
          // Satisfied: keep collecting only while full batches say more is ready
          if ((n >= min_count || woken) && removed < ask)
            break;
        }
        *count = n;
        return {};
      }

      void platform_async_io_cancel(async_io_queue& q, async_io_op& op) noexcept
      {
        (void)q;
        // FALSE with ERROR_NOT_FOUND: already finished, its packet is queued
        ::CancelIoEx(as_handle(op.file.native), overlapped_of(op));
      }

      void platform_async_io_wake(async_io_queue& q) noexcept
      {
        iocp_queue* s = state(q);
        if (s->wake_pending.exchange(1, std::memory_order_acq_rel) != 0)
          return; // a wake is already queued
        if (!::PostQueuedCompletionStatus(s->port, 0, KEY_WAKE, nullptr))
          s->wake_pending.store(0, std::memory_order_release);
      }
    } // namespace detail
  } // namespace v0_1
} // namespace osprim
