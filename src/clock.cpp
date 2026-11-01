/*****************************************************************/ /**
 * @file   clock.cpp
 * @brief  Platform-independent clock code: tick counter selection and
 * calibration, tick conversion, resolutions.
 *
 * The awake, continuous and wall clocks are read by the platform sources
 * (clock_linux.cpp, clock_windows.cpp, clock_apple.cpp).
 *********************************************************************/
#include "internal.hpp"

#if OSPRIM_ARCH_X86_64 && !defined(_MSC_VER)
  #include <cpuid.h>
#endif

namespace osprim
{
  inline namespace v0_1
  {
    namespace detail
    {
      clock_state g_clock{};
    } // namespace detail

    namespace
    {
      using detail::g_clock;

      constexpr uint64_t NS_PER_S = 1000000000;

      /// @brief Selects the tick counter and derives the conversion factor.
      void set_ticks(tick_source source, uint64_t frequency_hz, bool exact) noexcept
      {
        g_clock.ticks_source          = source;
        g_clock.ticks_frequency_hz    = frequency_hz;
        g_clock.ticks_frequency_exact = exact;
        const detail::ticks_conversion conv =
            detail::make_ticks_conversion(frequency_hz);
        g_clock.ticks_mult  = conv.mult;
        g_clock.ticks_shift = conv.shift;
        // Duration of one tick, rounded up, at least 1 ns
        const uint64_t res = (NS_PER_S + frequency_hz - 1) / frequency_hz;
        g_clock.resolution_ns[static_cast<size_t>(clock_id::CLOCK_ID_TICKS)] =
            static_cast<int64_t>(res == 0 ? 1 : res);
      }

#if OSPRIM_ARCH_X86_64
      /// @brief CPUID.80000007H:EDX[8]: the TSC runs at a constant rate in
      /// every P-, C- and T-state.
      bool has_invariant_tsc() noexcept
      {
  #if defined(_MSC_VER)
        int regs[4];
        __cpuid(regs, static_cast<int>(0x80000000u));
        if (static_cast<unsigned>(regs[0]) < 0x80000007u)
          return false;
        __cpuid(regs, static_cast<int>(0x80000007u));
        return ((static_cast<unsigned>(regs[3]) >> 8) & 1u) != 0;
  #else
        unsigned a = 0, b = 0, c = 0, d = 0;
        // __get_cpuid checks the highest supported extended leaf itself
        if (__get_cpuid(0x80000007u, &a, &b, &c, &d) == 0)
          return false;
        return ((d >> 8) & 1u) != 0;
  #endif
      }

      struct tsc_pair
      {
        uint64_t ticks;
        int64_t ns;
      };

      /// @brief A TSC reading bracketed by the tightest awake window of a few tries.
      tsc_pair tight_tsc_pair() noexcept
      {
        tsc_pair best{};
        int64_t best_window = INT64_MAX;
        for (int i = 0; i < 5; ++i)
        {
          const int64_t a0     = awake_now().ns;
          const uint64_t t     = detail::read_hardware_ticks();
          const int64_t a1     = awake_now().ns;
          const int64_t window = a1 - a0;
          if (window < best_window)
          {
            best_window = window;
            best        = tsc_pair{t, a0 + window / 2};
          }
        }
        return best;
      }

      /// @brief Measures the TSC frequency against the awake clock.
      /// @return 0 on failure
      uint64_t calibrate_tsc() noexcept
      {
        // ~10 ms: with windows of ~100 ns at each end, the error is ~10 ppm
        constexpr int64_t CALIBRATION_NS = 10'000'000;
        const tsc_pair p0                = tight_tsc_pair();
        detail::platform_sleep_ns(CALIBRATION_NS);
        const tsc_pair p1 = tight_tsc_pair();
        // The sleep must have been seen by the awake clock (a coarse or
        // stalled clock would give a meaningless frequency)
        if (p1.ns - p0.ns < CALIBRATION_NS / 2 || p1.ticks <= p0.ticks)
          return 0;
        const double hz = static_cast<double>(p1.ticks - p0.ticks) * 1e9
                          / static_cast<double>(p1.ns - p0.ns);
        if (!(hz >= 1.0) || hz >= 1e12)
          return 0;
        return static_cast<uint64_t>(hz + 0.5);
      }
#endif // OSPRIM_ARCH_X86_64

#if OSPRIM_ARCH_ARM64
      /// @brief Frequency of CNTVCT_EL0.
      /// @return 0 if unknown
      uint64_t cntvct_frequency() noexcept
      {
  #if OSPRIM_OS_APPLE
        // mach_absolute_time() counts CNTVCT_EL0 ticks: derive the exact
        // frequency from the timebase (125/3 on Apple silicon: 24 MHz).
        const uint64_t num = NS_PER_S * g_clock.mach_denom;
        if (num % g_clock.mach_numer != 0)
          return 0;
        return num / g_clock.mach_numer;
  #elif defined(_MSC_VER)
        return static_cast<uint64_t>(_ReadStatusReg(OSPRIM_ARM64_CNTFRQ_EL0))
               & 0xFFFFFFFFu;
  #else
        uint64_t v;
        __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(v));
        return v & 0xFFFFFFFFu; // the upper 32 bits are reserved
  #endif
      }
#endif // OSPRIM_ARCH_ARM64
    } // namespace

