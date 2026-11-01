/*****************************************************************/ /**
 * @file   clock_windows.cpp
 * @brief  Windows clocks.
 *
 * awake: QueryUnbiasedInterruptTimePrecise (excludes suspend).
 * continuous: QueryInterruptTimePrecise (includes suspend).
 * wall: GetSystemTimePreciseAsFileTime.
 * All three count 100 ns units. The first two (Windows 10+) are not in
 * kernel32.lib, so they are resolved once at initialization.
 *********************************************************************/
#include "internal.hpp"

#if !OSPRIM_OS_WINDOWS
  #error "clock_windows.cpp is for Windows only"
#endif

#include "win32.hpp"

namespace osprim
{
  inline namespace v0_1
  {
    namespace
    {
      using query_time_fn = VOID(WINAPI*)(PULONGLONG);

      query_time_fn g_query_unbiased  = nullptr;
      query_time_fn g_query_interrupt = nullptr;

      /// @brief 100 ns intervals between 1601-01-01 and 1970-01-01
      constexpr int64_t FILETIME_UNIX_EPOCH = 116444736000000000;

      query_time_fn resolve(HMODULE module, const char* name) noexcept
      {
        if (module == nullptr)
          return nullptr;
        const FARPROC p = ::GetProcAddress(module, name);
        // Through void(*)(void), the generic function pointer type
        return reinterpret_cast<query_time_fn>(reinterpret_cast<void (*)(void)>(p));
      }
    } // namespace

    awake_instant awake_now() noexcept
    {
      ULONGLONG t;
      g_query_unbiased(&t);
      return {static_cast<int64_t>(t) * 100};
    }

    continuous_instant continuous_now() noexcept
    {
      ULONGLONG t;
      g_query_interrupt(&t);
      return {static_cast<int64_t>(t) * 100};
    }

    wall_instant wall_now() noexcept
    {
      FILETIME ft;
      ::GetSystemTimePreciseAsFileTime(&ft);
      const uint64_t t =
          (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
      return {(static_cast<int64_t>(t) - FILETIME_UNIX_EPOCH) * 100};
    }

    namespace detail
    {
      status<initialize_result> platform_clock_initialize() noexcept
      {
        const wchar_t* const MODULES[2] = {L"kernelbase.dll", L"kernel32.dll"};
        g_query_unbiased                = nullptr;
        g_query_interrupt               = nullptr;
        for (const wchar_t* name : MODULES)
        {
          const HMODULE m = ::GetModuleHandleW(name);
          if (g_query_unbiased == nullptr)
            g_query_unbiased = resolve(m, "QueryUnbiasedInterruptTimePrecise");
          if (g_query_interrupt == nullptr)
            g_query_interrupt = resolve(m, "QueryInterruptTimePrecise");
        }
        if (g_query_unbiased == nullptr || g_query_interrupt == nullptr)
          return {
              initialize_result::INITIALIZE_SYSTEM_ERROR,
              native_error_kind::NATIVE_ERROR_WIN32,
              static_cast<int32_t>(ERROR_PROC_NOT_FOUND)};

        g_clock.resolution_ns[static_cast<size_t>(clock_id::CLOCK_ID_AWAKE)] = 100;
        g_clock.resolution_ns[static_cast<size_t>(clock_id::CLOCK_ID_CONTINUOUS)] =
            100;
        g_clock.resolution_ns[static_cast<size_t>(clock_id::CLOCK_ID_WALL)] = 100;
        return {};
      }

      void platform_sleep_ns(int64_t ns) noexcept
      {
        ::Sleep(static_cast<DWORD>((ns + 999999) / 1000000));
      }
    } // namespace detail
  } // namespace v0_1
} // namespace osprim
