/*****************************************************************/ /**
 * @file   wait_windows.cpp
 * @brief  Windows wait/wake on WaitOnAddress (Windows 8+).
 *
 * WaitOnAddress only takes a relative timeout in milliseconds: the remaining
 * time is rounded up, and a timeout is reported only once the awake clock
 * has reached the deadline (otherwise the caller simply waits again).
 *********************************************************************/
#include "internal.hpp"

#if !OSPRIM_OS_WINDOWS
  #error "wait_windows.cpp is for Windows only"
#endif

#include "win32.hpp"

#if defined(_MSC_VER)
  #pragma comment(lib, "synchronization.lib")
#endif

namespace osprim
{
  inline namespace v0_1
  {
    wait_result wait(
        const std::atomic<wait_word_t>& word, wait_word_t expected,
        awake_instant deadline) noexcept
    {
      DWORD timeout_ms = INFINITE;
      if (!deadline.is_forever())
      {
        const awake_instant now = awake_now();
        if (now >= deadline)
          return wait_result::WAIT_TIMED_OUT;
        const int64_t remaining = deadline.ns - now.ns; // > 0, no overflow
        const int64_t ms = remaining / 1000000 + (remaining % 1000000 != 0 ? 1 : 0);
        // INFINITE is 0xFFFFFFFF: stay just below (about 49.7 days)
        timeout_ms = ms >= static_cast<int64_t>(INFINITE) ? INFINITE - 1
                                                          : static_cast<DWORD>(ms);
      }

      wait_word_t compare = expected;
      if (::WaitOnAddress(
              detail::wait_address(word), &compare, sizeof(wait_word_t), timeout_ms))
        return wait_result::WAIT_WOKEN; // woken, the word differed, or spurious
      if (::GetLastError() == ERROR_TIMEOUT && awake_now() >= deadline)
        return wait_result::WAIT_TIMED_OUT;
      return wait_result::WAIT_WOKEN;
    }

    void wake_one(const std::atomic<wait_word_t>& word) noexcept
    {
      ::WakeByAddressSingle(detail::wait_address(word));
    }

    void wake_all(const std::atomic<wait_word_t>& word) noexcept
    {
      ::WakeByAddressAll(detail::wait_address(word));
    }
  } // namespace v0_1
} // namespace osprim
