/*****************************************************************/ /**
 * @file   async_io_linux.cpp
 * @brief  Async I/O on Linux: an io_uring per queue, through the raw
 * system calls (no liburing). Android has no backend: its app seccomp
 * filter forbids io_uring, so this file is not built there.
 *
 * Creation never crashes on refusal: io_uring_setup fails cleanly when the
 * kernel lacks io_uring (ENOSYS), when policy forbids it (EPERM: the
 * io_uring_disabled sysctl, container seccomp profiles; EACCES: LSMs), or
 * when the rings exceed RLIMIT_MEMLOCK (ENOMEM, kernels before 5.12).
 * "Too old" comes from feature bits and the opcode probe, never from the
 * version string: SINGLE_MMAP, NODROP, SUBMIT_STABLE and EXT_ARG (5.11, for
 * waits with a timeout), then READ, WRITE and ASYNC_CANCEL.
 *
 * - Sizes: the submission queue holds `capacity` + 1 entries (the wake
 *   poll); the completion queue 2 * `capacity` + 2 (each operation, at most
 *   one cancellation each, the wake poll). The kernel rounds both up to
 *   powers of two. NODROP keeps any overflow in the kernel anyway.
 * - Optional setup flags, dropped one level at a time when the kernel
 *   refuses them (EINVAL): SINGLE_ISSUER and DEFER_TASKRUN (6.0, 6.1: the
 *   queue has one owner thread by contract), SUBMIT_ALL (5.18).
 * - Entries are written to the submission queue and reach the kernel with
 *   the next io_uring_enter: submission, waits, or a full queue. Entries
 *   the kernel did not take (an interrupted submission) stay queued and go
 *   with the next call, so a submitted operation always completes.
 * - user_data is the async_io_op address; internal entries use small odd
 *   values no operation can have (UD_WAKE, UD_CANCEL).
 * - Wakes: async_io_queue_wake() writes an eventfd that a one-shot poll in
 *   the ring watches (a poll rather than a read: eventfd reads only became
 *   non-blocking for io_uring in 5.12). The owner empties the eventfd,
 *   then clears the pending flag, then arms the poll again.
 * - Waits pass the remaining time as a relative timeout (EXT_ARG, on
 *   CLOCK_MONOTONIC, the awake clock); a timeout is reported only once the
 *   awake clock has reached the deadline, and an early one waits again.
 *********************************************************************/
#include "internal.hpp"

#if !OSPRIM_OS_LINUX
  #error "async_io_linux.cpp is for Linux only"
#endif

#include <errno.h>
#include <linux/io_uring.h>
#include <poll.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <new>

// System call numbers: the same on every architecture (added after the
// unification of syscall tables), for C libraries whose headers predate them
#ifndef __NR_io_uring_setup
  #define __NR_io_uring_setup 425
#endif
#ifndef __NR_io_uring_enter
  #define __NR_io_uring_enter 426
#endif
#ifndef __NR_io_uring_register
  #define __NR_io_uring_register 427
#endif

