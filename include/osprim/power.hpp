/*****************************************************************/ /**
 * @file   power.hpp
 * @brief  Power and timing: tell the OS what the program needs where its
 * own guesses (window focus, user input, idle CPU) are wrong.
 *
 * Which feature do I need?
 *
 * | Goal                                         | Use                              | Scope   |
 * |----------------------------------------------|----------------------------------|---------|
 * | Wake up on time for the next frame           | sleep_until() (thread.hpp)       | thread  |
 * | Precise timeouts on waits (~1 ms, not 15 ms) | process_timer_resolution_begin() | process |
 * | Keep unimportant work off the fast cores     | THREAD_CLASS_BACKGROUND          | thread  |
 * | The user is here though the OS sees no input | process_activity_begin()         | process |
 * | Meet a frame deadline efficiently on mobile  | perf_session_create()            | threads |
 * | Lower quality before the device overheats    | current_thermal_level(), watch   | process |
 *
 * Rules:
 * - One feature per goal: osprim picks the mechanism of each platform.
 * - Results tell whether the goal is met: OK means in effect (whether
 *   osprim had to act or the platform already behaves that way);
 *   UNSUPPORTED means this platform cannot provide it and nothing changed.
 * - The scope is in the name: process_ affects the whole program.
 * - Nothing happens unless asked: initialize() changes no power setting.
 *   Requests are handles, tracked like every other handle.
 * - get_power_features() tells once which goals this device supports.
 *********************************************************************/
#ifndef OSPRIM_POWER_HPP
#define OSPRIM_POWER_HPP

#include <osprim/base.hpp>
#include <osprim/clock.hpp>
#include <osprim/wait.hpp>

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <source_location>

