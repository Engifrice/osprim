/*****************************************************************/ /**
 * @file   power_apple.cpp
 * @brief  Apple power and timing.
 *
 * - Timer resolution: already precise: nothing to do.
 * - Activity (macOS): NSProcessInfo activities, called through the
 *   Objective-C runtime's C functions (no Objective-C source needed).
 *   iOS has no equivalent here (App Nap does not exist there, and the
 *   display idle timer is a UIKit setting): UNSUPPORTED.
 * - Thermal (macOS, iOS): NSProcessInfo thermalState, which has exactly
 *   the four levels of thermal_level, and its change notification.
 * - Performance sessions, headroom: none (UNSUPPORTED).
 *********************************************************************/
#include "internal.hpp"

#if !OSPRIM_OS_APPLE
  #error "power_apple.cpp is for Apple platforms only"
#endif

#include <TargetConditionals.h>
#include <errno.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <sched.h>

#include <atomic>

/// @brief NSProcessInfoThermalStateDidChangeNotification (an NSString*),
/// declared here so that no Objective-C header is needed.
extern "C" id const NSProcessInfoThermalStateDidChangeNotification;

#if TARGET_OS_OSX
  #define OSPRIM_APPLE_ACTIVITY 1
#else
  #define OSPRIM_APPLE_ACTIVITY 0
#endif

