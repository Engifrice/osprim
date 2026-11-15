/*****************************************************************/ /**
 * @file   async_io.hpp
 * @brief  Asynchronous reads and writes of regular files, where the kernel
 * performs them: I/O completion ports (Windows) and io_uring (Linux).
 *
 * Which function do I need?
 *
 * | Goal                                          | Use                         |
 * |-----------------------------------------------|-----------------------------|
 * | Know at compile time if the platform has it   | ASYNC_IO_AVAILABLE          |
 * | Create a queue (and know if the OS allows it) | async_io_queue_create()     |
 * | Let a file be used with a queue               | async_io_queue_attach()     |
 * | Start reads and writes                        | async_io_queue_submit()     |
 * | Collect finished operations, with a deadline  | async_io_queue_wait()       |
 * | Stop an operation early                       | async_io_queue_cancel()     |
 * | Wake the queue's owner from another thread    | async_io_queue_wake()       |
 *
 * osprim offers asynchronous I/O only where the kernel performs it, and
 * never emulates it with threads.
 *
 * | Platform       | ASYNC_IO_AVAILABLE | Backend                | Can still fail at run time   |
 * |----------------|--------------------|------------------------|------------------------------|
 * | Windows        | true               | I/O completion port    | no                           |
 * | Linux          | true               | io_uring (5.11+)       | disabled, too old, memlock   |
 * | Android        | false              | none: the app seccomp  | -                            |
 * |                |                    | filter forbids io_uring|                              |
 * | Apple          | false              | none: no kernel async  | -                            |
 * |                |                    | file I/O worth using   |                              |
 *
 * Where ASYNC_IO_AVAILABLE is false, every function is still declared (so
 * `if constexpr` works outside templates): async_io_queue_create() returns
 * ASYNC_IO_QUEUE_CREATE_UNSUPPORTED without making any system call, and no
 * other function may be called, since no queue can exist.
 *
 * Rules:
 * - Regular files only, opened by file_open() or file_open_at(), or
 *   adopted (Windows: adopted handles must be overlapped). Reads and
 *   writes only: flush, size changes and opening stay synchronous file_
 *   calls, because no Windows backend performs them asynchronously.
 * - Operations (async_io_op) are owned by the caller: the library never
 *   allocates. An operation must stay valid and must not move from
 *   async_io_queue_submit() until async_io_queue_wait() returns it. So
 *   must its buffer, which belongs to the kernel in between.
 * - Every submitted operation completes exactly once, through
 *   async_io_queue_wait(): success, failure and cancellation alike. Even an
 *   operation that fails or finishes during submission arrives this way.
 * - A queue has one owner thread, the only one that submits, waits,
 *   cancels and attaches. async_io_queue_wake() is the one function any
 *   thread may call.
 * - A queue holds at most `capacity` operations in flight (submitted and
 *   not yet returned by a wait), on every platform.
 * - Attaching is permanent: a file is attached to at most one queue for
 *   the rest of its life, and asynchronous operations on it go through that
 *   queue only. Blocking calls (file_read_at(), ...) keep working on an
 *   attached file. This is the Windows rule, applied everywhere.
 * - Buffered and unbuffered files both work. Unbuffered files follow
 *   file_unbuffered_alignment() as for file_read_at() (checked in tracked
 *   builds at submission), and are the most predictable: reads and writes
 *   go straight to the device instead of through the OS cache.
 * - Submission can block (the operation runs to completion inside
 *   async_io_queue_submit(); it still completes through the queue):
 *   - Windows: writes that extend a file (whatever the size set by
 *     file_set_size(): the valid data length grows synchronously, and
 *     moving it needs a privilege games do not have), and NTFS-compressed
 *     or encrypted files. Reads served from the OS cache complete at once.
 *   - Linux: rarely; io_uring hands work that would block to kernel
 *     workers (buffered writes on most filesystems: at most two at a time
 *     per file, which take the inode lock anyway).
 *   Writing new files (saves, caches) is therefore not asynchronous on
 *   Windows: run those writes on an engine thread.
 * - Deadlines are on the awake clock. A timeout is never reported before
 *   the deadline, but may be reported late (see wait.hpp).
 *********************************************************************/