namespace osprim
{
  inline namespace v0_1
  {
    /*****************************/
    /* FEATURES                  */
    /*****************************/

    /// @brief Which goals this device supports. Constant after initialize().
    /// Precise timeouts (process_timer_resolution_begin) are met everywhere.
    struct power_features
    {
      bool activity_latency_critical;  ///< process_activity_begin: latency_critical
      bool activity_keep_system_awake; ///< process_activity_begin: keep_system_awake
      bool activity_keep_display_on;   ///< process_activity_begin: keep_display_on
      bool perf_session;     ///< perf_session_create (may still refuse a session)
      bool thermal_level;    ///< current_thermal_level
      bool thermal_watch;    ///< thermal_watch_begin
      bool thermal_headroom; ///< thermal_headroom
    };

    /// @brief The goals this device supports.
    OSPRIM_API power_features get_power_features() noexcept;

    /*****************************/
    /* TIMER RESOLUTION          */
    /*****************************/

    /// @brief A timer resolution request. Trivial, released by
    /// process_timer_resolution_end().
    struct timer_resolution_handle
    {
      uint64_t id;
      uint32_t period_ms; ///< The period requested from the OS (Windows)
    };

    /// @brief Result of process_timer_resolution_begin().
    enum class timer_resolution_result : uint8_t
    {
      TIMER_RESOLUTION_OK,
      TIMER_RESOLUTION_SYSTEM_ERROR, ///< The OS refused (see native_code)
    };

    /// @brief Makes timed waits of the whole process precise to about
    /// `period_ns` (wait(), thread_join() deadlines, ...) while held.
    ///
    /// Use when: short timeouts must not be late by a whole system tick
    ///   (15.6 ms by default on Windows), during gameplay or on a server.
    /// Don't use when: pacing frames (sleep_until() is already precise,
    ///   at no cost), or in menus, paused or minimized: it costs battery.
    /// Platforms:
    /// - Windows: timeBeginPeriod(ceil(period) in ms, clamped to what the
    ///   system supports). Windows 11 ignores it while the process's window
    ///   is minimized, occluded and silent, unless an activity with
    ///   latency_critical is held.
    /// - Linux, Android, Apple: timers are already precise (Linux within
    ///   50 us of timer slack): OK, nothing to do.
    /// @pre period_ns > 0
    OSPRIM_API status<timer_resolution_result> process_timer_resolution_begin(
        int64_t period_ns, timer_resolution_handle* out,
        std::source_location loc = std::source_location::current()) noexcept;

    /// @brief Releases a request of process_timer_resolution_begin().
    OSPRIM_API void process_timer_resolution_end(
        timer_resolution_handle h,
        std::source_location loc = std::source_location::current()) noexcept;

    /*****************************/
    /* PROCESS ACTIVITY          */
    /*****************************/

    /// @brief What an activity asks for. Also what was granted.
    struct activity_desc
    {
      /// @brief Do not throttle the process even if the OS believes nobody
      /// is using it: no App Nap or timer coalescing (macOS), explicit
      /// high QoS and timer resolution honored while occluded (Windows).
      bool latency_critical;
      /// @brief Do not let the machine go to sleep when idle.
      bool keep_system_awake;
      /// @brief Do not let the display dim or turn off when idle.
      bool keep_display_on;
      /// @brief Shown by the OS power tools (powercfg /requests,
      /// pmset -g assertions): static UTF-8, at most ACTIVITY_REASON_MAX bytes.
      const char* reason;
    };

    /// @brief Longest activity reason in bytes.
    inline constexpr size_t ACTIVITY_REASON_MAX = 127;

    /// @brief A process activity. Trivial, released by process_activity_end().
    struct activity_handle
    {
      uint64_t id;
      uint64_t native;       ///< Power request (Windows), activity token (macOS)
      activity_desc granted; ///< What is in effect (reason: null)
    };

    /// @brief Result of process_activity_begin().
    enum class activity_result : uint8_t
    {
      ACTIVITY_OK,           ///< At least one request granted: see `granted`
      ACTIVITY_UNSUPPORTED,  ///< Nothing requested is available here: no handle
      ACTIVITY_SYSTEM_ERROR, ///< The OS refused (see native_code): no handle
    };

    /// @brief Tells the OS the user is engaged with the process although
    /// it may see no input, while held.
    ///
    /// Use when: controller-only play, long cutscenes (no input),
    ///   dedicated servers (no window), downloads the user waits for.
    /// Don't use when: the game is paused, in a menu nobody looks at, or
    ///   minimized: it costs battery and keeps the machine awake.
    /// Platforms:
    /// - Windows: all three (process power throttling, power requests).
    /// - macOS: all three (NSProcessInfo activity).
    /// - iOS, Android, Linux: none here (UNSUPPORTED). Keeping the screen
    ///   on is a window setting on mobile: the engine's UI layer does it.
    /// @pre At least one request set; `reason` not null, at most
    /// ACTIVITY_REASON_MAX bytes
    OSPRIM_API status<activity_result> process_activity_begin(
        const activity_desc& desc, activity_handle* out,
        std::source_location loc = std::source_location::current()) noexcept;

    /// @brief Ends an activity of process_activity_begin().
    OSPRIM_API void process_activity_end(
        activity_handle h,
        std::source_location loc = std::source_location::current()) noexcept;

    /*****************************/
    /* PERFORMANCE SESSIONS      */
    /*****************************/

    /// @brief Most threads in a performance session.
    inline constexpr size_t PERF_SESSION_MAX_THREADS = 32;

    /// @brief A performance session. Trivial, closed by perf_session_close().
    /// Its functions are not thread-safe: one thread at a time per session.
    struct perf_session
    {
      uint64_t id;
      uint64_t native;
    };

    /// @brief Result of the perf_session functions.
    enum class perf_session_result : uint8_t
    {
      PERF_SESSION_OK,
      PERF_SESSION_UNSUPPORTED,  ///< No performance hints here: no session
      PERF_SESSION_SYSTEM_ERROR, ///< The OS refused (see native_code)
    };

    /// @brief Asks the OS to adjust CPU frequency and placement so that a
    /// periodic workload (a frame) of these threads takes `target_ns`.
    ///
    /// Use when: a mobile game produces frames on a few long-lived threads
    ///   (main, render): report each frame's time with perf_session_report().
    /// Don't use when: threads are short-lived, or the work is not periodic.
    /// Platforms:
    /// - Android 13+ on devices that support hint sessions (ADPF).
    /// - Everywhere else: UNSUPPORTED.
    /// @param thread_ids OS ids (thread_context::id, current_thread_id()) of
    /// long-lived threads of this process
    /// @pre 1 <= count <= PERF_SESSION_MAX_THREADS, target_ns > 0
    OSPRIM_API status<perf_session_result> perf_session_create(
        const uint64_t* thread_ids, size_t count, int64_t target_ns,
        perf_session* out,
        std::source_location loc = std::source_location::current()) noexcept;

    /// @brief Changes the target duration of each cycle.
    /// @pre target_ns > 0
    OSPRIM_API status<perf_session_result> perf_session_set_target(
        perf_session s, int64_t target_ns) noexcept;

    /// @brief Reports how long the last cycle took (awake clock), once per cycle.
    /// @pre actual_ns > 0
    OSPRIM_API status<perf_session_result> perf_session_report(
        perf_session s, int64_t actual_ns) noexcept;

    /// @brief Closes a session of perf_session_create().
    OSPRIM_API void perf_session_close(
        perf_session s,
        std::source_location loc = std::source_location::current()) noexcept;

    /*****************************/
    /* THERMAL                   */
    /*****************************/

    /// @brief How hot the device is, from the point of view of throttling.
    enum class thermal_level : uint8_t
    {
      THERMAL_LEVEL_NOMINAL, ///< No throttling
      THERMAL_LEVEL_FAIR, ///< Throttling starts, little visible effect: cut optional work
      THERMAL_LEVEL_SERIOUS,  ///< Performance visibly reduced: lower quality now
      THERMAL_LEVEL_CRITICAL, ///< The system is doing everything it can: minimum load
    };

    /// @brief Result of the thermal functions.
    enum class thermal_result : uint8_t
    {
      THERMAL_OK,
      THERMAL_UNSUPPORTED, ///< Not available on this platform or device
      THERMAL_UNAVAILABLE, ///< No value right now (headroom: asked more than ~once a second, or warming up)
      THERMAL_SYSTEM_ERROR, ///< The OS refused (see native_code)
    };

    /// @brief The current thermal level.
    ///
    /// Use when: deciding whether to lower resolution, effects or frame rate
    ///   before the system throttles the CPU and GPU abruptly.
    /// Platforms:
    /// - Android 11+: thermal status (LIGHT, MODERATE: FAIR; SEVERE:
    ///   SERIOUS; CRITICAL and above: CRITICAL).
    /// - Apple (macOS, iOS): NSProcessInfo thermalState, the same four levels.
    /// - Windows, Linux: UNSUPPORTED.
    OSPRIM_API status<thermal_result> current_thermal_level(
        thermal_level* out) noexcept;

    /// @brief A thermal watch. Trivial, ended by thermal_watch_end().
    struct thermal_watch
    {
      uint64_t id;
      uint64_t native;
      int32_t token;
    };

    /// @brief Increments `counter` and wakes its waiters (wake_all) every
    /// time the thermal level changes, until thermal_watch_end(). Wait on
    /// it, then read current_thermal_level(): no OS callback runs engine code.
    ///
    /// Use when: reacting to thermal changes without polling.
    /// Platforms: as current_thermal_level(). Apple: at most 16 watches at
    /// once (THERMAL_SYSTEM_ERROR, ENOSPC beyond).
    /// @pre `counter` outlives the watch; one watch per counter
    OSPRIM_API status<thermal_result> thermal_watch_begin(
        std::atomic<wait_word_t>& counter, thermal_watch* out,
        std::source_location loc = std::source_location::current()) noexcept;

    /// @brief Ends a watch of thermal_watch_begin(): once it returns,
    /// the counter is no longer touched.
    OSPRIM_API void thermal_watch_end(
        thermal_watch w,
        std::source_location loc = std::source_location::current()) noexcept;

    /// @brief Forecast of the thermal headroom `forecast_ns` from now:
    /// 0 is no throttling at all, 1 is the point where severe throttling
    /// starts (may exceed 1).
    ///
    /// Use when: lowering the load ahead of throttling, not after it.
    /// Don't use when: asking more than about once a second (UNAVAILABLE).
    /// Platforms: Android 12+. Everywhere else: UNSUPPORTED.
    /// @pre 0 <= forecast_ns
    OSPRIM_API status<thermal_result> thermal_headroom(
        int64_t forecast_ns, float* out) noexcept;
  } // namespace v0_1
} // namespace osprim

#endif // !OSPRIM_POWER_HPP
