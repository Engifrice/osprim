/*****************************************************************/ /**
 * @file   dynlib_posix.cpp
 * @brief  Dynamic libraries on Linux, Android and Apple: dlopen with
 * RTLD_NOW | RTLD_LOCAL (macOS defaults to RTLD_GLOBAL), dlsym, dlclose.
 * dlerror() is read (and so cleared) after every failure, whether or not
 * the caller asked for the message. A failed PATH load is NOT_FOUND or
 * ACCESS_DENIED when the file itself is missing or unreadable.
 *
 * dynlib_path_of(): dl_iterate_phdr on Linux and Android (the object whose
 * loaded segments contain the address; the executable, which has no name
 * there, through /proc/self/exe), dladdr on Apple (the executable through
 * _NSGetExecutablePath). Paths are made canonical with realpath, except
 * libraries inside an APK and images of the dyld shared cache (no file on
 * disk), which are reported as the loader recorded them.
 *********************************************************************/
#include "internal.hpp"

#if OSPRIM_OS_WINDOWS
  #error "dynlib_posix.cpp is for POSIX platforms only"
#endif

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if OSPRIM_OS_APPLE
  #include <mach-o/dyld.h>
  #include <sys/param.h>
#else
  #include <link.h>
#endif

namespace osprim
{
  inline namespace v0_1
  {
    namespace
    {
      using PR = dynlib_path_result;

      status<PR> path_fail(PR r, int err) noexcept
      {
        return {r, native_error_kind::NATIVE_ERROR_POSIX_ERRNO, err};
      }

      status<PR> path_fail(PR r) noexcept
      {
        return {r, native_error_kind::NATIVE_ERROR_NONE, 0};
      }

      /// @brief Writes a terminated path to `out` as a reported path.
      status<PR> path_report(const char* p, PR r, char* out, size_t* size) noexcept
      {
        const size_t n = ::strlen(p);
        if (n > MAX_PATH_BYTES)
          return path_fail(PR::DYNLIB_PATH_PATH_TOO_LONG);
        if (!detail::utf8_valid(p, n))
          return path_fail(PR::DYNLIB_PATH_SYSTEM_ERROR, EILSEQ);
        ::memcpy(out, p, n + 1);
        *size = n;
        return {r, native_error_kind::NATIVE_ERROR_NONE, 0};
      }

      /// @brief The canonical form of `p`, or `p` itself when it names no
      /// file (dyld shared cache) and `keep_missing`.
      status<PR> path_canonical_report(
          const char* p, bool keep_missing, char* out, size_t* size) noexcept
      {
        char real[PATH_MAX];
        if (::realpath(p, real) != nullptr)
          return path_report(real, PR::DYNLIB_PATH_OK, out, size);
        const int e = errno;
        if (keep_missing && e == ENOENT)
          return path_report(p, PR::DYNLIB_PATH_OK, out, size);
        return path_fail(PR::DYNLIB_PATH_SYSTEM_ERROR, e);
      }

#if !OSPRIM_OS_APPLE
      struct phdr_search
      {
        uintptr_t address;
        bool found;
        bool too_long;
        char name[MAX_PATH_BYTES + 1];
      };

      int phdr_callback(dl_phdr_info* info, size_t, void* data) noexcept
      {
        auto* search = static_cast<phdr_search*>(data);
        for (size_t i = 0; i < info->dlpi_phnum; ++i)
        {
          const ElfW(Phdr) & ph = info->dlpi_phdr[i];
          if (ph.p_type != PT_LOAD)
            continue;
          const uintptr_t start =
              static_cast<uintptr_t>(info->dlpi_addr + ph.p_vaddr);
          if (search->address >= start
              && search->address - start < static_cast<uintptr_t>(ph.p_memsz))
          {
            // Copied here: the name belongs to the loader
            search->found    = true;
            const char* name = info->dlpi_name != nullptr ? info->dlpi_name : "";
            const size_t n   = ::strlen(name);
            search->too_long = n > MAX_PATH_BYTES;
            if (!search->too_long)
              ::memcpy(search->name, name, n + 1);
            return 1;
          }
        }
        return 0;
      }
#endif
    } // namespace

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

      status<dynlib_path_result> platform_dynlib_path_of(
          const void* address, char* out, size_t* size) noexcept
      {
#if OSPRIM_OS_APPLE
        Dl_info info{};
        if (::dladdr(address, &info) == 0 || info.dli_fname == nullptr)
          return path_fail(PR::DYNLIB_PATH_NOT_FOUND);
        if (info.dli_fbase == static_cast<const void*>(::_dyld_get_image_header(0)))
        {
          char raw[PATH_MAX];
          uint32_t capacity = sizeof(raw);
          if (::_NSGetExecutablePath(raw, &capacity) != 0)
            return path_fail(PR::DYNLIB_PATH_PATH_TOO_LONG);
          return path_canonical_report(raw, false, out, size);
        }
        return path_canonical_report(info.dli_fname, true, out, size);
#else
        phdr_search search;
        search.address  = reinterpret_cast<uintptr_t>(address);
        search.found    = false;
        search.too_long = false;
        search.name[0]  = '\0';
        ::dl_iterate_phdr(&phdr_callback, &search);
        if (!search.found)
          return path_fail(PR::DYNLIB_PATH_NOT_FOUND);
        if (search.too_long)
          return path_fail(PR::DYNLIB_PATH_PATH_TOO_LONG);
        if (search.name[0] == '\0') // the executable
          return path_canonical_report("/proc/self/exe", false, out, size);
        if (::strstr(search.name, "!/") != nullptr) // inside an APK
          return path_report(search.name, PR::DYNLIB_PATH_IN_ARCHIVE, out, size);
        return path_canonical_report(search.name, false, out, size);
#endif
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
