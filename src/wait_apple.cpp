/*****************************************************************/ /**
 * @file   wait_apple.cpp
 * @brief  Apple wait/wake on os_sync_wait_on_address (public since
 * macOS 14.4, iOS/tvOS 17.4, watchOS 10.4, visionOS 1.1).
 *
 * The deadline is passed on OS_CLOCK_MACH_ABSOLUTE_TIME, the clock behind
 * awake_now(), converted back to mach ticks rounding up.
 *********************************************************************/
#include "internal.hpp"

#if !OSPRIM_OS_APPLE
  #error "wait_apple.cpp is for Apple platforms only"
#endif

#include <TargetConditionals.h>

#if defined(__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__)
  #if __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ < 140400
    #error \
        "osprim requires macOS 14.4 or later (os_sync_wait_on_address): set CMAKE_OSX_DEPLOYMENT_TARGET"
  #endif
#elif defined(__ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__)
  #if __ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__ < 170400
    #error \
        "osprim requires iOS 17.4 or later (os_sync_wait_on_address): set CMAKE_OSX_DEPLOYMENT_TARGET"
  #endif
#elif defined(__ENVIRONMENT_TV_OS_VERSION_MIN_REQUIRED__)
  #if __ENVIRONMENT_TV_OS_VERSION_MIN_REQUIRED__ < 170400
    #error \
        "osprim requires tvOS 17.4 or later (os_sync_wait_on_address): set CMAKE_OSX_DEPLOYMENT_TARGET"
  #endif
#endif

#include <errno.h>
#include <mach/mach_time.h>
#include <os/os_sync_wait_on_address.h>

namespace osprim
{
  inline namespace v0_1
  {
    wait_result wait(
        const std::atomic<wait_word_t>& word, wait_word_t expected,
        awake_instant deadline) noexcept
    {
      void* const addr = detail::wait_address(word);
      uint64_t ticks   = 0;
      int rc;
      if (deadline.is_forever())
        rc = ::os_sync_wait_on_address(
            addr, expected, sizeof(wait_word_t), OS_SYNC_WAIT_ON_ADDRESS_NONE);
      else if (awake_now() >= deadline) // no system call for a passed deadline
        return wait_result::WAIT_TIMED_OUT;
      else if (!detail::awake_ns_to_mach_ceil(deadline.ns, &ticks)) // centuries away
        rc = ::os_sync_wait_on_address(
            addr, expected, sizeof(wait_word_t), OS_SYNC_WAIT_ON_ADDRESS_NONE);
      else
      {
        rc = ::os_sync_wait_on_address_with_deadline(
            addr, expected, sizeof(wait_word_t), OS_SYNC_WAIT_ON_ADDRESS_NONE,
            OS_CLOCK_MACH_ABSOLUTE_TIME, ticks);
      }
      // rc >= 0: woken or the word differed. EINTR, EFAULT: spurious.
      if (rc < 0 && errno == ETIMEDOUT && awake_now() >= deadline)
        return wait_result::WAIT_TIMED_OUT;
      return wait_result::WAIT_WOKEN;
    }

    void wake_one(const std::atomic<wait_word_t>& word) noexcept
    {
      // ENOENT when nobody waits: not an error here
      ::os_sync_wake_by_address_any(
          detail::wait_address(word), sizeof(wait_word_t),
          OS_SYNC_WAKE_BY_ADDRESS_NONE);
    }

    void wake_all(const std::atomic<wait_word_t>& word) noexcept
    {
      ::os_sync_wake_by_address_all(
          detail::wait_address(word), sizeof(wait_word_t),
          OS_SYNC_WAKE_BY_ADDRESS_NONE);
    }
  } // namespace v0_1
} // namespace osprim
