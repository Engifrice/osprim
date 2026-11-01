/*****************************************************************/ /**
 * @file   wait.hpp
 * @brief  Address-based wait and wake: the primitive every lock, pool and
 * run loop parks on (futex, WaitOnAddress, os_sync_wait_on_address).
 *
 * - wait() blocks while the word holds `expected`, until a wake, the
 *   deadline, or a spurious return. It does not loop: the caller re-checks
 *   its own condition after every return.
 * - The comparison with `expected` is atomic with respect to wakes: a
 *   wake issued after the word stopped holding `expected` is never missed.
 * - Neither function orders memory: the caller's own loads and stores
 *   (acquire/release) carry the synchronization.
 * - Words are private to the process.
 * - Deadlines are on the awake clock. A timeout is never reported before
 *   the deadline, but may be reported late: timer slack (Linux, 50 us by
 *   default), timer coalescing (Apple), the system timer resolution
 *   (Windows, 15.6 ms by default). Reducing lateness is a thread or power
 *   setting, not a wait parameter.
 *********************************************************************/
#ifndef OSPRIM_WAIT_HPP
#define OSPRIM_WAIT_HPP

#include <osprim/base.hpp>
#include <osprim/clock.hpp>

#include <stdint.h>

#include <atomic>

namespace osprim
{
  inline namespace v0_1
  {
    /// @brief The word the OS can wait on: uint32_t on every current target,
    /// may differ on future ones.
    using wait_word_t = uint32_t;

    // The OS reads the storage of the std::atomic directly
    static_assert(std::atomic<wait_word_t>::is_always_lock_free);
    static_assert(sizeof(std::atomic<wait_word_t>) == sizeof(wait_word_t));
    static_assert(alignof(std::atomic<wait_word_t>) == alignof(wait_word_t));

    /// @brief Result of wait(). Advisory: the caller re-checks its condition.
    enum class wait_result : uint8_t
    {
      /// @brief Woken, the word already differed from `expected`, or a
      /// spurious return. The word may still hold `expected`.
      WAIT_WOKEN,
      /// @brief The awake deadline has passed. The word may nevertheless
      /// have changed (a wake can race the deadline).
      WAIT_TIMED_OUT,
    };

    /// @brief Blocks while `word` holds `expected`, until woken or `deadline`.
    /// @param word The word to wait on
    /// @param expected Returns at once if the word does not hold this value
    /// @param deadline awake_instant::forever() to wait without deadline;
    /// a deadline already passed returns at once
    wait_result wait(
        const std::atomic<wait_word_t>& word, wait_word_t expected,
        awake_instant deadline) noexcept;

    /// @brief Wakes at least one thread blocked in wait() on `word`, if any.
    /// Always a system call: callers that care track their waiters.
    void wake_one(const std::atomic<wait_word_t>& word) noexcept;

    /// @brief Wakes every thread blocked in wait() on `word`.
    /// Always a system call: callers that care track their waiters.
    void wake_all(const std::atomic<wait_word_t>& word) noexcept;
  } // namespace v0_1
} // namespace osprim

#endif // !OSPRIM_WAIT_HPP