namespace osprim
{
  inline namespace v0_1
  {
    namespace
    {
      template<class R>
      status<R> fail(R r, int err) noexcept
      {
        return {r, native_error_kind::NATIVE_ERROR_POSIX_ERRNO, err};
      }

      template<class R>
      status<R> fail(R r) noexcept
      {
        return {r, native_error_kind::NATIVE_ERROR_NONE, 0};
      }

      // Values from the kernel ABI, defined here so that older kernel
      // headers still build (the kernel decides at run time)

      constexpr uint32_t SETUP_CQSIZE        = 1U << 3;  // 5.5
      constexpr uint32_t SETUP_SUBMIT_ALL    = 1U << 7;  // 5.18
      constexpr uint32_t SETUP_SINGLE_ISSUER = 1U << 12; // 6.0
      constexpr uint32_t SETUP_DEFER_TASKRUN = 1U << 13; // 6.1
      constexpr uint32_t FEAT_SINGLE_MMAP    = 1U << 0;  // 5.4
      constexpr uint32_t FEAT_NODROP         = 1U << 1;  // 5.5
      constexpr uint32_t FEAT_SUBMIT_STABLE  = 1U << 2;  // 5.5
      constexpr uint32_t FEAT_EXT_ARG        = 1U << 8;  // 5.11
      constexpr uint32_t ENTER_GETEVENTS     = 1U << 0;
      constexpr uint32_t ENTER_EXT_ARG       = 1U << 3; // 5.11
      constexpr unsigned REGISTER_PROBE      = 8;       // 5.6
      constexpr uint16_t PROBE_OP_SUPPORTED  = 1U << 0;
      constexpr uint8_t OP_POLL_ADD          = 6;
      constexpr uint8_t OP_ASYNC_CANCEL      = 14;
      constexpr uint8_t OP_READ              = 22;
      constexpr uint8_t OP_WRITE             = 23;
      constexpr uint64_t OFF_SQ_RING         = 0;
      constexpr uint64_t OFF_SQES            = 0x10000000ULL;

      constexpr uint32_t REQUIRED_FEATURES =
          FEAT_SINGLE_MMAP | FEAT_NODROP | FEAT_SUBMIT_STABLE | FEAT_EXT_ARG;

      /// @brief struct __kernel_timespec
      struct kernel_timespec
      {
        int64_t tv_sec;
        long long tv_nsec;
      };

      /// @brief struct io_uring_getevents_arg (5.11)
      struct getevents_arg
      {
        uint64_t sigmask;
        uint32_t sigmask_sz;
        uint32_t pad;
        uint64_t ts;
      };

      /// @brief struct io_uring_probe with room for every opcode
      constexpr size_t PROBE_OPS = 256;
      struct probe_buffer
      {
        uint8_t last_op;
        uint8_t ops_len;
        uint16_t resv;
        uint32_t resv2[3];
        struct
        {
          uint8_t op;
          uint8_t resv;
          uint16_t flags;
          uint32_t resv2;
        } ops[PROBE_OPS];
      };

      /// @brief user_data of the wake poll and of cancellations: odd, so
      /// never the address of an async_io_op.
      constexpr uint64_t UD_WAKE   = 1;
      constexpr uint64_t UD_CANCEL = 3;

      /// @brief The backend state in async_io_queue::native.
      struct uring_queue
      {
        uint64_t id;
        int ring_fd;
        int event_fd;
        void* ring;
        size_t ring_size;
        io_uring_sqe* sqes;
        size_t sqes_size;
        uint32_t* sq_khead;
        uint32_t* sq_ktail;
        uint32_t* cq_khead;
        uint32_t* cq_ktail;
        io_uring_cqe* cqes;
        uint32_t sq_mask;
        uint32_t sq_entries;
        /// @brief Next submission slot (published to sq_ktail before each enter)
        uint32_t sq_tail;
        uint32_t cq_mask;
        uint32_t cq_entries;
        uint32_t capacity;
        /// @brief Submitted and not yet returned by a wait (owner only)
        uint32_t in_flight;
        /// @brief Completions only appear during io_uring_enter
        bool defer_taskrun;
        /// @brief The wake poll is in the ring
        bool wake_armed;
        /// @brief The wake poll failed: wakes are no longer delivered
        bool wake_broken;
        /// @brief 1 while the eventfd holds an unconsumed wake
        std::atomic<uint32_t> wake_pending;
      };

      uring_queue* state(async_io_queue& q) noexcept
      {
        return detail::async_io_state<uring_queue>(q);
      }

      uint32_t load_acquire(uint32_t* p) noexcept
      {
        return std::atomic_ref<uint32_t>(*p).load(std::memory_order_acquire);
      }

      void store_release(uint32_t* p, uint32_t v) noexcept
      {
        std::atomic_ref<uint32_t>(*p).store(v, std::memory_order_release);
      }

      /// @brief Entries written and not yet taken by the kernel.
      uint32_t unsubmitted(uring_queue* s) noexcept
      {
        return s->sq_tail - load_acquire(s->sq_khead);
      }

      /// @brief io_uring_enter after publishing the submission tail.
      /// @return The kernel's result, or -errno
      long enter(
          uring_queue* s, uint32_t min_complete, uint32_t flags,
          const getevents_arg* arg) noexcept
      {
        store_release(s->sq_ktail, s->sq_tail);
        const uint32_t to_submit = unsubmitted(s);
        if (arg != nullptr)
          flags |= ENTER_EXT_ARG;
        const long r = ::syscall(
            __NR_io_uring_enter, s->ring_fd, to_submit, min_complete, flags, arg,
            arg != nullptr ? sizeof(getevents_arg) : size_t{0});
        return r < 0 ? -static_cast<long>(errno) : r;
      }

      /// @brief An io_uring_enter error that leaves the ring usable:
      /// interrupted (EINTR), no memory for requests now (EAGAIN), or
      /// completions waiting to be collected (EBUSY).
      bool transient(long r) noexcept
      {
        return r == -EINTR || r == -EAGAIN || r == -EBUSY;
      }

      /// @brief A cleared submission entry, handing queued entries to the
      /// kernel first if the queue is full.
      /// @return null if the queue stays full
      io_uring_sqe* get_sqe(uring_queue* s) noexcept
      {
        if (unsubmitted(s) >= s->sq_entries)
        {
          (void)enter(s, 0, 0, nullptr);
          if (unsubmitted(s) >= s->sq_entries)
            return nullptr;
        }
        io_uring_sqe* sqe = &s->sqes[s->sq_tail & s->sq_mask];
        ::memset(sqe, 0, sizeof(*sqe));
        return sqe;
      }

      /// @brief Queues the wake poll (it reaches the kernel with the next
      /// enter: a wake sent meanwhile is seen at once).
      void arm_wake(uring_queue* s) noexcept
      {
        io_uring_sqe* sqe = get_sqe(s);
        if (sqe == nullptr)
          return; // tried again by the next wait
        sqe->opcode      = OP_POLL_ADD;
        sqe->fd          = s->event_fd;
        sqe->poll_events = POLLIN;
        sqe->user_data   = UD_WAKE;
        ++s->sq_tail;
        s->wake_armed = true;
      }

      /// @brief The result of an operation that failed with `e`.
      status<async_io_op_result> op_error(int e) noexcept
      {
        using R = async_io_op_result;
        if (e == ECANCELED || e == EINTR)
          return fail(R::ASYNC_IO_OP_CANCELED, e);
        if (e == ENOSPC || e == EDQUOT)
          return fail(R::ASYNC_IO_OP_NO_SPACE, e);
        if (e == EFBIG)
          return fail(R::ASYNC_IO_OP_FILE_TOO_LARGE, e);
        if (e == EIO)
          return fail(R::ASYNC_IO_OP_IO_ERROR, e);
        if (e == EINVAL)
          return fail(R::ASYNC_IO_OP_MISALIGNED, e);
        return fail(R::ASYNC_IO_OP_SYSTEM_ERROR, e);
      }

      /// @brief Takes completions into `out` (from index `n`), handling
      /// internal ones on the way.
      /// @return The new number of operations in `out`
      size_t reap(
          uring_queue* s, async_io_op** out, size_t capacity, size_t n,
          bool* woken) noexcept
      {
        uint32_t head       = *s->cq_khead; // written by this thread only
        const uint32_t tail = load_acquire(s->cq_ktail);
        while (head != tail)
        {
          const io_uring_cqe& cqe = s->cqes[head & s->cq_mask];
          if (cqe.user_data == UD_WAKE)
          {
            s->wake_armed = false;
            if (cqe.res < 0)
              s->wake_broken = true; // not armed again: no busy loop
            // Empty the eventfd, then clear the flag: a wake sent in
            // between finds it still set and is covered by this return
            uint64_t value = 0;
            (void)!::read(s->event_fd, &value, sizeof(value));
            s->wake_pending.exchange(0, std::memory_order_acq_rel);
            *woken = true;
          }
          else if (cqe.user_data != UD_CANCEL)
          {
            if (n == capacity)
              break; // the rest stays for the next wait
            auto* op = reinterpret_cast<async_io_op*>(
                static_cast<uintptr_t>(cqe.user_data));
            op->transferred = 0;
            op->result      = {};
            if (cqe.res >= 0)
              op->transferred = static_cast<uint32_t>(cqe.res);
            else
              op->result = op_error(-cqe.res);
            --s->in_flight;
            out[n++] = op;
          }
          ++head;
        }
        store_release(s->cq_khead, head);
        return n;
      }

      void close_ring(uring_queue* s) noexcept
      {
        if (s->sqes != nullptr)
          ::munmap(s->sqes, s->sqes_size);
        if (s->ring != nullptr)
          ::munmap(s->ring, s->ring_size);
        if (s->ring_fd >= 0)
          ::close(s->ring_fd);
        if (s->event_fd >= 0)
          ::close(s->event_fd);
      }

      /// @brief Maps a refusal of io_uring_setup.
      status<async_io_queue_create_result> setup_error(int e) noexcept
      {
        using R = async_io_queue_create_result;
        switch (e)
        {
        case ENOSYS:
        case EINVAL:
          return fail(R::ASYNC_IO_QUEUE_CREATE_TOO_OLD, e);
        case EPERM:
        case EACCES:
          return fail(R::ASYNC_IO_QUEUE_CREATE_DISABLED, e);
        case ENOMEM:
          return fail(R::ASYNC_IO_QUEUE_CREATE_OUT_OF_MEMORY, e);
        default:
          return fail(R::ASYNC_IO_QUEUE_CREATE_SYSTEM_ERROR, e);
        }
      }

      /// @brief READ, WRITE and ASYNC_CANCEL are supported by the ring.
      /// @return 0, or the errno of the probe
      int probe(int fd, bool* supported) noexcept
      {
        probe_buffer p{};
        if (::syscall(__NR_io_uring_register, fd, REGISTER_PROBE, &p, PROBE_OPS) < 0)
          return errno;
        const uint8_t needed[] = {OP_READ, OP_WRITE, OP_ASYNC_CANCEL};
        *supported             = true;
        for (const uint8_t op : needed)
          if (op > p.last_op || (p.ops[op].flags & PROBE_OP_SUPPORTED) == 0)
            *supported = false;
        return 0;
      }
    } // namespace

    namespace detail
    {
      status<async_io_queue_create_result> platform_async_io_create(
          uint32_t capacity, uint64_t id, async_io_queue& q) noexcept
      {
        using R        = async_io_queue_create_result;
        uring_queue* s = new (q.native) uring_queue();
        s->id          = id;
        s->ring_fd     = -1;
        s->event_fd    = -1;
        s->capacity    = capacity;
        s->wake_pending.store(0, std::memory_order_relaxed);

        // Most capable setup first; EINVAL means an unknown flag
        const uint32_t flag_sets[] = {
            SETUP_CQSIZE | SETUP_SUBMIT_ALL | SETUP_SINGLE_ISSUER
                | SETUP_DEFER_TASKRUN,
            SETUP_CQSIZE | SETUP_SUBMIT_ALL,
            SETUP_CQSIZE,
        };
        io_uring_params p{};
        int e = 0;
        for (const uint32_t flags : flag_sets)
        {
          ::memset(&p, 0, sizeof(p));
          p.flags       = flags;
          p.cq_entries  = 2 * capacity + 2;
          const long fd = ::syscall(__NR_io_uring_setup, capacity + 1, &p);
          if (fd >= 0)
          {
            s->ring_fd = static_cast<int>(fd);
            break;
          }
          e = errno;
          if (e != EINVAL)
            break;
        }
        if (s->ring_fd < 0)
        {
          s->~uring_queue();
          return setup_error(e);
        }
        s->defer_taskrun = (p.flags & SETUP_DEFER_TASKRUN) != 0;

        if ((p.features & REQUIRED_FEATURES) != REQUIRED_FEATURES)
        {
          close_ring(s);
          s->~uring_queue();
          return fail(R::ASYNC_IO_QUEUE_CREATE_TOO_OLD);
        }
        bool supported = false;
        if (const int pe = probe(s->ring_fd, &supported); pe != 0 || !supported)
        {
          close_ring(s);
          s->~uring_queue();
          if (pe != 0 && pe != EINVAL)
            return fail(R::ASYNC_IO_QUEUE_CREATE_SYSTEM_ERROR, pe);
          return pe != 0 ? fail(R::ASYNC_IO_QUEUE_CREATE_TOO_OLD, pe)
                         : fail(R::ASYNC_IO_QUEUE_CREATE_TOO_OLD);
        }

        // One mapping for both rings (SINGLE_MMAP), one for the entries
        const size_t sq_size = p.sq_off.array + p.sq_entries * sizeof(uint32_t);
        const size_t cq_size = p.cq_off.cqes + p.cq_entries * sizeof(io_uring_cqe);
        s->ring_size         = sq_size > cq_size ? sq_size : cq_size;
        void* ring           = ::mmap(
            nullptr, s->ring_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
            s->ring_fd, static_cast<off_t>(OFF_SQ_RING));
        if (ring == MAP_FAILED)
        {
          e = errno;
          close_ring(s);
          s->~uring_queue();
          return e == ENOMEM ? fail(R::ASYNC_IO_QUEUE_CREATE_OUT_OF_MEMORY, e)
                             : fail(R::ASYNC_IO_QUEUE_CREATE_SYSTEM_ERROR, e);
        }
        s->ring      = ring;
        s->sqes_size = p.sq_entries * sizeof(io_uring_sqe);
        void* sqes   = ::mmap(
            nullptr, s->sqes_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
            s->ring_fd, static_cast<off_t>(OFF_SQES));
        if (sqes == MAP_FAILED)
        {
          e = errno;
          close_ring(s);
          s->~uring_queue();
          return e == ENOMEM ? fail(R::ASYNC_IO_QUEUE_CREATE_OUT_OF_MEMORY, e)
                             : fail(R::ASYNC_IO_QUEUE_CREATE_SYSTEM_ERROR, e);
        }
        s->sqes = static_cast<io_uring_sqe*>(sqes);

        auto* base    = static_cast<unsigned char*>(ring);
        s->sq_khead   = reinterpret_cast<uint32_t*>(base + p.sq_off.head);
        s->sq_ktail   = reinterpret_cast<uint32_t*>(base + p.sq_off.tail);
        s->sq_mask    = *reinterpret_cast<uint32_t*>(base + p.sq_off.ring_mask);
        s->sq_entries = p.sq_entries;
        s->sq_tail    = *s->sq_ktail;
        s->cq_khead   = reinterpret_cast<uint32_t*>(base + p.cq_off.head);
        s->cq_ktail   = reinterpret_cast<uint32_t*>(base + p.cq_off.tail);
        s->cq_mask    = *reinterpret_cast<uint32_t*>(base + p.cq_off.ring_mask);
        s->cq_entries = p.cq_entries;
        s->cqes       = reinterpret_cast<io_uring_cqe*>(base + p.cq_off.cqes);
        // Submission slot i always holds entry i
        auto* array = reinterpret_cast<uint32_t*>(base + p.sq_off.array);
        for (uint32_t i = 0; i < p.sq_entries; ++i)
          array[i] = i;

        s->event_fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (s->event_fd < 0)
        {
          e = errno;
          close_ring(s);
          s->~uring_queue();
          return fail(R::ASYNC_IO_QUEUE_CREATE_SYSTEM_ERROR, e);
        }
        arm_wake(s);
        return {};
      }

      void platform_async_io_destroy(async_io_queue& q) noexcept
      {
        uring_queue* s = state(q);
        // Closing the ring cancels the wake poll; nothing else is in flight
        close_ring(s);
        s->~uring_queue();
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
        // io_uring takes any descriptor: the attachment is bookkeeping
        // (async_io.cpp checks it in tracked builds)
        (void)q;
        (void)file;
        return {};
      }

      status<async_io_submit_result> platform_async_io_submit(
          async_io_queue& q, async_io_op* const* ops, size_t count,
          size_t* submitted) noexcept
      {
        using R        = async_io_submit_result;
        uring_queue* s = state(q);
        size_t i       = 0;
        for (; i < count && s->in_flight < s->capacity; ++i)
        {
          io_uring_sqe* sqe = get_sqe(s);
          if (sqe == nullptr)
          {
            *submitted = i;
            return fail(R::ASYNC_IO_SUBMIT_SYSTEM_ERROR, EBUSY);
          }
          async_io_op& op = *ops[i];
          sqe->opcode =
              op.kind == async_io_op_kind::ASYNC_IO_OP_READ ? OP_READ : OP_WRITE;
          sqe->fd   = op.file.native;
          sqe->off  = op.offset;
          sqe->addr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(op.buffer));
          sqe->len  = op.size;
          sqe->user_data = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&op));
          ++s->sq_tail;
          ++s->in_flight;
        }
        *submitted = i;
        if (i != 0)
        {
          const long r = enter(s, 0, 0, nullptr);
          if (r < 0 && !transient(r))
            return fail(R::ASYNC_IO_SUBMIT_SYSTEM_ERROR, static_cast<int>(-r));
        }
        return {};
      }

      status<async_io_wait_result> platform_async_io_wait(
          async_io_queue& q, async_io_op** out, size_t capacity, size_t min_count,
          awake_instant deadline, size_t* count) noexcept
      {
        using R        = async_io_wait_result;
        uring_queue* s = state(q);
        bool woken     = false;
        size_t n       = reap(s, out, capacity, 0, &woken);
        bool entered   = false;
        for (;;)
        {
          if (!s->wake_armed && !s->wake_broken)
            arm_wake(s);
          const bool need_more = n < min_count && !woken;
          if (!need_more)
          {
            // Nothing found yet: run deferred completions and hand queued
            // entries to the kernel once, without blocking
            if (n == 0 && !woken && !entered
                && (s->defer_taskrun || unsubmitted(s) != 0))
            {
              entered      = true;
              const long r = enter(s, 0, ENTER_GETEVENTS, nullptr);
              n            = reap(s, out, capacity, n, &woken);
              if (r < 0 && !transient(r))
              {
                *count = n;
                return fail(R::ASYNC_IO_WAIT_SYSTEM_ERROR, static_cast<int>(-r));
              }
              continue;
            }
            break;
          }

          getevents_arg arg{};
          kernel_timespec ts{};
          const getevents_arg* argp = nullptr;
          if (!deadline.is_forever())
          {
            const awake_instant now = awake_now();
            if (now >= deadline)
            {
              *count = n;
              return {
                  R::ASYNC_IO_WAIT_TIMED_OUT, native_error_kind::NATIVE_ERROR_NONE,
                  0};
            }
            const int64_t remaining = deadline.ns - now.ns; // > 0
            ts.tv_sec               = remaining / 1000000000;
            ts.tv_nsec              = remaining % 1000000000;
            arg.ts = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&ts));
            argp   = &arg;
          }
          const size_t missing = min_count - n;
          const uint32_t want  = missing > s->cq_entries
                                     ? s->cq_entries
                                     : static_cast<uint32_t>(missing);
          entered              = true;
          const long r         = enter(s, want, ENTER_GETEVENTS, argp);
          n                    = reap(s, out, capacity, n, &woken);
          if (r >= 0)
            continue; // collected, or internal completions only: check again
          if (r == -ETIME)
            continue; // satisfied meanwhile, TIMED_OUT, or an early timer: wait again
          if (transient(r))
            break; // an interruption: an early return
          *count = n;
          return fail(R::ASYNC_IO_WAIT_SYSTEM_ERROR, static_cast<int>(-r));
        }
        *count = n;
        return {};
      }

      void platform_async_io_cancel(async_io_queue& q, async_io_op& op) noexcept
      {
        uring_queue* s    = state(q);
        io_uring_sqe* sqe = get_sqe(s);
        if (sqe == nullptr)
          return; // best effort: the operation completes on its own
        sqe->opcode    = OP_ASYNC_CANCEL;
        sqe->fd        = -1;
        sqe->addr      = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&op));
        sqe->user_data = UD_CANCEL;
        ++s->sq_tail;
      }

      void platform_async_io_wake(async_io_queue& q) noexcept
      {
        uring_queue* s = state(q);
        if (s->wake_pending.exchange(1, std::memory_order_acq_rel) != 0)
          return; // a wake is already pending
        const uint64_t one = 1;
        ssize_t r          = 0;
        do
        {
          r = ::write(s->event_fd, &one, sizeof(one));
        } while (r < 0 && errno == EINTR);
        if (r < 0)
          s->wake_pending.store(0, std::memory_order_release);
      }
    } // namespace detail
  } // namespace v0_1
} // namespace osprim
