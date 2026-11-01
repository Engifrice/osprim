/*****************************************************************/ /**
 * @file   thread_windows.cpp
 * @brief  Windows threads, plus the calling thread settings.
 *
 * - class: SetThreadPriority on the calling thread.
 * - name: SetThreadDescription (Windows 10 1607+, resolved at run time), from UTF-8.
 * - sleep: a high-resolution waitable timer (Windows 10 1803+): precise
 *   without raising the system timer resolution. Falls back to a regular
 *   timer, then to Sleep().
 *********************************************************************/
#include "internal.hpp"

#if !OSPRIM_OS_WINDOWS
  #error "thread_windows.cpp is for Windows only"
#endif

#include "win32.hpp"

#include <string.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
  #define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace osprim
{
  inline namespace v0_1
  {
    namespace
    {
      DWORD WINAPI trampoline(LPVOID ctx) noexcept
      {
        detail::run_thread(static_cast<thread_context*>(ctx));
        return 0;
      }

      HANDLE from_native(uint64_t v) noexcept
      {
        return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(v));
      }
    } // namespace

    namespace detail
    {
      status<thread_create_result> platform_thread_create(
          thread_context& ctx, size_t stack_size) noexcept
      {
        // The size is a reservation (like pthreads), rounded up by Windows
        const HANDLE h = ::CreateThread(
            nullptr, stack_size, &trampoline, &ctx,
            stack_size != 0 ? STACK_SIZE_PARAM_IS_A_RESERVATION : 0, nullptr);
        if (h == nullptr)
        {
          const DWORD err = ::GetLastError();
          const bool resources =
              err == ERROR_NOT_ENOUGH_MEMORY || err == ERROR_OUTOFMEMORY
              || err == ERROR_MAX_THRDS_REACHED || err == ERROR_COMMITMENT_LIMIT;
          return {
              resources ? thread_create_result::THREAD_CREATE_OUT_OF_RESOURCES
                        : thread_create_result::THREAD_CREATE_SYSTEM_ERROR,
              native_error_kind::NATIVE_ERROR_WIN32, static_cast<int32_t>(err)};
        }
        ctx.native = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(h));
        return {};
      }

      void platform_thread_join(uint64_t native) noexcept
      {
        const HANDLE h = from_native(native);
        ::WaitForSingleObject(h, INFINITE);
        ::CloseHandle(h);
      }

      void platform_thread_detach(uint64_t native) noexcept
      {
        ::CloseHandle(from_native(native));
      }
    } // namespace detail

    uint64_t current_thread_id() noexcept
    {
      return ::GetCurrentThreadId();
    }

    status<set_thread_name_result> set_current_thread_name(const char* name) noexcept
    {
      OSPRIM_ASSERT(name != nullptr, "osprim::set_current_thread_name: null name");
      OSPRIM_ASSERT(
          ::strlen(name) <= THREAD_NAME_MAX,
          "osprim::set_current_thread_name: longer than THREAD_NAME_MAX bytes");
      // At most one UTF-16 unit per UTF-8 byte, plus the terminator
      wchar_t wide[THREAD_NAME_MAX + 1];
      if (::MultiByteToWideChar(
              CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, wide, THREAD_NAME_MAX + 1)
          == 0)
        return {
            set_thread_name_result::SET_THREAD_NAME_SYSTEM_ERROR,
            native_error_kind::NATIVE_ERROR_WIN32,
            static_cast<int32_t>(::GetLastError())};
      // Resolved at run time: not declared by every SDK (MinGW), and
      // linking it would stop the program from loading before Windows 10 1607
      using set_description_fn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
      const HMODULE kernel     = ::GetModuleHandleW(L"kernel32.dll");
      const FARPROC proc = kernel != nullptr
                               ? ::GetProcAddress(kernel, "SetThreadDescription")
                               : nullptr;
      if (proc == nullptr)
        return {
            set_thread_name_result::SET_THREAD_NAME_SYSTEM_ERROR,
            native_error_kind::NATIVE_ERROR_WIN32,
            static_cast<int32_t>(ERROR_PROC_NOT_FOUND)};
      const auto set_description = reinterpret_cast<set_description_fn>(
          reinterpret_cast<void (*)(void)>(proc));
      const HRESULT hr = set_description(::GetCurrentThread(), wide);
      if (FAILED(hr))
      {
        // HRESULTs wrapping a Win32 error are reported as that error
        const bool win32 = HRESULT_FACILITY(hr) == FACILITY_WIN32;
        return {
            set_thread_name_result::SET_THREAD_NAME_SYSTEM_ERROR,
            win32 ? native_error_kind::NATIVE_ERROR_WIN32
                  : native_error_kind::NATIVE_ERROR_NONE,
            win32 ? static_cast<int32_t>(HRESULT_CODE(hr)) : 0};
      }
      return {};
    }

    status<set_thread_class_result> set_current_thread_class(thread_class c) noexcept
    {
      OSPRIM_ASSERT(
          static_cast<size_t>(c)
              <= static_cast<size_t>(thread_class::THREAD_CLASS_INTERACTIVE),
          "osprim::set_current_thread_class: invalid class");
      constexpr int PRIORITY[4] = {
          THREAD_PRIORITY_LOWEST, THREAD_PRIORITY_BELOW_NORMAL,
          THREAD_PRIORITY_NORMAL, THREAD_PRIORITY_ABOVE_NORMAL};
      if (::SetThreadPriority(
              ::GetCurrentThread(), PRIORITY[static_cast<size_t>(c)]))
        return {};
      const DWORD err = ::GetLastError();
      return {
          err == ERROR_ACCESS_DENIED
              ? set_thread_class_result::SET_THREAD_CLASS_DENIED
              : set_thread_class_result::SET_THREAD_CLASS_SYSTEM_ERROR,
          native_error_kind::NATIVE_ERROR_WIN32, static_cast<int32_t>(err)};
    }

    void yield_thread() noexcept
    {
      ::SwitchToThread();
    }

    void sleep_until(awake_instant deadline) noexcept
    {
      OSPRIM_ASSERT(
          !deadline.is_forever(), "osprim::sleep_until: cannot sleep forever");
      if (awake_now() >= deadline)
        return;
      HANDLE timer = ::CreateWaitableTimerExW(
          nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
      if (timer == nullptr) // before Windows 10 1803
        timer = ::CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
      for (;;)
      {
        const awake_instant now = awake_now();
        if (now >= deadline)
          break;
        const int64_t remaining = deadline.ns - now.ns; // > 0
        if (timer != nullptr)
        {
          // Relative due time: negative, in 100 ns units, rounded up
          LARGE_INTEGER due;
          due.QuadPart = -(remaining / 100 + (remaining % 100 != 0 ? 1 : 0));
          if (::SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
          {
            ::WaitForSingleObject(timer, INFINITE);
            continue;
          }
        }
        const int64_t ms = remaining / 1000000 + (remaining % 1000000 != 0 ? 1 : 0);
        ::Sleep(
            ms >= static_cast<int64_t>(INFINITE) ? INFINITE - 1
                                                 : static_cast<DWORD>(ms));
      }
      if (timer != nullptr)
        ::CloseHandle(timer);
    }
  } // namespace v0_1
} // namespace osprim
