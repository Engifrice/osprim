/*****************************************************************/ /**
 * @file   clock.hpp
 * @brief  Clocks: awake, continuous, wall, and a raw tick counter.
 *
 * - awake: monotonic, stops while the system is suspended. Every
 *   deadline in osprim (waits, sleeps, I/O timeouts) is an awake_instant.
 * - continuous: monotonic, keeps counting while the system is suspended.
 * - wall: UTC since the Unix epoch. Can jump. Never used for waits.
 * - ticks: the cheapest monotonic counter, in native units, for
 *   measurement only. Never a deadline.
 *
 * Values and durations are signed 64-bit nanoseconds (int64_t), like
 * std::chrono::nanoseconds. Ticks are unsigned (raw hardware counters).
 * FOREVER_NS as a deadline means "no deadline".
 * Every function is thread-safe, lock-free and never allocates.
 *********************************************************************/
#ifndef OSPRIM_CLOCK_HPP
#define OSPRIM_CLOCK_HPP

#include <osprim/base.hpp>

#include <stdint.h>

#include <compare>

namespace osprim
{
  inline namespace v0_1
  {
    /// @brief As a deadline: none. As a timeout: forever.
    inline constexpr int64_t FOREVER_NS = INT64_MAX;

    /*****************************/
    /* INSTANTS                  */
    /*****************************/

    /// @brief A point on the awake clock (ns from an arbitrary origin, never negative).
    struct awake_instant
    {
      int64_t ns;

      /// @brief The instant meaning "no deadline"
      static constexpr awake_instant forever() noexcept { return {FOREVER_NS}; }
      /// @brief True if this means "no deadline"
      constexpr bool is_forever() const noexcept { return ns == FOREVER_NS; }
      /// @brief `*this + d` (d may be negative), saturating to forever()
      /// above and to INT64_MIN below
      constexpr awake_instant after(int64_t d) const noexcept
      {
        if (is_forever() || (d > 0 && d >= FOREVER_NS - ns))
          return forever();
        if (d < 0 && ns < INT64_MIN - d)
          return {INT64_MIN};
        return {ns + d};
      }
      /// @brief Signed nanoseconds until this deadline seen from `now`:
      /// negative if overdue (saturating at INT64_MIN), FOREVER_NS if no
      /// deadline. Never FOREVER_NS for a finite deadline.
      constexpr int64_t remaining(awake_instant now) const noexcept
      {
        if (is_forever())
          return FOREVER_NS;
        if (now.ns > 0 && ns < INT64_MIN + now.ns)
          return INT64_MIN;
        if (now.ns < 0 && ns > FOREVER_NS - 1 + now.ns)
          return FOREVER_NS - 1;
        return ns - now.ns;
      }
      friend constexpr auto operator<=>(awake_instant, awake_instant) noexcept =
          default;
    };

    /// @brief A point on the continuous clock (ns from an arbitrary origin, never negative).
    struct continuous_instant
    {
      int64_t ns;
      friend constexpr auto operator<=>(
          continuous_instant, continuous_instant) noexcept = default;
    };

    /// @brief A point on the wall clock (ns since 1970-01-01 00:00:00 UTC, may be negative).
    struct wall_instant
    {
      int64_t ns;
      friend constexpr auto operator<=>(wall_instant, wall_instant) noexcept =
          default;
    };

    /// @brief `end - start` in nanoseconds (negative if `end` is before `start`).
    constexpr int64_t elapsed_ns(awake_instant start, awake_instant end) noexcept
    {
      return end.ns - start.ns;
    }
    /// @copydoc elapsed_ns(awake_instant, awake_instant)
    constexpr int64_t elapsed_ns(
        continuous_instant start, continuous_instant end) noexcept
    {
      return end.ns - start.ns;
    }

    /*****************************/
    /* READING                   */
    /*****************************/

    /// @brief Current awake time. Never decreases, on any thread.
    OSPRIM_API awake_instant awake_now() noexcept;
    /// @brief Current continuous time. Never decreases, on any thread.
    OSPRIM_API continuous_instant continuous_now() noexcept;
    /// @brief Current wall time as reported by the OS (may jump).
    OSPRIM_API wall_instant wall_now() noexcept;

    /// @brief The deadline `d` nanoseconds from now. FOREVER_NS: no deadline.
    /// Zero or negative: a deadline that has already passed.
    inline awake_instant deadline_after(int64_t d) noexcept
    {
      return d >= FOREVER_NS ? awake_instant::forever() : awake_now().after(d);
    }

    /*****************************/
    /* CLOCK INFORMATION         */
    /*****************************/

    /// @brief Identifies a clock.
    enum class clock_id : uint8_t
    {
      CLOCK_ID_AWAKE,
      CLOCK_ID_CONTINUOUS,
      CLOCK_ID_WALL,
      CLOCK_ID_TICKS,
    };

    /// @brief Granularity of the readings of a clock in nanoseconds
    /// (the smallest non-zero difference two readings can show).
    /// This is not the precision of timers or waits.
    OSPRIM_API int64_t clock_resolution_ns(clock_id c) noexcept;

    /*****************************/
    /* TICKS                     */
    /*****************************/

    /// @brief Where ticks come from.
    enum class tick_source : uint8_t
    {
      TICK_SOURCE_AWAKE,   ///< Fallback: ticks are awake nanoseconds (1 GHz, exact)
      TICK_SOURCE_X86_TSC, ///< Invariant TSC (RDTSC), frequency calibrated
      TICK_SOURCE_ARM64_CNTVCT, ///< CNTVCT_EL0, frequency reported by the hardware
    };

    /// @brief Describes the tick counter. Constant after initialize().
    struct ticks_info
    {
      uint64_t frequency_hz;
      tick_source source;
      /// @brief True if reported by the hardware/OS, false if calibrated
      bool frequency_exact;
    };

    /// @brief A tick reading and an awake reading taken as close together as possible.
    struct ticks_sample
    {
      uint64_t ticks;
      awake_instant awake;
    };

    /// @brief Reads the tick counter. Monotonic on a given thread; readings
    /// on different threads may show a small skew on some systems.
    /// No fence: the read is not ordered with surrounding memory operations
    /// (the CPU may execute it slightly earlier or later), so it is meant for
    /// measuring durations, not for ordering events across threads.
    OSPRIM_API uint64_t ticks_now() noexcept;
    /// @brief Describes the tick counter.
    OSPRIM_API ticks_info get_ticks_info() noexcept;
    /// @brief Converts ticks (usually a difference) to nanoseconds, saturating at INT64_MAX.
    OSPRIM_API int64_t ticks_to_ns(uint64_t ticks) noexcept;
    /// @brief Two samples far apart give the exact tick frequency over that
    /// interval and map ticks onto the awake clock.
    OSPRIM_API ticks_sample sample_ticks() noexcept;
  } // namespace v0_1
} // namespace osprim

#endif // !OSPRIM_CLOCK_HPP
