/*****************************************************************/ /**
 * @file   internal.hpp
 * @brief  Internal: state and helpers shared by the library sources.
 *
 * Not installed. The tests include it to exercise the tracking registry
 * directly.
 *********************************************************************/
#ifndef OSPRIM_SRC_INTERNAL_HPP
#define OSPRIM_SRC_INTERNAL_HPP

#include "platform.hpp"

#include <osprim/base.hpp>
#include <osprim/clock.hpp>
#include <osprim/power.hpp>
#include <osprim/thread.hpp>
#include <osprim/wait.hpp>

#include <stddef.h>
#include <stdint.h>

#include <source_location>

#if OSPRIM_ARCH_X86_64
  #if defined(_MSC_VER)
    #include <intrin.h>
  #else
    #include <x86intrin.h>
  #endif
#elif OSPRIM_ARCH_ARM64 && defined(_MSC_VER)
  #include <intrin.h>
#endif

namespace osprim
{
  inline namespace v0_1
  {
    namespace detail
    {
      /*****************************/
      /* LIBRARY STATE             */
      /*****************************/

      /// @brief True between initialize() and deinitialize().
      bool is_initialized() noexcept;

      /// @brief Allocates through the hooks given to initialize().
      /// @return null on failure
      void* allocate(size_t size, size_t align) noexcept;
      /// @brief Frees memory returned by allocate() with the same size and alignment.
      void deallocate(void* ptr, size_t size, size_t align) noexcept;

      /// @brief Sends a diagnostic to the callback given to initialize(), if any.
      void report(const diagnostic& d) noexcept;

      /// @brief A process-unique non-zero id, for handles whose native
      /// value is not unique (or absent) and must still be tracked.
      uint64_t next_handle_id() noexcept;

      /*****************************/
      /* HANDLE TRACKING           */
      /*****************************/

      // Every module that creates handles calls track_open() after a
      // successful creation and track_close() before releasing the handle.
      // A handle is identified by its type name and native value. These are
      // no-ops when tracking is not compiled in or disabled at runtime.

#if OSPRIM_TRACKING
      /// @brief Records an open handle.
      void track_open(
          const char* type, uint64_t native, std::source_location loc) noexcept;
      /// @brief Forgets a handle, reporting DIAGNOSTIC_HANDLE_MISUSE if it is not open.
      void track_close(
          const char* type, uint64_t native, std::source_location loc) noexcept;
      /// @brief Number of handles currently recorded.
      size_t tracked_count() noexcept;
#else
      inline void track_open(const char*, uint64_t, std::source_location) noexcept
      {
      }
      inline void track_close(const char*, uint64_t, std::source_location) noexcept
      {
      }
      inline size_t tracked_count() noexcept
      {
        return 0;
      }
#endif

      /*****************************/
      /* CLOCKS                    */
      /*****************************/

      /// @brief Clock state, written by clock_initialize() only.
      struct clock_state
      {
        /// @brief ticks_to_ns(t) = (t * ticks_mult) >> ticks_shift (128-bit product)
        uint64_t ticks_mult;
        uint32_t ticks_shift;
        uint64_t ticks_frequency_hz;
        /// @brief clock_resolution_ns(), indexed by clock_id
        int64_t resolution_ns[4];
        tick_source ticks_source;
        bool ticks_frequency_exact;
#if OSPRIM_OS_APPLE
        /// @brief mach_timebase_info: ns = ticks * numer / denom
        uint32_t mach_numer;
        uint32_t mach_denom;
#endif
      };

      extern clock_state g_clock;

      /// @brief Fixed-point factor converting ticks to nanoseconds.
      struct ticks_conversion
      {
        uint64_t mult;
        uint32_t shift;
      };

      /// @brief The factor for a counter running at `hz`: mult is
      /// ceil(1e9 * 2^shift / hz), with shift as large as possible (<= 63).
      /// Rounding up makes the conversion exact (equal to
      /// floor(t * 1e9 / hz)) for every t below 2^shift / (hz / gcd(hz, 1e9)):
      /// decades for every real counter (127 years at 24 MHz, 39 at
      /// 19.2 MHz, 97 at 3 GHz). Beyond, it is a few nanoseconds too large at most.
      /// @pre hz != 0
      ticks_conversion make_ticks_conversion(uint64_t hz) noexcept;

      /// @brief Fills g_clock: platform clocks, then the tick counter.
      status<initialize_result> clock_initialize() noexcept;

      // Implemented by the platform source (clock_linux.cpp, ...):

      /// @brief Platform setup: resolutions of the awake, continuous and wall
      /// clocks, plus anything awake_now() needs.
      status<initialize_result> platform_clock_initialize() noexcept;
      /// @brief Sleeps for about `ns` nanoseconds (used for calibration only).
      void platform_sleep_ns(int64_t ns) noexcept;

      /*****************************/
      /* RAW TICK COUNTER          */
      /*****************************/

#if OSPRIM_ARCH_ARM64 && defined(_MSC_VER)
  // Values of ARM64_SYSREG() from winnt.h, without including windows.h
  #define OSPRIM_ARM64_SYSREG(op0, op1, crn, crm, op2)               \
    ((((op0) & 1) << 14) | (((op1) & 7) << 11) | (((crn) & 15) << 7) \
     | (((crm) & 15) << 3) | ((op2) & 7))
  #define OSPRIM_ARM64_CNTVCT_EL0 OSPRIM_ARM64_SYSREG(3, 3, 14, 0, 2)
  #define OSPRIM_ARM64_CNTFRQ_EL0 OSPRIM_ARM64_SYSREG(3, 3, 14, 0, 0)
#endif

      /// @brief Reads the hardware counter (TSC or CNTVCT_EL0), no fence.
      inline uint64_t read_hardware_ticks() noexcept
      {
#if OSPRIM_ARCH_X86_64
        return __rdtsc();
#elif defined(_MSC_VER)
        return static_cast<uint64_t>(_ReadStatusReg(OSPRIM_ARM64_CNTVCT_EL0));
#else
        uint64_t v;
        __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
        return v;
#endif
      }

#if OSPRIM_OS_APPLE
      /// @brief Mach ticks reaching at least `ns` awake nanoseconds (ns >= 0).
      /// @return false if not representable (centuries away)
      bool awake_ns_to_mach_ceil(int64_t ns, uint64_t* ticks) noexcept;
#endif

      /*****************************/
      /* THREADS                   */
      /*****************************/

      /// @brief Body of every thread, called by the native trampoline:
      /// publishes the id and the states, runs the entry function.
      void run_thread(thread_context* ctx) noexcept;

      // Implemented by the platform source (thread_posix.cpp, ...):

      /// @brief Starts a native thread running run_thread(&ctx), sets ctx.native.
      status<thread_create_result> platform_thread_create(
          thread_context& ctx, size_t stack_size) noexcept;
      /// @brief Releases a thread whose state is EXITED (waits for the last
      /// few instructions of the trampoline).
      void platform_thread_join(uint64_t native) noexcept;
      /// @brief Releases a thread that may still run.
      void platform_thread_detach(uint64_t native) noexcept;

      /*****************************/
      /* POWER                     */
      /*****************************/

      /// @brief Set by power_initialize(), constant until deinitialize().
      extern power_features g_power_features;

      /// @brief Detects the supported goals (never fails: unsupported is false).
      void power_initialize() noexcept;
      /// @brief Releases what power_initialize() acquired.
      void power_deinitialize() noexcept;

      // Implemented by the platform source (power_windows.cpp, ...):

      void platform_power_initialize(power_features* out) noexcept;
      void platform_power_deinitialize() noexcept;
      status<timer_resolution_result> platform_timer_resolution_begin(
          int64_t period_ns, uint32_t* period_ms) noexcept;
      void platform_timer_resolution_end(uint32_t period_ms) noexcept;
      /// @param reason UTF-8, at most ACTIVITY_REASON_MAX bytes
      status<activity_result> platform_activity_begin(
          const activity_desc& desc, uint64_t* native,
          activity_desc* granted) noexcept;
      void platform_activity_end(
          uint64_t native, const activity_desc& granted) noexcept;
      /// @param tids `count` thread ids, count in [1, PERF_SESSION_MAX_THREADS]
      status<perf_session_result> platform_perf_session_create(
          const uint64_t* tids, size_t count, int64_t target_ns,
          uint64_t* native) noexcept;
      status<perf_session_result> platform_perf_session_set_target(
          uint64_t native, int64_t target_ns) noexcept;
      status<perf_session_result> platform_perf_session_report(
          uint64_t native, int64_t actual_ns) noexcept;
      void platform_perf_session_close(uint64_t native) noexcept;
      status<thermal_result> platform_thermal_level(thermal_level* out) noexcept;
      status<thermal_result> platform_thermal_watch_begin(
          std::atomic<wait_word_t>& counter, uint64_t* native,
          int32_t* token) noexcept;
      void platform_thermal_watch_end(uint64_t native, int32_t token) noexcept;
      status<thermal_result> platform_thermal_headroom(
          int64_t forecast_ns, float* out) noexcept;

      /*****************************/
      /* WAIT                      */
      /*****************************/

      /// @brief The storage of a wait word, as the OS primitives take it.
      inline wait_word_t* wait_address(const std::atomic<wait_word_t>& word) noexcept
      {
        return const_cast<wait_word_t*>(reinterpret_cast<const wait_word_t*>(&word));
      }

      /*****************************/
      /* ARITHMETIC                */
      /*****************************/

      /// @brief High and low halves of a 64x64 bit product.
      inline uint64_t mul_64x64(uint64_t a, uint64_t b, uint64_t* hi) noexcept
      {
#if defined(__SIZEOF_INT128__)
        // __extension__: __int128 is not ISO C++ (-Wpedantic)
        __extension__ using u128 = unsigned __int128;
        const u128 p             = static_cast<u128>(a) * b;
        *hi                      = static_cast<uint64_t>(p >> 64);
        return static_cast<uint64_t>(p);
#elif defined(_MSC_VER) && defined(_M_X64)
        return _umul128(a, b, hi);
#elif defined(_MSC_VER) && defined(_M_ARM64)
        *hi = __umulh(a, b);
        return a * b;
#else
  #error "osprim: no 64x64->128 bit multiplication for this compiler"
#endif
      }
    } // namespace detail
  } // namespace v0_1
} // namespace osprim

#endif // !OSPRIM_SRC_INTERNAL_HPP