namespace osprim
{
  inline namespace v0_1
  {
    namespace
    {
      /// @brief objc_msgSend, cast to the exact signature of the method.
      template<class R, class... A>
      R send(id self, const char* selector, A... args) noexcept
      {
        using fn = R (*)(id, SEL, A...);
        return reinterpret_cast<fn>(&objc_msgSend)(
            self, sel_registerName(selector), args...);
      }

      id class_named(const char* name) noexcept
      {
        return reinterpret_cast<id>(objc_getClass(name));
      }

      /// @brief An autorelease pool for the duration of a call (the calling
      /// thread may have none).
      struct autorelease_pool
      {
        id pool;
        autorelease_pool() noexcept
            : pool(
                  send<id>(
                      send<id>(class_named("NSAutoreleasePool"), "alloc"), "init"))
        {
        }
        ~autorelease_pool() { send<void>(pool, "drain"); }
        autorelease_pool(const autorelease_pool&)            = delete;
        autorelease_pool& operator=(const autorelease_pool&) = delete;
      };

#if OSPRIM_APPLE_ACTIVITY
      // NSActivityOptions (NSProcessInfo.h)
      constexpr uint64_t NS_ACTIVITY_IDLE_DISPLAY_SLEEP_DISABLED    = 1ull << 40;
      constexpr uint64_t NS_ACTIVITY_IDLE_SYSTEM_SLEEP_DISABLED     = 1ull << 20;
      constexpr uint64_t NS_ACTIVITY_SUDDEN_TERMINATION_DISABLED    = 1ull << 14;
      constexpr uint64_t NS_ACTIVITY_AUTOMATIC_TERMINATION_DISABLED = 1ull << 15;
      constexpr uint64_t NS_ACTIVITY_USER_INITIATED =
          0x00FFFFFFull | NS_ACTIVITY_IDLE_SYSTEM_SLEEP_DISABLED;
      constexpr uint64_t NS_ACTIVITY_USER_INITIATED_ALLOWING_IDLE_SYSTEM_SLEEP =
          NS_ACTIVITY_USER_INITIATED & ~NS_ACTIVITY_IDLE_SYSTEM_SLEEP_DISABLED;
      constexpr uint64_t NS_ACTIVITY_LATENCY_CRITICAL = 0xFF00000000ull;
#endif

      /*****************************/
      /* THERMAL WATCHES           */
      /*****************************/

      // A notification can be in the middle of being delivered while its
      // observer is removed, and NSNotificationCenter does not wait for it.
      // So handlers never hold the counter themselves: they find it in this
      // table, under a lock, by slot and generation. Once
      // thermal_watch_end() has cleared the slot, a late handler finds
      // nothing, and the counter is no longer touched.

      constexpr size_t MAX_THERMAL_WATCHES = 16;

      struct watch_slot
      {
        std::atomic<wait_word_t>* counter;
        uint32_t generation;
      };

      watch_slot g_watches[MAX_THERMAL_WATCHES]{};
      std::atomic_flag g_watches_lock; // clear (C++20)

      struct watches_lock
      {
        watches_lock() noexcept
        {
          while (g_watches_lock.test_and_set(std::memory_order_acquire))
            while (g_watches_lock.test(std::memory_order_relaxed))
              ::sched_yield();
        }
        ~watches_lock() { g_watches_lock.clear(std::memory_order_release); }
        watches_lock(const watches_lock&)            = delete;
        watches_lock& operator=(const watches_lock&) = delete;
      };

      void notify_watch(size_t slot, uint32_t generation) noexcept
      {
        watches_lock lock;
        watch_slot& w = g_watches[slot];
        if (w.counter == nullptr || w.generation != generation)
          return; // ended
        w.counter->fetch_add(1, std::memory_order_release);
        wake_all(*w.counter);
      }

      /// @brief NSProcessInfoThermalState: Nominal, Fair, Serious, Critical
      thermal_level map_thermal_state(long state) noexcept
      {
        switch (state)
        {
        case 0:
          return thermal_level::THERMAL_LEVEL_NOMINAL;
        case 1:
          return thermal_level::THERMAL_LEVEL_FAIR;
        case 2:
          return thermal_level::THERMAL_LEVEL_SERIOUS;
        default:
          return thermal_level::THERMAL_LEVEL_CRITICAL;
        }
      }
    } // namespace

    namespace detail
    {
      void platform_power_initialize(power_features* out) noexcept
      {
        const bool foundation = objc_getClass("NSProcessInfo") != nullptr;
#if OSPRIM_APPLE_ACTIVITY
        out->activity_latency_critical  = foundation;
        out->activity_keep_system_awake = foundation;
        out->activity_keep_display_on   = foundation;
#endif
        // thermalState: macOS 10.10.3, iOS 11: always there from our minimums
        out->thermal_level = foundation;
        out->thermal_watch = foundation;
      }

      void platform_power_deinitialize() noexcept
      {
      }

      status<timer_resolution_result> platform_timer_resolution_begin(
          int64_t, uint32_t* period_ms) noexcept
      {
        *period_ms = 0; // already precise
        return {};
      }

      void platform_timer_resolution_end(uint32_t) noexcept
      {
      }

      status<activity_result> platform_activity_begin(
          const activity_desc& desc, uint64_t* native,
          activity_desc* granted) noexcept
      {
#if OSPRIM_APPLE_ACTIVITY
        if (!g_power_features.activity_latency_critical)
          return {
              activity_result::ACTIVITY_UNSUPPORTED,
              native_error_kind::NATIVE_ERROR_NONE, 0};
        uint64_t options = 0;
        if (desc.latency_critical)
          options |= NS_ACTIVITY_USER_INITIATED_ALLOWING_IDLE_SYSTEM_SLEEP
                     | NS_ACTIVITY_LATENCY_CRITICAL;
        if (desc.keep_system_awake)
          options |= NS_ACTIVITY_IDLE_SYSTEM_SLEEP_DISABLED;
        if (desc.keep_display_on)
          options |= NS_ACTIVITY_IDLE_DISPLAY_SLEEP_DISABLED;
        // Never change how the process may be terminated
        options &=
            ~(NS_ACTIVITY_SUDDEN_TERMINATION_DISABLED
              | NS_ACTIVITY_AUTOMATIC_TERMINATION_DISABLED);

        autorelease_pool pool;
        const id info   = send<id>(class_named("NSProcessInfo"), "processInfo");
        const id reason = send<id>(
            send<id>(class_named("NSString"), "alloc"),
            "initWithUTF8String:", desc.reason);
        const id token =
            send<id>(info, "beginActivityWithOptions:reason:", options, reason);
        if (token != nullptr)
          send<id>(token, "retain"); // released by platform_activity_end()
        send<void>(reason, "release");
        if (token == nullptr)
          return {
              activity_result::ACTIVITY_SYSTEM_ERROR,
              native_error_kind::NATIVE_ERROR_NONE, 0};

        *native  = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(token));
        *granted = desc;
        return {};
#else
        (void)desc;
        (void)native;
        (void)granted;
        return {
            activity_result::ACTIVITY_UNSUPPORTED,
            native_error_kind::NATIVE_ERROR_NONE, 0};
#endif
      }

      void platform_activity_end(uint64_t native, const activity_desc&) noexcept
      {
#if OSPRIM_APPLE_ACTIVITY
        autorelease_pool pool;
        const id token = reinterpret_cast<id>(static_cast<uintptr_t>(native));
        const id info  = send<id>(class_named("NSProcessInfo"), "processInfo");
        send<void>(info, "endActivity:", token);
        send<void>(token, "release");
#else
        (void)native;
#endif
      }

      status<perf_session_result> platform_perf_session_create(
          const uint64_t*, size_t, int64_t, uint64_t*) noexcept
      {
        return {
            perf_session_result::PERF_SESSION_UNSUPPORTED,
            native_error_kind::NATIVE_ERROR_NONE, 0};
      }

      status<perf_session_result> platform_perf_session_set_target(
          uint64_t, int64_t) noexcept
      {
        return {
            perf_session_result::PERF_SESSION_UNSUPPORTED,
            native_error_kind::NATIVE_ERROR_NONE, 0};
      }

      status<perf_session_result> platform_perf_session_report(
          uint64_t, int64_t) noexcept
      {
        return {
            perf_session_result::PERF_SESSION_UNSUPPORTED,
            native_error_kind::NATIVE_ERROR_NONE, 0};
      }

      void platform_perf_session_close(uint64_t) noexcept
      {
      }

      status<thermal_result> platform_thermal_level(thermal_level* out) noexcept
      {
        if (!g_power_features.thermal_level)
          return {
              thermal_result::THERMAL_UNSUPPORTED,
              native_error_kind::NATIVE_ERROR_NONE, 0};
        autorelease_pool pool;
        const id info = send<id>(class_named("NSProcessInfo"), "processInfo");
        *out          = map_thermal_state(send<long>(info, "thermalState"));
        return {};
      }

      status<thermal_result> platform_thermal_watch_begin(
          std::atomic<wait_word_t>& counter, uint64_t* native,
          int32_t* token) noexcept
      {
        if (!g_power_features.thermal_watch)
          return {
              thermal_result::THERMAL_UNSUPPORTED,
              native_error_kind::NATIVE_ERROR_NONE, 0};

        size_t slot         = MAX_THERMAL_WATCHES;
        uint32_t generation = 0;
        {
          watches_lock lock;
          for (size_t i = 0; i < MAX_THERMAL_WATCHES; ++i)
          {
            if (g_watches[i].counter == nullptr)
            {
              slot                 = i;
              g_watches[i].counter = &counter;
              generation           = ++g_watches[i].generation;
              break;
            }
          }
        }
        if (slot == MAX_THERMAL_WATCHES)
          return {
              thermal_result::THERMAL_SYSTEM_ERROR,
              native_error_kind::NATIVE_ERROR_POSIX_ERRNO, ENOSPC};

        autorelease_pool pool;
        const id center =
            send<id>(class_named("NSNotificationCenter"), "defaultCenter");
        // A nil queue: delivered on the posting thread. The center copies
        // the block and keeps the returned observer until it is removed.
        const id observer = send<id>(
            center, "addObserverForName:object:queue:usingBlock:",
            NSProcessInfoThermalStateDidChangeNotification, static_cast<id>(nullptr),
            static_cast<id>(nullptr), ^(id) {
              notify_watch(slot, generation);
            });
        if (observer == nullptr)
        {
          watches_lock lock;
          g_watches[slot].counter = nullptr;
          return {
              thermal_result::THERMAL_SYSTEM_ERROR,
              native_error_kind::NATIVE_ERROR_NONE, 0};
        }
        *native = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(observer));
        *token  = static_cast<int32_t>(slot);
        return {};
      }

      void platform_thermal_watch_end(uint64_t native, int32_t token) noexcept
      {
        {
          autorelease_pool pool;
          const id center =
              send<id>(class_named("NSNotificationCenter"), "defaultCenter");
          send<void>(
              center, "removeObserver:",
              reinterpret_cast<id>(static_cast<uintptr_t>(native)));
        }
        // A delivery still in flight finds the slot empty
        watches_lock lock;
        g_watches[static_cast<size_t>(token)].counter = nullptr;
      }

      status<thermal_result> platform_thermal_headroom(int64_t, float*) noexcept
      {
        return {
            thermal_result::THERMAL_UNSUPPORTED,
            native_error_kind::NATIVE_ERROR_NONE, 0};
      }
    } // namespace detail
  } // namespace v0_1
} // namespace osprim
