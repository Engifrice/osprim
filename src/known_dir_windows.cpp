/*****************************************************************/ /**
 * @file   known_dir_windows.cpp
 * @brief  Known directories on Windows.
 *
 * - DATA, STATE, CACHE: SHGetKnownFolderPath(FOLDERID_LocalAppData,
 *   KF_FLAG_DONT_VERIFY), its string freed with CoTaskMemFree. shell32 and
 *   ole32 are loaded on first use (from System32 only), not by
 *   initialize(): loading shell32 is costly. They stay loaded until the
 *   process ends. No COM initialization needed.
 * - TEMP: GetTempPath2W where it exists (SYSTEM processes get
 *   C:\Windows\SystemTemp), else GetTempPathW.
 * - EXECUTABLE, RESOURCES: the directory of GetModuleFileNameW(nullptr).
 * - Everything is resolved at each call: the shell, not the environment,
 *   holds these folders. Overrides are normalized as file_open() does.
 * - Paths are compared ignoring case (CompareStringOrdinal).
 * - Backup exclusion: UNSUPPORTED (no per-folder mechanism; Windows'
 *   own backups skip AppData).
 *********************************************************************/
#include "internal.hpp"

#if !OSPRIM_OS_WINDOWS
  #error "known_dir_windows.cpp is for Windows only"
#endif

#include "win32.hpp"

#include <string.h>
#include <wchar.h>

#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
  #define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#endif

