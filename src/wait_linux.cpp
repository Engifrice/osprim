/*****************************************************************/ /**
 * @file   wait_linux.cpp
 * @brief  Linux and Android wait/wake on private futexes.
 *
 * FUTEX_WAIT_BITSET takes an absolute CLOCK_MONOTONIC deadline: the awake
 * clock itself, so a timeout reported by the kernel is never early and
 * retries keep the same deadline without any conversion.
 *********************************************************************/
#include "internal.hpp"

#if !(OSPRIM_OS_LINUX || OSPRIM_OS_ANDROID)
  #error "wait_linux.cpp is for Linux and Android only"
#endif

#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

namespace osprim
{
  inline namespace v0_1
  {
    wait_result wait(
        const std::atomic<wait_word_t>& word, wait_word_t expected,
        awake_instant deadline) noexcept
    {
      struct timespec ts;
      const struct timespec* timeout = nullptr;
      if (!deadline.is_forever())
      {
        // The kernel would arm a timer and switch threads (~25 us) even
        // for a deadline that has passed: one clock read (~40 ns) avoids it
        if (awake_now() >= deadline)
          return wait_result::WAIT_TIMED_OUT;
        ts.tv_sec  = static_cast<time_t>(deadline.ns / 1000000000);
        ts.tv_nsec = static_cast<long>(deadline.ns % 1000000000);
        timeout    = &ts;
      }
      const long rc = ::syscall(
          SYS_futex, detail::wait_address(word), FUTEX_WAIT_BITSET_PRIVATE, expected,
          timeout, nullptr, FUTEX_BITSET_MATCH_ANY);
      if (rc == -1 && errno == ETIMEDOUT)
        return wait_result::WAIT_TIMED_OUT;
      // 0: woken. EAGAIN: the word differed. EINTR: a signal.
      return wait_result::WAIT_WOKEN;
    }

    void wake_one(const std::atomic<wait_word_t>& word) noexcept
    {
      ::syscall(
          SYS_futex, detail::wait_address(word), FUTEX_WAKE_PRIVATE, 1, nullptr,
          nullptr, 0);
    }

    void wake_all(const std::atomic<wait_word_t>& word) noexcept
    {
      ::syscall(
          SYS_futex, detail::wait_address(word), FUTEX_WAKE_PRIVATE, INT_MAX,
          nullptr, nullptr, 0);
    }
  } // namespace v0_1
} // namespace osprim