    namespace detail
    {
      ticks_conversion make_ticks_conversion(uint64_t hz) noexcept
      {
        OSPRIM_ASSERT(hz != 0, "make_ticks_conversion: zero frequency");
        // 1e9 * 2^s = (q * hz + r) * 2^k with k = s - 32, computed in two
        // steps because 1e9 * 2^s does not fit in 64 bits (NS_PER_S << 32
        // is below 2^62).
        const uint64_t q = (NS_PER_S << 32) / hz;
        const uint64_t r = (NS_PER_S << 32) % hz;
        for (uint32_t s = 63; s > 32; --s)
        {
          const uint32_t k = s - 32;
          // q << k, plus up to 2^k, must not overflow
          if (q >= (uint64_t{1} << (64 - k)) - 1)
            continue;
          // r << k must not overflow (r < hz)
          if ((r >> (64 - k)) != 0)
            continue;
          const uint64_t rk = r << k;
          const uint64_t m  = (q << k) + rk / hz + (rk % hz != 0 ? 1 : 0);
          return ticks_conversion{m, s};
        }
        return ticks_conversion{q + (r != 0 ? 1 : 0), 32};
      }

      status<initialize_result> clock_initialize() noexcept
      {
        g_clock                           = clock_state{};
        const status<initialize_result> s = platform_clock_initialize();
        if (!s.ok())
          return s;

        // Fallback first: calibration below reads the awake clock
        set_ticks(tick_source::TICK_SOURCE_AWAKE, NS_PER_S, true);
#if OSPRIM_ARCH_X86_64
        if (has_invariant_tsc())
        {
          const uint64_t hz = calibrate_tsc();
          if (hz != 0)
            set_ticks(tick_source::TICK_SOURCE_X86_TSC, hz, false);
        }
#elif OSPRIM_ARCH_ARM64
        const uint64_t hz = cntvct_frequency();
        if (hz != 0)
          set_ticks(tick_source::TICK_SOURCE_ARM64_CNTVCT, hz, true);
#endif
        return {};
      }
    } // namespace detail

    int64_t clock_resolution_ns(clock_id c) noexcept
    {
      OSPRIM_ASSERT(
          static_cast<size_t>(c) <= static_cast<size_t>(clock_id::CLOCK_ID_TICKS),
          "osprim::clock_resolution_ns: invalid clock_id");
      return g_clock.resolution_ns[static_cast<size_t>(c)];
    }

    uint64_t ticks_now() noexcept
    {
      if (g_clock.ticks_source != tick_source::TICK_SOURCE_AWAKE) [[likely]]
        return detail::read_hardware_ticks();
      return static_cast<uint64_t>(awake_now().ns);
    }

    ticks_info get_ticks_info() noexcept
    {
      return ticks_info{
          g_clock.ticks_frequency_hz, g_clock.ticks_source,
          g_clock.ticks_frequency_exact};
    }

    int64_t ticks_to_ns(uint64_t ticks) noexcept
    {
      uint64_t hi;
      const uint64_t lo    = detail::mul_64x64(ticks, g_clock.ticks_mult, &hi);
      const uint32_t shift = g_clock.ticks_shift; // in [32, 63]
      // (hi:lo) >> shift fits in int64_t only if hi < 2^(shift - 1)
      if ((hi >> (shift - 1)) != 0)
        return INT64_MAX;
      return static_cast<int64_t>((hi << (64 - shift)) | (lo >> shift));
    }

    ticks_sample sample_ticks() noexcept
    {
      ticks_sample best{};
      int64_t best_window = INT64_MAX;
      for (int i = 0; i < 3; ++i)
      {
        const awake_instant a0 = awake_now();
        const uint64_t t       = ticks_now();
        const awake_instant a1 = awake_now();
        const int64_t window   = a1.ns - a0.ns;
        if (window < best_window)
        {
          best_window = window;
          best        = ticks_sample{t, awake_instant{a0.ns + window / 2}};
        }
      }
      return best;
    }
  } // namespace v0_1
} // namespace osprim
