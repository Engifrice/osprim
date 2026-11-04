/*****************************************************************/ /**
 * @file   dynlib_posix.cpp
 * @brief  Dynamic libraries on Linux, Android and Apple: dlopen with
 * RTLD_NOW | RTLD_LOCAL (macOS defaults to RTLD_GLOBAL), dlsym, dlclose.
 * dlerror() is read (and so cleared) after every failure, whether or not
 * the caller asked for the message. A failed PATH load is NOT_FOUND or
 * ACCESS_DENIED when the file itself is missing or unreadable.
 *********************************************************************/
#include "internal.hpp"

#if OSPRIM_OS_WINDOWS
  #error "dynlib_posix.cpp is for POSIX platforms only"
#endif

#include <dlfcn.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

namespace osprim
{
  inline namespace v0_1
  {
    namespace detail
    {
      status<dynlib_open_result> platform_dynlib_open(
          const char* name, size_t, const dynlib_open_desc& desc,
          native_dynlib_t* out, size_t* message_size) noexcept
      {
        using R      = dynlib_open_result;
        void* handle = ::dlopen(name, RTLD_NOW | RTLD_LOCAL);
        if (handle != nullptr)
        {
          *out = handle;
          return {};
        }
        // Always read the message: it is per thread and stays until read
        const char* msg = ::dlerror();
        if (msg != nullptr && desc.message != nullptr && desc.message_capacity != 0)
        {
          size_t n = ::strlen(msg);
          if (n > desc.message_capacity)
            n = desc.message_capacity;
          ::memcpy(desc.message, msg, n);
          if (message_size != nullptr)
            *message_size = n;
        }
        if (desc.search == dynlib_search::DYNLIB_SEARCH_PATH
            && ::strstr(name, "!/") == nullptr)
        {
          // The file itself, or something inside it? ("!/": a library inside an APK)
          if (::access(name, F_OK) != 0)
            return {
                R::DYNLIB_OPEN_NOT_FOUND,
                native_error_kind::NATIVE_ERROR_POSIX_ERRNO, errno};
          if (::access(name, R_OK) != 0)
            return {
                R::DYNLIB_OPEN_ACCESS_DENIED,
                native_error_kind::NATIVE_ERROR_POSIX_ERRNO, errno};
        }
        return {R::DYNLIB_OPEN_LOAD_FAILED, native_error_kind::NATIVE_ERROR_NONE, 0};
      }

      void platform_dynlib_close(native_dynlib_t lib) noexcept
      {
        ::dlclose(lib);
      }

      void* platform_dynlib_symbol(native_dynlib_t lib, const char* name) noexcept
      {
        ::dlerror(); // clear: a symbol may legitimately be null
        void* p = ::dlsym(lib, name);
        if (p == nullptr)
          ::dlerror(); // consume the message
        return p;
      }
    } // namespace detail
  } // namespace v0_1
} // namespace osprim
