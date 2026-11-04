/*****************************************************************/ /**
 * @file   dynlib.cpp
 * @brief  Dynamic libraries: name checks, tracking (reference counted:
 * the OS returns the same handle for every load of a library). The OS calls
 * live in dynlib_posix.cpp and dynlib_windows.cpp.
 *********************************************************************/
#include "internal.hpp"

#include <string.h>

namespace osprim
{
  inline namespace v0_1
  {
    namespace
    {
      uint64_t lib_key(native_dynlib_t lib) noexcept
      {
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(lib));
      }

      bool has_separator(const char* p, size_t n) noexcept
      {
        for (size_t i = 0; i < n; ++i)
        {
#if OSPRIM_OS_WINDOWS
          if (p[i] == '/' || p[i] == '\\' || p[i] == ':')
            return true;
#else
          if (p[i] == '/')
            return true;
#endif
        }
        return false;
      }

      status<dynlib_open_result> fail(dynlib_open_result r) noexcept
      {
        return {r, native_error_kind::NATIVE_ERROR_NONE, 0};
      }
    } // namespace

    status<dynlib_open_result> dynlib_open(
        const char* name, size_t name_size, const dynlib_open_desc& desc,
        dynlib_handle& out, size_t* message_size, std::source_location loc) noexcept
    {
      using R = dynlib_open_result;
      if (message_size != nullptr)
        *message_size = 0;
      OSPRIM_ASSERT(
          name != nullptr && name_size > 0, "osprim::dynlib_open: empty name");
      if (name_size > MAX_PATH_BYTES)
        return fail(R::DYNLIB_OPEN_PATH_TOO_LONG);
      if (!detail::utf8_valid(name, name_size))
        return fail(R::DYNLIB_OPEN_INVALID_PATH);
      if (desc.search == dynlib_search::DYNLIB_SEARCH_SYSTEM)
      {
        if (has_separator(name, name_size)
            || (name_size <= 2 && name[0] == '.' && name[name_size - 1] == '.'))
          return fail(R::DYNLIB_OPEN_INVALID_PATH);
      }
#if !OSPRIM_OS_WINDOWS
      else if (name[0] != '/')
        return fail(R::DYNLIB_OPEN_PATH_NOT_ABSOLUTE);
#endif
      char buffer[MAX_PATH_BYTES + 1];
      ::memcpy(buffer, name, name_size);
      buffer[name_size] = '\0';

      native_dynlib_t native = nullptr;
      const status<R> s      = detail::platform_dynlib_open(
          buffer, name_size, desc, &native, message_size);
      if (!s.ok())
        return s;
      out.native = native;
      detail::track_open_shared(detail::DYNLIB_TRACK_TYPE, lib_key(native), loc);
      return s;
    }

    void dynlib_close(dynlib_handle lib, std::source_location loc) noexcept
    {
      detail::track_close(detail::DYNLIB_TRACK_TYPE, lib_key(lib.native), loc);
      detail::platform_dynlib_close(lib.native);
    }

    status<dynlib_symbol_result> dynlib_symbol(
        dynlib_handle lib, const char* name, void*& out) noexcept
    {
      out = nullptr;
      if (name == nullptr || name[0] == '\0')
        return {
            dynlib_symbol_result::DYNLIB_SYMBOL_INVALID_NAME,
            native_error_kind::NATIVE_ERROR_NONE, 0};
      out = detail::platform_dynlib_symbol(lib.native, name);
      if (out == nullptr)
        return {
            dynlib_symbol_result::DYNLIB_SYMBOL_NOT_FOUND,
            native_error_kind::NATIVE_ERROR_NONE, 0};
      return {};
    }

    status<dynlib_symbol_result> dynlib_symbol(
        dynlib_handle lib, const char* name, size_t name_size, void*& out) noexcept
    {
      out = nullptr;
      if (name == nullptr || name_size == 0 || name_size > DYNLIB_SYMBOL_MAX
          || ::memchr(name, '\0', name_size) != nullptr)
        return {
            dynlib_symbol_result::DYNLIB_SYMBOL_INVALID_NAME,
            native_error_kind::NATIVE_ERROR_NONE, 0};
      char buffer[DYNLIB_SYMBOL_MAX + 1];
      ::memcpy(buffer, name, name_size);
      buffer[name_size] = '\0';
      return dynlib_symbol(lib, buffer, out);
    }
  } // namespace v0_1
} // namespace osprim
