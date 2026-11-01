/*****************************************************************/ /**
 * @file   thread.hpp
 * @brief  Threads: creation, timed join, detach, identity, names,
 * scheduling classes, yielding, spinning and precise sleeping.
 *
 * - A thread is described by a caller-owned thread_context: the library
 *   never allocates for a thread. The context must stay valid until
 *   thread_join() returns OK, or, for a detached thread, until its state
 *   reads THREAD_STATE_EXITED (the library never touches it afterwards).
 * - Settings (name, class) apply to the calling thread only: some
 *   platforms cannot change them on another thread. A thread sets its own,
 *   usually first thing in its entry function.
 * - New threads inherit some settings from their creator (the nice value
 *   on Linux and Android, the QoS class on Apple): a thread that cares
 *   sets its class explicitly.
 *********************************************************************/
#ifndef OSPRIM_THREAD_HPP
#define OSPRIM_THREAD_HPP

#include <osprim/base.hpp>
#include <osprim/clock.hpp>
#include <osprim/wait.hpp>

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <source_location>

#if defined(_MSC_VER) && !defined(__clang__)
  #include <intrin.h> // _mm_pause, __isb
#endif

namespace osprim
{
  inline namespace v0_1
  {
    /*****************************/
    /* LIFECYCLE                 */
    /*****************************/

    /// @brief Life of a thread, as seen through thread_context::state.
    enum class thread_state : wait_word_t
    {
      THREAD_STATE_STARTING, ///< Created, entry not called yet (id not valid yet)
      THREAD_STATE_RUNNING,  ///< Entry called (id valid)
      THREAD_STATE_EXITED, ///< Entry returned: the library no longer uses the context
    };

    /// @brief Caller-owned storage for one thread. The caller sets `entry`
    /// and `user`; the library sets everything else.
    struct thread_context
    {
      /// @brief Runs on the new thread. Must not throw.
      void (*entry)(void* user) noexcept = nullptr;
      /// @brief Passed to `entry`.
      void* user = nullptr;
      /// @brief A thread_state. Woken (wake_all) on every change, so it can
      /// be waited on.
      std::atomic<wait_word_t> state{0};
      /// @brief OS thread id (as current_thread_id() returns on that thread),
      /// valid once state is RUNNING or EXITED.
      uint64_t id = 0;
      /// @brief Native handle (pthread_t, HANDLE), valid from thread_create()
      /// to thread_join() or thread_detach().
      uint64_t native = 0;
    };

    /// @brief The state of `ctx` (acquire): RUNNING or EXITED makes `ctx.id`
    /// readable.
    inline thread_state thread_state_of(const thread_context& ctx) noexcept
    {
      return static_cast<thread_state>(ctx.state.load(std::memory_order_acquire));
    }

    /// @brief Parameters of thread_create().
    struct thread_desc
    {
      /// @brief Stack size in bytes, rounded up to the page size and the
      /// platform minimum. 0: the platform default, which differs a lot
      /// (512 KiB on Apple, 1 MiB on Windows, usually 8 MiB on Linux).
      size_t stack_size = 0;
    };

    /// @brief Result of thread_create().
    enum class thread_create_result : uint8_t
    {
      THREAD_CREATE_OK,
      THREAD_CREATE_OUT_OF_RESOURCES, ///< Thread or memory limit reached (EAGAIN, ...)
      THREAD_CREATE_SYSTEM_ERROR,     ///< Any other refusal (see native_code)
    };

    /// @brief Starts a thread running `ctx.entry(ctx.user)`.
    /// @pre `ctx.entry` is set, `ctx` is not in use by another thread
    status<thread_create_result> thread_create(
        thread_context& ctx, const thread_desc& desc,
        std::source_location loc = std::source_location::current()) noexcept;

    /// @brief Result of thread_join().
    enum class thread_join_result : uint8_t
    {
      THREAD_JOIN_OK,        ///< Exited and released: `ctx` may be reused or freed
      THREAD_JOIN_TIMED_OUT, ///< Still running at the deadline: `ctx` still in use
    };

    /// @brief Waits until the thread exits or `deadline`, then releases it.
    /// @pre Created, not joined or detached, and not the calling thread
    thread_join_result thread_join(
        thread_context& ctx, awake_instant deadline,
        std::source_location loc = std::source_location::current()) noexcept;

    /// @brief Releases the thread without waiting: it cleans up on its own.
    /// `ctx` must stay valid until its state reads THREAD_STATE_EXITED.
    /// @pre Created, not joined or detached
    void thread_detach(
        thread_context& ctx,
        std::source_location loc = std::source_location::current()) noexcept;

    /*****************************/
    /* CALLING THREAD            */
    /*****************************/

    /// @brief OS id of the calling thread (gettid, GetCurrentThreadId,
    /// pthread_threadid_np): what profilers and the OS performance APIs use.
    uint64_t current_thread_id() noexcept;

    /// @brief Longest thread name in bytes (UTF-8, without terminator): the
    /// Linux limit, enforced on every platform.
    inline constexpr size_t THREAD_NAME_MAX = 15;

    /// @brief Result of set_current_thread_name().
    enum class set_thread_name_result : uint8_t
    {
      SET_THREAD_NAME_OK,
      SET_THREAD_NAME_SYSTEM_ERROR, ///< Refused, or not valid UTF-8 on Windows
    };

    /// @brief Names the calling thread (shown by debuggers and profilers).
    /// @pre `name` is not null and at most THREAD_NAME_MAX bytes
    status<set_thread_name_result> set_current_thread_name(
        const char* name) noexcept;

    /// @brief Scheduling class of a thread. Mapping:
    ///
    /// | class       | Windows        | Apple QoS        | Linux/Android nice |
    /// |-------------|----------------|------------------|--------------------|
    /// | BACKGROUND  | LOWEST         | BACKGROUND       | 10                 |
    /// | UTILITY     | BELOW_NORMAL   | UTILITY          | 5                  |
    /// | NORMAL      | NORMAL         | USER_INITIATED   | 0                  |
    /// | INTERACTIVE | ABOVE_NORMAL   | USER_INTERACTIVE | -6                 |
    ///
    /// Desktop Linux refuses any decrease of the nice value to unprivileged
    /// users (RLIMIT_NICE): INTERACTIVE, and NORMAL after a lower class.
    enum class thread_class : uint8_t
    {
      THREAD_CLASS_BACKGROUND, ///< Work nobody waits for
      THREAD_CLASS_UTILITY, ///< Long-running work with a visible result (streaming, decompression)
      THREAD_CLASS_NORMAL,      ///< The default
      THREAD_CLASS_INTERACTIVE, ///< Frame-critical work (game, render threads)
    };

    /// @brief Result of set_current_thread_class().
    enum class set_thread_class_result : uint8_t
    {
      SET_THREAD_CLASS_OK,
      SET_THREAD_CLASS_DENIED, ///< Not allowed for this process (EPERM, EACCES, ...)
      SET_THREAD_CLASS_SYSTEM_ERROR, ///< Any other refusal (see native_code)
    };

    /// @brief Sets the scheduling class of the calling thread.
    status<set_thread_class_result> set_current_thread_class(
        thread_class c) noexcept;

    /// @brief Gives up the rest of the time slice (sched_yield, SwitchToThread).
    void yield_thread() noexcept;

    /// @brief Spin-wait hint, for the body of a spin loop: PAUSE on x86-64
    /// (about 10 cycles before Skylake, about 140 since: bound spin loops by
    /// time, not by iteration count), ISB on arm64 (YIELD is close to a
    /// no-op on current cores).
    inline void cpu_relax() noexcept
    {
#if defined(_MSC_VER) && !defined(__clang__)
  #if defined(_M_X64)
      _mm_pause();
  #else
      __isb(_ARM64_BARRIER_SY);
  #endif
#elif defined(__x86_64__)
      __builtin_ia32_pause();
#else
      __asm__ __volatile__("isb" ::: "memory");
#endif
    }

    /// @brief Sleeps until the awake clock reaches `deadline`: never returns
    /// before it, and as close after it as the platform allows (absolute
    /// clock_nanosleep, mach_wait_until, high-resolution waitable timer).
    /// A deadline already passed returns at once.
    /// @pre `deadline` is not awake_instant::forever()
    void sleep_until(awake_instant deadline) noexcept;
  } // namespace v0_1
} // namespace osprim

#endif // !OSPRIM_THREAD_HPP