namespace osprim
{
  inline namespace v0_1
  {
    namespace
    {
      using namespace detail;
      using GR = known_dir_get_result;
      using OR = known_dir_open_result;

      template<class R>
      status<R> fail_win(R r, DWORD e) noexcept
      {
        return {r, native_error_kind::NATIVE_ERROR_WIN32, static_cast<int32_t>(e)};
      }

      template<class R>
      status<R> fail(R r) noexcept
      {
        return {r, native_error_kind::NATIVE_ERROR_NONE, 0};
      }

      /// @brief FOLDERID_LocalAppData, {F1B32785-6FBA-4FCF-9D55-7B8E7F157091}
      constexpr GUID FOLDER_LOCAL_APP_DATA = {
          0xF1B32785,
          0x6FBA,
          0x4FCF,
          {0x9D, 0x55, 0x7B, 0x8E, 0x7F, 0x15, 0x70, 0x91}};
      /// @brief KF_FLAG_DONT_VERIFY: no check that the folder exists
      constexpr DWORD KNOWN_FOLDER_DONT_VERIFY = 0x00004000;

      using get_known_folder_fn =
          HRESULT(WINAPI*)(const GUID*, DWORD, HANDLE, PWSTR*);
      using task_mem_free_fn = void(WINAPI*)(void*);
      using get_temp_path_fn = DWORD(WINAPI*)(DWORD, LPWSTR);

      /// @brief The functions loaded on first use.
      struct shell_api
      {
        HMODULE shell32;
        HMODULE ole32;
        get_known_folder_fn get_known_folder;
        task_mem_free_fn task_mem_free;
        get_temp_path_fn get_temp_path2;
      };

      shell_api g_shell{};
      INIT_ONCE g_shell_once = INIT_ONCE_STATIC_INIT;

      template<class F>
      F resolve_proc(HMODULE module, const char* name) noexcept
      {
        if (module == nullptr)
          return nullptr;
        // Through void(*)(void), the generic function pointer type
        return reinterpret_cast<F>(
            reinterpret_cast<void (*)(void)>(::GetProcAddress(module, name)));
      }

      BOOL CALLBACK load_shell(PINIT_ONCE, PVOID, PVOID*) noexcept
      {
        g_shell.shell32 =
            ::LoadLibraryExW(L"shell32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        g_shell.ole32 =
            ::LoadLibraryExW(L"ole32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        g_shell.get_known_folder = resolve_proc<get_known_folder_fn>(
            g_shell.shell32, "SHGetKnownFolderPath");
        g_shell.task_mem_free =
            resolve_proc<task_mem_free_fn>(g_shell.ole32, "CoTaskMemFree");
        g_shell.get_temp_path2 = resolve_proc<get_temp_path_fn>(
            ::GetModuleHandleW(L"kernel32.dll"), "GetTempPath2W");
        return TRUE;
      }

      const shell_api& shell() noexcept
      {
        ::InitOnceExecuteOnce(&g_shell_once, &load_shell, nullptr, nullptr);
        return g_shell;
      }

      status<GR> report(
          const wchar_t* w, size_t units, char* out, size_t* size) noexcept
      {
        const uint32_t e = windows_report_path(w, units, out, size);
        if (e == ERROR_FILENAME_EXCED_RANGE)
          return fail(GR::KNOWN_DIR_GET_PATH_TOO_LONG);
        if (e != 0)
          return fail_win(GR::KNOWN_DIR_GET_SYSTEM_ERROR, e);
        return {};
      }

      /// @brief An HRESULT as a status: its Win32 code when it carries one.
      status<GR> fail_hresult(HRESULT hr) noexcept
      {
        const uint32_t v = static_cast<uint32_t>(hr);
        if ((v & 0xFFFF0000u) == 0x80070000u) // FACILITY_WIN32
          return fail_win(GR::KNOWN_DIR_GET_SYSTEM_ERROR, v & 0xFFFFu);
        return fail(GR::KNOWN_DIR_GET_SYSTEM_ERROR);
      }

      constexpr DWORD WIDE_CAPACITY = MAX_PATH_BYTES + 16;
    } // namespace

    namespace detail
    {
      bool platform_known_dir_normalize(
          const char* path, size_t size, char* out, size_t* out_size) noexcept
      {
        wchar_t wide[WIDE_CAPACITY];
        bool verbatim = false;
        if (!windows_file_path(path, size, wide, WIDE_CAPACITY, &verbatim).ok())
          return false;
        return windows_report_path(wide, ::wcslen(wide), out, out_size) == 0;
      }

      void platform_known_dir_initialize(known_dir_table&) noexcept
      {
      }

      void platform_known_dir_deinitialize() noexcept
      {
        // shell32 and ole32 stay loaded until the process ends: shell32 may
        // leave worker threads and hooks behind, which unloading it would
        // crash (and a later initialize() would load it again for nothing)
      }

      status<known_dir_get_result> platform_known_dir_resolve(
          known_dir which, char* out, size_t* size) noexcept
      {
        wchar_t w[WIDE_CAPACITY];
        switch (which)
        {
        case known_dir::KNOWN_DIR_DATA:
        case known_dir::KNOWN_DIR_STATE:
        case known_dir::KNOWN_DIR_CACHE:
        {
          const shell_api& api = shell();
          if (api.get_known_folder == nullptr || api.task_mem_free == nullptr)
            return fail_win(GR::KNOWN_DIR_GET_SYSTEM_ERROR, ERROR_PROC_NOT_FOUND);
          PWSTR p          = nullptr;
          const HRESULT hr = api.get_known_folder(
              &FOLDER_LOCAL_APP_DATA, KNOWN_FOLDER_DONT_VERIFY, nullptr, &p);
          if (FAILED(hr) || p == nullptr)
          {
            if (p != nullptr)
              api.task_mem_free(p);
            return fail_hresult(hr);
          }
          const status<GR> s = report(p, ::wcslen(p), out, size);
          api.task_mem_free(p);
          return s;
        }
        case known_dir::KNOWN_DIR_TEMP:
        {
          const get_temp_path_fn temp2 = shell().get_temp_path2;
          const DWORD n = temp2 != nullptr ? temp2(WIDE_CAPACITY, w)
                                           : ::GetTempPathW(WIDE_CAPACITY, w);
          if (n == 0)
            return fail_win(GR::KNOWN_DIR_GET_SYSTEM_ERROR, ::GetLastError());
          if (n >= WIDE_CAPACITY)
            return fail(GR::KNOWN_DIR_GET_PATH_TOO_LONG);
          return report(w, n, out, size);
        }
        case known_dir::KNOWN_DIR_EXECUTABLE:
        case known_dir::KNOWN_DIR_RESOURCES:
        {
          const DWORD n = ::GetModuleFileNameW(nullptr, w, WIDE_CAPACITY);
          if (n == 0)
            return fail_win(GR::KNOWN_DIR_GET_SYSTEM_ERROR, ::GetLastError());
          if (n >= WIDE_CAPACITY)
            return fail(GR::KNOWN_DIR_GET_PATH_TOO_LONG);
          size_t units = n;
          while (units > 0 && w[units - 1] != L'\\' && w[units - 1] != L'/')
            --units;
          if (units == 0)
            return fail(GR::KNOWN_DIR_GET_SYSTEM_ERROR);
          return report(
              w, units, out, size); // the separator goes (except at a root)
        }
        }
        return fail(GR::KNOWN_DIR_GET_NOT_CONFIGURED);
      }

      void platform_known_dir_traits(
          known_dir which, uint32_t, known_dir_info* info) noexcept
      {
        info->app_private   = which == known_dir::KNOWN_DIR_EXECUTABLE
                              || which == known_dir::KNOWN_DIR_RESOURCES;
        info->backed_up     = false;
        info->may_be_purged = which == known_dir::KNOWN_DIR_TEMP;
      }

      bool platform_known_dir_same(
          const char* a, size_t an, const char* b, size_t bn) noexcept
      {
        wchar_t wa[MAX_PATH_BYTES + 1];
        wchar_t wb[MAX_PATH_BYTES + 1];
        const int na = ::MultiByteToWideChar(
            CP_UTF8, 0, a, static_cast<int>(an), wa,
            static_cast<int>(MAX_PATH_BYTES));
        const int nb = ::MultiByteToWideChar(
            CP_UTF8, 0, b, static_cast<int>(bn), wb,
            static_cast<int>(MAX_PATH_BYTES));
        if (na <= 0 || nb <= 0)
          return an == bn && ::memcmp(a, b, an) == 0;
        return ::CompareStringOrdinal(wa, na, wb, nb, TRUE) == CSTR_EQUAL;
      }

      mkdir_outcome platform_known_dir_mkdir(
          const char* path, size_t size,
          status<known_dir_open_result>* error) noexcept
      {
        wchar_t wide[WIDE_CAPACITY];
        bool verbatim = false;
        const status<file_open_result> c =
            windows_file_path(path, size, wide, WIDE_CAPACITY, &verbatim);
        if (!c.ok())
        {
          *error = {
              c.result == file_open_result::FILE_OPEN_PATH_TOO_LONG
                  ? OR::KNOWN_DIR_OPEN_PATH_TOO_LONG
                  : OR::KNOWN_DIR_OPEN_SYSTEM_ERROR,
              c.native_kind, c.native_code};
          return mkdir_outcome::FAILED;
        }
        if (::CreateDirectoryW(wide, nullptr))
          return mkdir_outcome::CREATED;
        const DWORD e = ::GetLastError();
        switch (e)
        {
        case ERROR_ALREADY_EXISTS:
          return mkdir_outcome::EXISTS;
        case ERROR_PATH_NOT_FOUND:
          return mkdir_outcome::PARENT_MISSING;
        case ERROR_ACCESS_DENIED:
        case ERROR_WRITE_PROTECT:
          *error = fail_win(OR::KNOWN_DIR_OPEN_ACCESS_DENIED, e);
          return mkdir_outcome::FAILED;
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL:
          *error = fail_win(OR::KNOWN_DIR_OPEN_NO_SPACE, e);
          return mkdir_outcome::FAILED;
        case ERROR_FILENAME_EXCED_RANGE:
          *error = fail_win(OR::KNOWN_DIR_OPEN_PATH_TOO_LONG, e);
          return mkdir_outcome::FAILED;
        default:
          *error = fail_win(OR::KNOWN_DIR_OPEN_SYSTEM_ERROR, e);
          return mkdir_outcome::FAILED;
        }
      }

      status<dir_set_backup_excluded_result> platform_dir_set_backup_excluded(
          native_dir_t, bool) noexcept
      {
        return fail(
            dir_set_backup_excluded_result::DIR_SET_BACKUP_EXCLUDED_UNSUPPORTED);
      }
    } // namespace detail
  } // namespace v0_1
} // namespace osprim
