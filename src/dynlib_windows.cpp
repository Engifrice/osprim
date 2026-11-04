/*****************************************************************/ /**
 * @file   dynlib_windows.cpp
 * @brief  Dynamic libraries on Windows: LoadLibraryExW with the safe search
 * flags, GetProcAddress, FreeLibrary.
 *
 * - PATH: LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
 *   (dependencies from the library's own directory, then the application
 *   directory, System32 and AddDllDirectory directories). The path is
 *   normalized as file_open() does, then passed in its plain form when it
 *   fits in MAX_PATH.
 * - SYSTEM: LOAD_LIBRARY_SEARCH_DEFAULT_DIRS: never the current directory
 *   or PATH (no DLL hijacking).
 * - Critical-error dialogs are suppressed on the calling thread meanwhile.
 *********************************************************************/
#include "internal.hpp"

#if !OSPRIM_OS_WINDOWS
  #error "dynlib_windows.cpp is for Windows only"
#endif

#include "win32.hpp"

#include <string.h>
#include <wchar.h>

#ifndef LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
  #define LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR 0x00000100
#endif
#ifndef LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
  #define LOAD_LIBRARY_SEARCH_DEFAULT_DIRS 0x00001000
#endif

namespace osprim
{
  inline namespace v0_1
  {
    namespace
    {
      /// @brief The system message for `err`, in UTF-8, truncated to the buffer.
      size_t system_message(DWORD err, char* out, size_t capacity) noexcept
      {
        wchar_t w[512];
        DWORD n = ::FormatMessageW(
            FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err,
            0, w, static_cast<DWORD>(sizeof(w) / sizeof(w[0])), nullptr);
        while (n > 0
               && (w[n - 1] == L'\r' || w[n - 1] == L'\n' || w[n - 1] == L' '
                   || w[n - 1] == L'.'))
          --n;
        if (n == 0)
          return 0;
        char utf8[3 * 512];
        const int m = ::WideCharToMultiByte(
            CP_UTF8, 0, w, static_cast<int>(n), utf8, static_cast<int>(sizeof(utf8)),
            nullptr, nullptr);
        if (m <= 0)
          return 0;
        size_t len = static_cast<size_t>(m);
        if (len > capacity)
          len = capacity;
        ::memcpy(out, utf8, len);
        return len;
      }
    } // namespace

    namespace detail
    {
      status<dynlib_open_result> platform_dynlib_open(
          const char* name, size_t size, const dynlib_open_desc& desc,
          native_dynlib_t* out, size_t* message_size) noexcept
      {
        using R = dynlib_open_result;
        wchar_t wide[MAX_PATH_BYTES + 16];
        const wchar_t* target = wide;
        DWORD flags           = LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
        if (desc.search == dynlib_search::DYNLIB_SEARCH_PATH)
        {
          bool verbatim = false;
          const status<file_open_result> conv =
              windows_file_path(name, size, wide, MAX_PATH_BYTES + 16, &verbatim);
          switch (conv.result)
          {
          case file_open_result::FILE_OPEN_OK:
            break;
          case file_open_result::FILE_OPEN_PATH_NOT_ABSOLUTE:
            return {
                R::DYNLIB_OPEN_PATH_NOT_ABSOLUTE,
                native_error_kind::NATIVE_ERROR_NONE, 0};
          case file_open_result::FILE_OPEN_PATH_TOO_LONG:
            return {
                R::DYNLIB_OPEN_PATH_TOO_LONG, native_error_kind::NATIVE_ERROR_NONE,
                0};
          default:
            return {R::DYNLIB_OPEN_INVALID_PATH, conv.native_kind, conv.native_code};
          }
          // The plain form when it fits (the loader's support for \\?\ varies),
          // unless the caller asked for the verbatim form
          const size_t n = ::wcslen(wide);
          if (!verbatim && n >= 4 && ::wcsncmp(wide, L"\\\\?\\", 4) == 0
              && n - 4 < MAX_PATH)
          {
            if (n >= 8 && ::wcsncmp(wide + 4, L"UNC\\", 4) == 0)
            {
              wide[6] = L'\\'; // \\?\UNC\s\x -> \\s\x
              target  = wide + 6;
            }
            else
              target = wide + 4;
          }
          flags |= LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR;
        }
        else
        {
          const int n = ::MultiByteToWideChar(
              CP_UTF8, MB_ERR_INVALID_CHARS, name, static_cast<int>(size), wide,
              static_cast<int>(MAX_PATH_BYTES));
          if (n <= 0)
            return {
                R::DYNLIB_OPEN_INVALID_PATH, native_error_kind::NATIVE_ERROR_WIN32,
                static_cast<int32_t>(::GetLastError())};
          wide[n] = L'\0';
        }

        DWORD old_mode = 0;
        const bool mode_set =
            ::SetThreadErrorMode(
                SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &old_mode)
            != 0;
        const HMODULE h = ::LoadLibraryExW(target, nullptr, flags);
        const DWORD err = h == nullptr ? ::GetLastError() : 0;
        if (mode_set)
          ::SetThreadErrorMode(old_mode, nullptr);
        if (h != nullptr)
        {
          *out = h;
          return {};
        }
        if (desc.message != nullptr && desc.message_capacity != 0)
        {
          const size_t n = system_message(err, desc.message, desc.message_capacity);
          if (message_size != nullptr)
            *message_size = n;
        }
        if (desc.search == dynlib_search::DYNLIB_SEARCH_PATH)
        {
          // The file itself, or one of its dependencies? (A directory fails
          // with ERROR_ACCESS_DENIED too: not a library, as elsewhere.)
          const DWORD attributes = ::GetFileAttributesW(target);
          if (attributes == INVALID_FILE_ATTRIBUTES)
          {
            const DWORD e = ::GetLastError();
            if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)
              return {
                  R::DYNLIB_OPEN_NOT_FOUND, native_error_kind::NATIVE_ERROR_WIN32,
                  static_cast<int32_t>(e)};
          }
          else if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            return {
                R::DYNLIB_OPEN_LOAD_FAILED, native_error_kind::NATIVE_ERROR_WIN32,
                static_cast<int32_t>(err)};
          if (err == ERROR_ACCESS_DENIED)
            return {
                R::DYNLIB_OPEN_ACCESS_DENIED, native_error_kind::NATIVE_ERROR_WIN32,
                static_cast<int32_t>(err)};
        }
        return {
            R::DYNLIB_OPEN_LOAD_FAILED, native_error_kind::NATIVE_ERROR_WIN32,
            static_cast<int32_t>(err)};
      }

      void platform_dynlib_close(native_dynlib_t lib) noexcept
      {
        ::FreeLibrary(static_cast<HMODULE>(lib));
      }

      void* platform_dynlib_symbol(native_dynlib_t lib, const char* name) noexcept
      {
        return reinterpret_cast<void*>(
            ::GetProcAddress(static_cast<HMODULE>(lib), name));
      }
    } // namespace detail
  } // namespace v0_1
} // namespace osprim
