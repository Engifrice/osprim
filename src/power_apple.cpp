/*****************************************************************/ /**
 * @file   power_apple.cpp
 * @brief  Apple power and timing.
 *
 * - Timer resolution: already precise: nothing to do.
 * - Activity (macOS): NSProcessInfo activities, called through the
 *   Objective-C runtime's C functions (no Objective-C source needed).
 *   iOS has no equivalent here (App Nap does not exist there, and the
 *   display idle timer is a UIKit setting): UNSUPPORTED.
 * - Thermal: the thermal pressure level, published by the system through
 *   the notify(3) C API (kOSThermalNotificationPressureLevelName).
 * - Performance sessions, headroom: none (UNSUPPORTED).
 *********************************************************************/
#include "internal.hpp"

#if !OSPRIM_OS_APPLE
  #error "power_apple.cpp is for Apple platforms only"
#endif

#include <TargetConditionals.h>
#include <dispatch/dispatch.h>
#include <libkern/OSThermalNotification.h>
#include <notify.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <string.h>

#if TARGET_OS_OSX
  #define OSPRIM_APPLE_ACTIVITY 1
#else
  #define OSPRIM_APPLE_ACTIVITY 0
#endif

#if TARGET_OS_OSX || (defined(TARGET_OS_MACCATALYST) && TARGET_OS_MACCATALYST)
  #define OSPRIM_APPLE_THERMAL_LIGHT 0 // macOS has no Light pressure level
#else
  #define OSPRIM_APPLE_THERMAL_LIGHT 1
#endif

namespace osprim
{
  inline namespace v0_1
  {
    namespace
    {
      /// @brief Token of notify_register_check for reading the pressure level.
      int g_thermal_token  = 0;
      bool g_thermal_valid = false;

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
#endif

      thermal_level map_pressure(uint64_t level) noexcept
      {
        if (level == static_cast<uint64_t>(kOSThermalPressureLevelNominal))
          return thermal_level::THERMAL_LEVEL_NOMINAL;
#if OSPRIM_APPLE_THERMAL_LIGHT
        if (level == static_cast<uint64_t>(kOSThermalPressureLevelLight))
          return thermal_level::THERMAL_LEVEL_FAIR;
#endif
        if (level == static_cast<uint64_t>(kOSThermalPressureLevelModerate))
          return thermal_level::THERMAL_LEVEL_FAIR;
        if (level == static_cast<uint64_t>(kOSThermalPressureLevelHeavy))
          return thermal_level::THERMAL_LEVEL_SERIOUS;
        return thermal_level::THERMAL_LEVEL_CRITICAL; // Trapping, Sleeping
      }

      void drain_noop(void*) noexcept
      {
      }
    } // namespace

    namespace detail
    {
      void platform_power_initialize(power_features* out) noexcept
      {
#if OSPRIM_APPLE_ACTIVITY
        const bool has_activity         = objc_getClass("NSProcessInfo") != nullptr;
        out->activity_latency_critical  = has_activity;
        out->activity_keep_system_awake = has_activity;
        out->activity_keep_display_on   = has_activity;
#endif
        g_thermal_valid =
            ::notify_register_check(
                kOSThermalNotificationPressureLevelName, &g_thermal_token)
            == NOTIFY_STATUS_OK;
        out->thermal_level = g_thermal_valid;
        out->thermal_watch = g_thermal_valid;
      }

      void platform_power_deinitialize() noexcept
      {
        if (g_thermal_valid)
          ::notify_cancel(g_thermal_token);
        g_thermal_valid = false;
        g_thermal_token = 0;
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
        if (!g_thermal_valid)
          return {
              thermal_result::THERMAL_UNSUPPORTED,
              native_error_kind::NATIVE_ERROR_NONE, 0};
        uint64_t level   = 0;
        const uint32_t r = ::notify_get_state(g_thermal_token, &level);
        if (r != NOTIFY_STATUS_OK)
          return {
              thermal_result::THERMAL_SYSTEM_ERROR,
              native_error_kind::NATIVE_ERROR_NONE, static_cast<int32_t>(r)};
        *out = map_pressure(level);
        return {};
      }

      status<thermal_result> platform_thermal_watch_begin(
          std::atomic<wait_word_t>& counter, uint64_t* native,
          int32_t* token) noexcept
      {
        if (!g_thermal_valid)
          return {
              thermal_result::THERMAL_UNSUPPORTED,
              native_error_kind::NATIVE_ERROR_NONE, 0};
        // A private serial queue: ending the watch drains it, so no handler
        // runs once thermal_watch_end() returns
        dispatch_queue_t queue =
            ::dispatch_queue_create("osprim.thermal_watch", DISPATCH_QUEUE_SERIAL);
        if (queue == nullptr)
          return {
              thermal_result::THERMAL_SYSTEM_ERROR,
              native_error_kind::NATIVE_ERROR_NONE, 0};
        std::atomic<wait_word_t>* c = &counter;
        int t                       = 0;
        const uint32_t r            = ::notify_register_dispatch(
            kOSThermalNotificationPressureLevelName, &t, queue, ^(int) {
              c->fetch_add(1, std::memory_order_release);
              wake_all(*c);
            });
        if (r != NOTIFY_STATUS_OK)
        {
          ::dispatch_release(queue);
          return {
              thermal_result::THERMAL_SYSTEM_ERROR,
              native_error_kind::NATIVE_ERROR_NONE, static_cast<int32_t>(r)};
        }
        *native = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(queue));
        *token  = t;
        return {};
      }

      void platform_thermal_watch_end(uint64_t native, int32_t token) noexcept
      {
        ::notify_cancel(token);
        const dispatch_queue_t queue =
            reinterpret_cast<dispatch_queue_t>(static_cast<uintptr_t>(native));
        // Wait for a handler already queued or running
        ::dispatch_sync_f(queue, nullptr, &drain_noop);
        ::dispatch_release(queue);
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