#ifndef OSPRIM_ASYNC_IO_HPP
#define OSPRIM_ASYNC_IO_HPP

#include <osprim/base.hpp>
#include <osprim/clock.hpp>
#include <osprim/file.hpp>
#include <osprim/platform.hpp>

#include <stddef.h>
#include <stdint.h>

#include <source_location>

namespace osprim
{
  inline namespace v0_1
  {
    /*****************************/
    /* AVAILABILITY              */
    /*****************************/

    /// @brief True where the kernel offers asynchronous file I/O to
    /// applications (Windows, Linux). False on Android and Apple: there,
    /// async_io_queue_create() returns UNSUPPORTED without a system call.
    inline constexpr bool ASYNC_IO_AVAILABLE =
        OSPRIM_OS_WINDOWS != 0 || OSPRIM_OS_LINUX != 0;

    /*****************************/
    /* STORAGE SIZES             */
    /*****************************/

#if OSPRIM_OS_WINDOWS
    /// @brief Bytes of backend storage in an async_io_op: an OVERLAPPED.
    inline constexpr size_t ASYNC_IO_OP_NATIVE_SIZE = 32;
    /// @brief Bytes of backend storage in an async_io_queue: the port,
    /// counters and the wake flag.
    inline constexpr size_t ASYNC_IO_QUEUE_NATIVE_SIZE = 64;
#elif OSPRIM_OS_LINUX
    /// @brief Bytes of backend storage in an async_io_op: unused (io_uring
    /// keeps everything in the rings), kept so the layout has the member.
    inline constexpr size_t ASYNC_IO_OP_NATIVE_SIZE = 8;
    /// @brief Bytes of backend storage in an async_io_queue: the ring
    /// descriptor, its mappings, the wake eventfd and counters.
    inline constexpr size_t ASYNC_IO_QUEUE_NATIVE_SIZE = 256;
#else
    /// @brief No backend: minimal storage.
    inline constexpr size_t ASYNC_IO_OP_NATIVE_SIZE = 8;
    /// @brief No backend: minimal storage.
    inline constexpr size_t ASYNC_IO_QUEUE_NATIVE_SIZE = 8;
#endif

    /*****************************/
    /* QUEUES                    */
    /*****************************/

    /// @brief Most operations a queue may hold in flight.
    inline constexpr uint32_t ASYNC_IO_CAPACITY_MAX = 16384;

    /// @brief A queue, owned by the caller. Holds no valid value until a
    /// successful async_io_queue_create(). Must not be copied or moved
    /// until async_io_queue_destroy(): the kernel and the library keep
    /// pointers into it.
    struct async_io_queue
    {
      alignas(8) unsigned char native[ASYNC_IO_QUEUE_NATIVE_SIZE];
    };

    /// @brief Parameters of async_io_queue_create().
    struct async_io_queue_desc
    {
      /// @brief Most operations in flight, 1 to ASYNC_IO_CAPACITY_MAX.
      /// Internal entries (wakes, cancellations) are reserved on top.
      uint32_t capacity = 256;
    };

    /// @brief Result of async_io_queue_create().
    enum class async_io_queue_create_result : uint8_t
    {
      ASYNC_IO_QUEUE_CREATE_OK,
      ASYNC_IO_QUEUE_CREATE_UNSUPPORTED, ///< ASYNC_IO_AVAILABLE is false (no system call made)
      ASYNC_IO_QUEUE_CREATE_DISABLED, ///< Linux: refused by policy (io_uring_disabled sysctl, container seccomp: EPERM)
      ASYNC_IO_QUEUE_CREATE_TOO_OLD, ///< Linux: no io_uring (ENOSYS), or a kernel before 5.11 (feature probe)
      ASYNC_IO_QUEUE_CREATE_OUT_OF_MEMORY, ///< Linux before 5.12: rings count against RLIMIT_MEMLOCK
      ASYNC_IO_QUEUE_CREATE_SYSTEM_ERROR, ///< Any other refusal (see native_code)
    };

    /// @brief Creates a queue. The calling thread becomes its owner.
    /// Linux: never crashes, except under a seccomp policy that kills on
    /// io_uring_setup (custom sandboxes); Android is never asked.
    /// @pre 1 <= `desc.capacity` <= ASYNC_IO_CAPACITY_MAX
    OSPRIM_API status<async_io_queue_create_result> async_io_queue_create(
        const async_io_queue_desc& desc, async_io_queue& out,
        std::source_location loc = std::source_location::current()) noexcept;

    /// @brief Destroys the queue. Files attached to it stay open and keep
    /// working with blocking calls, but can never be attached again.
    /// @pre Created; nothing in flight (cancel and collect first); called
    /// by the owner, with no async_io_queue_wake() in progress
    OSPRIM_API void async_io_queue_destroy(
        async_io_queue& q,
        std::source_location loc = std::source_location::current()) noexcept;

    /*****************************/
    /* ATTACHING FILES           */
    /*****************************/

    /// @brief Result of async_io_queue_attach().
    enum class async_io_attach_result : uint8_t
    {
      ASYNC_IO_ATTACH_OK,
      ASYNC_IO_ATTACH_NOT_OVERLAPPED, ///< Windows: an adopted handle opened for synchronous I/O
      ASYNC_IO_ATTACH_SYSTEM_ERROR, ///< Any other refusal (see native_code)
    };

    /// @brief Attaches `file` to `q` for the rest of the file's life
    /// (CreateIoCompletionPort; Linux: bookkeeping only).
    /// @pre Called by the owner; `file` open and attached to no queue,
    /// this one included (checked in tracked builds on every platform);
    /// Windows: an adopted handle never given
    /// FILE_SKIP_COMPLETION_PORT_ON_SUCCESS (its operations finishing
    /// during submission would never complete)
    OSPRIM_API status<async_io_attach_result> async_io_queue_attach(
        async_io_queue& q, file_handle file,
        std::source_location loc = std::source_location::current()) noexcept;

    /*****************************/
    /* OPERATIONS                */
    /*****************************/

    /// @brief What an operation does.
    enum class async_io_op_kind : uint8_t
    {
      ASYNC_IO_OP_READ,
      ASYNC_IO_OP_WRITE,
    };

    /// @brief Result of one operation, written at completion.
    enum class async_io_op_result : uint8_t
    {
      ASYNC_IO_OP_OK, ///< `transferred` < `size`: READ at the end of the file, WRITE when space ran out
      ASYNC_IO_OP_CANCELED, ///< async_io_queue_cancel() stopped it (`transferred` may be > 0)
      ASYNC_IO_OP_NO_SPACE,       ///< WRITE: disk full or quota reached
      ASYNC_IO_OP_FILE_TOO_LARGE, ///< WRITE: above the filesystem's file size limit
      ASYNC_IO_OP_IO_ERROR,       ///< The device failed
      ASYNC_IO_OP_MISALIGNED, ///< Unbuffered file and unaligned offset, size or buffer
      ASYNC_IO_OP_SYSTEM_ERROR, ///< Any other refusal (see native_code)
    };

    /// @brief One read or write, owned by the caller. The caller fills the
    /// request fields before submitting; the library writes `result` and
    /// `transferred` at completion, and owns `native` while in flight.
    /// The same record may be submitted again once returned by a wait.
    struct async_io_op
    {
      // Backend (first: its alignment then needs no padding)

      /// @brief Backend storage (Windows: the OVERLAPPED).
      alignas(8) unsigned char native[ASYNC_IO_OP_NATIVE_SIZE];

      // Request

      /// @brief The file, attached to the queue the operation goes to.
      file_handle file;
      /// @brief Byte offset. Reading at or past the end of the file
      /// completes OK with `transferred` 0; writing past it grows the file
      /// (the gap reads as zero).
      uint64_t offset;
      /// @brief Destination (READ) or source (WRITE, not modified). Owned
      /// by the kernel until the operation completes.
      void* buffer;
      /// @brief Bytes to transfer, at most FILE_IO_MAX.
      uint32_t size;
      async_io_op_kind kind;
      /// @brief Free for the caller: not read or written by the library.
      void* user;

      // Completion

      /// @brief How the operation ended (valid once returned by a wait).
      status<async_io_op_result> result;
      /// @brief Bytes transferred (valid once returned by a wait).
      uint32_t transferred;
    };

    /*****************************/
    /* SUBMISSION                */
    /*****************************/

    /// @brief Result of async_io_queue_submit().
    enum class async_io_submit_result : uint8_t
    {
      ASYNC_IO_SUBMIT_OK, ///< `submitted` < `count` only when the queue reached its capacity
      ASYNC_IO_SUBMIT_SYSTEM_ERROR, ///< The queue refused further work (see native_code)
    };

    /// @brief Starts `ops[0..count)` in order, with a single system call
    /// where the backend allows it (io_uring; Windows: one call per
    /// operation). Stops at the queue's capacity. The failure of one
    /// operation is reported through its completion, never here.
    /// On return, `ops[0..submitted)` are in flight and complete through
    /// async_io_queue_wait(); the others were not started and stay the
    /// caller's. May block (see the rules above).
    /// @pre Called by the owner; each op not in flight, its file attached
    /// to `q`, `size` <= FILE_IO_MAX, `offset` + `size` <= INT64_MAX,
    /// unbuffered files aligned (checked in tracked builds)
    OSPRIM_API status<async_io_submit_result> async_io_queue_submit(
        async_io_queue& q, async_io_op* const* ops, size_t count, size_t& submitted,
        std::source_location loc = std::source_location::current()) noexcept;

    /*****************************/
    /* COMPLETION                */
    /*****************************/

    /// @brief Result of async_io_queue_wait().
    enum class async_io_wait_result : uint8_t
    {
      /// @brief At least `min_count` operations collected, or an early
      /// return: async_io_queue_wake(), or an interruption. `count` may
      /// then be below `min_count`, even 0: the caller re-checks its own
      /// state, as with wait().
      ASYNC_IO_WAIT_OK,
      /// @brief The awake deadline has passed with fewer than `min_count`
      /// collected. `count` may still be above 0: process those.
      ASYNC_IO_WAIT_TIMED_OUT,
      /// @brief The queue failed (see native_code). `count` operations were
      /// still collected.
      ASYNC_IO_WAIT_SYSTEM_ERROR,
    };

    /// @brief Collects up to `capacity` completed operations into `out`,
    /// waiting until at least `min_count` have completed, a wake, or
    /// `deadline`. `min_count` 0 collects what is ready without blocking.
    /// Does not loop: returns early on a wake or an interruption.
    /// Operations written to `out` are no longer in flight and belong to
    /// the caller again, with `result` and `transferred` set.
    /// @param deadline awake_instant::forever() to wait without deadline
    /// @pre Called by the owner; `min_count` <= `capacity`
    OSPRIM_API status<async_io_wait_result> async_io_queue_wait(
        async_io_queue& q, async_io_op** out, size_t capacity, size_t min_count,
        awake_instant deadline, size_t& count) noexcept;

    /*****************************/
    /* CANCELLATION AND WAKING   */
    /*****************************/

    /// @brief Asks the kernel to stop `op` (CancelIoEx,
    /// IORING_OP_ASYNC_CANCEL). Best effort: `op` still completes exactly
    /// once, CANCELED or with its real result if it finished first.
    /// Linux: the request is batched and reaches the kernel with the next
    /// async_io_queue_submit() or async_io_queue_wait().
    /// @pre Called by the owner; `op` in flight on `q`, and not cancelled
    /// already
    OSPRIM_API void async_io_queue_cancel(
        async_io_queue& q, async_io_op& op) noexcept;

    /// @brief Makes the owner's current or next async_io_queue_wait()
    /// return early (OK). Wakes coalesce: several before a wait make one
    /// early return. Never lost: a wake sent while the owner is not
    /// waiting makes its next wait return at once. Callable from any
    /// thread; a system call unless a wake is already pending.
    /// @pre Created, and not being destroyed
    OSPRIM_API void async_io_queue_wake(async_io_queue& q) noexcept;
  } // namespace v0_1
} // namespace osprim

#endif // !OSPRIM_ASYNC_IO_HPP
