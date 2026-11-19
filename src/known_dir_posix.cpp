/*****************************************************************/ /**
 * @file   known_dir_posix.cpp
 * @brief  Known directories and backup exclusion on Linux, Android and
 * Apple.
 *
 * - Linux: XDG base directories ($XDG_DATA_HOME, $XDG_STATE_HOME,
 *   $XDG_CACHE_HOME, used only when absolute, as the specification
 *   requires), else under $HOME; $TMPDIR else /tmp; the executable from
 *   /proc/self/exe. Sandboxed under Flatpak ($FLATPAK_ID) and Snap ($SNAP).
 *   Backup exclusion: a CACHEDIR.TAG (bford.info/cachedir).
 * - Android: everything under the application's data root, taken from an
 *   overridden DATA named "files", else from $TMPDIR = <root>/cache
 *   (Android 13+), checked to belong to the process's user.
 * - Apple: under $HOME (the container when sandboxed); the temporary
 *   directory from confstr; the executable from _NSGetExecutablePath; the
 *   resources from the main CFBundle. Backup exclusion:
 *   kCFURLIsExcludedFromBackupKey.
 * - Every path comes from the snapshot taken by initialize(), except the
 *   executable, the resources and (Apple) the temporary directory, which
 *   are resolved at each call.
 *********************************************************************/
#include "internal.hpp"

#if OSPRIM_OS_WINDOWS
  #error "known_dir_posix.cpp is for POSIX platforms only"
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <initializer_list>

#if OSPRIM_OS_APPLE
  #include <CoreFoundation/CoreFoundation.h>
  #include <TargetConditionals.h>
  #include <mach-o/dyld.h>
  #include <sys/param.h>
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
      using BR = dir_set_backup_excluded_result;

      template<class R>
      status<R> fail(R r, int err) noexcept
      {
        return {r, native_error_kind::NATIVE_ERROR_POSIX_ERRNO, err};
      }

      template<class R>
      status<R> fail(R r) noexcept
      {
        return {r, native_error_kind::NATIVE_ERROR_NONE, 0};
      }

      known_dir_slot& slot(known_dir_table& t, known_dir d) noexcept
      {
        return t.slots[static_cast<size_t>(d)];
      }

      /// @brief `n` without trailing separators ("/" stays).
      size_t trim(const char* p, size_t n) noexcept
      {
        while (n > 1 && p[n - 1] == '/')
          --n;
        return n;
      }

      /// @brief Size of the parent of an absolute path ("/" for "/x").
      size_t parent_size(const char* p, size_t n) noexcept
      {
        size_t i = n;
        while (i > 0 && p[i - 1] != '/')
          --i;
        return i <= 1 ? 1 : i - 1;
      }

      /// @brief The last component of an absolute path is `name`.
      [[maybe_unused]] bool last_is(
          const char* p, size_t n, const char* name) noexcept
      {
        const size_t k = ::strlen(name);
        return n > k && p[n - k - 1] == '/' && ::memcmp(p + n - k, name, k) == 0;
      }

      /// @brief An environment variable usable as a directory: absolute,
      /// UTF-8, without trailing separators.
      /// @return its size, 0 if unset or unusable
      size_t env_dir(const char* name, const char** out) noexcept
      {
        const char* v = ::getenv(name);
        if (v == nullptr || v[0] != '/')
          return 0;
        const size_t n = ::strlen(v);
        if (!utf8_valid(v, n))
          return 0;
        *out = v;
        return trim(v, n);
      }

      [[maybe_unused]] bool env_set(const char* name) noexcept
      {
        const char* v = ::getenv(name);
        return v != nullptr && v[0] != '\0';
      }

      /// @brief Writes the first `n` bytes of `p` as a reported path.
      [[maybe_unused]] status<GR> report_path(
          const char* p, size_t n, char* out, size_t* size) noexcept
      {
        if (n > MAX_PATH_BYTES)
          return fail(GR::KNOWN_DIR_GET_PATH_TOO_LONG);
        if (!utf8_valid(p, n))
          return fail(GR::KNOWN_DIR_GET_SYSTEM_ERROR, EILSEQ);
        ::memcpy(out, p, n);
        out[n] = '\0';
        *size  = n;
        return {};
      }

      template<class R>
      status<R> map_write_error(int e, R access, R no_space, R other) noexcept
      {
        switch (e)
        {
        case EACCES:
        case EPERM:
        case EROFS:
          return fail(access, e);
        case ENOSPC:
        case EDQUOT:
          return fail(no_space, e);
        default:
          return fail(other, e);
        }
      }

#if OSPRIM_OS_LINUX || OSPRIM_OS_ANDROID
      /// @brief The directory of the executable.
      [[maybe_unused]] status<GR> executable_dir(char* out, size_t* size) noexcept
      {
        char buffer[MAX_PATH_BYTES + 2];
        const ssize_t r = ::readlink("/proc/self/exe", buffer, sizeof(buffer));
        if (r < 0)
          return fail(GR::KNOWN_DIR_GET_SYSTEM_ERROR, errno);
        size_t n = static_cast<size_t>(r);
        if (n > MAX_PATH_BYTES)
          return fail(GR::KNOWN_DIR_GET_PATH_TOO_LONG);
        // Replaced or removed since it started: "/x/game (deleted)"
        static constexpr char deleted[] = " (deleted)";
        constexpr size_t deleted_size   = sizeof(deleted) - 1;
        if (n > deleted_size
            && ::memcmp(buffer + n - deleted_size, deleted, deleted_size) == 0)
          n -= deleted_size;
        return report_path(buffer, parent_size(buffer, n), out, size);
      }
#endif

#if OSPRIM_OS_LINUX
      void xdg(
          known_dir_table& t, known_dir d, const char* variable, const char* home,
          size_t home_size, const char* suffix) noexcept
      {
        known_dir_slot& s = slot(t, d);
        if (s.set)
          return;
        const char* v  = nullptr;
        const size_t n = env_dir(variable, &v);
        if (n != 0)
          known_dir_set(s, v, n);
        else if (home_size != 0)
          known_dir_join(s, home, home_size == 1 ? 0 : home_size, suffix);
        else
          s.error = GR::KNOWN_DIR_GET_NOT_CONFIGURED;
      }
#endif

#if OSPRIM_OS_ANDROID
      /// @brief A directory owned by the process's user.
      bool owned_dir(const char* path) noexcept
      {
        struct stat st{};
        return ::stat(path, &st) == 0 && S_ISDIR(st.st_mode)
               && st.st_uid == ::getuid();
      }
#endif

#if OSPRIM_OS_APPLE
      /// @brief The canonical form of a terminated path into `out`.
      status<GR> real_dir(
          const char* path, bool parent, char* out, size_t* size) noexcept
      {
        char real[PATH_MAX];
        if (::realpath(path, real) == nullptr)
          return fail(GR::KNOWN_DIR_GET_SYSTEM_ERROR, errno);
        size_t n = trim(real, ::strlen(real));
        if (parent)
          n = parent_size(real, n);
        return report_path(real, n, out, size);
      }
#endif

#if OSPRIM_OS_LINUX
      /// @brief Cache directory tags: the first 43 bytes are the signature.
      constexpr char TAG_NAME[]      = "CACHEDIR.TAG";
      constexpr char TAG_SIGNATURE[] = "Signature: 8a477f597d28d172789f06886806bc55";
      constexpr size_t SIGNATURE_SIZE = sizeof(TAG_SIGNATURE) - 1;
      constexpr char TAG_CONTENT[] =
          "Signature: 8a477f597d28d172789f06886806bc55\n"
          "# This file is a cache directory tag created by osprim.\n"
          "# For information about cache directory tags, see:\n"
          "#\thttps://bford.info/cachedir/\n";

      enum class tag_state : uint8_t
      {
        MISSING,
        OURS,   ///< A regular file starting with the signature
        OTHER,  ///< Anything else under that name
        FAILED, ///< See errno
      };

      tag_state read_tag(int dir) noexcept
      {
        int fd = -1;
        do
        {
          fd = ::openat(
              dir, TAG_NAME, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        } while (fd < 0 && errno == EINTR);
        if (fd < 0)
          return errno == ENOENT  ? tag_state::MISSING
                 : errno == ELOOP ? tag_state::OTHER
                                  : tag_state::FAILED;
        tag_state state = tag_state::OTHER;
        struct stat st{};
        if (::fstat(fd, &st) == 0 && S_ISREG(st.st_mode))
        {
          char head[SIGNATURE_SIZE];
          size_t got = 0;
          while (got < SIGNATURE_SIZE)
          {
            const ssize_t r = ::read(fd, head + got, SIGNATURE_SIZE - got);
            if (r < 0 && errno == EINTR)
              continue;
            if (r <= 0)
              break;
            got += static_cast<size_t>(r);
          }
          if (got == SIGNATURE_SIZE
              && ::memcmp(head, TAG_SIGNATURE, SIGNATURE_SIZE) == 0)
            state = tag_state::OURS;
        }
        ::close(fd);
        return state;
      }

      status<BR> write_tag(int dir) noexcept
      {
        for (int attempt = 0; attempt < 2; ++attempt)
        {
          int fd = -1;
          do
          {
            fd = ::openat(
                dir, TAG_NAME, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                0644);
          } while (fd < 0 && errno == EINTR);
          if (fd >= 0)
          {
            constexpr size_t total = sizeof(TAG_CONTENT) - 1;
            size_t done            = 0;
            int err                = 0;
            while (done < total)
            {
              const ssize_t w = ::write(fd, TAG_CONTENT + done, total - done);
              if (w < 0)
              {
                if (errno == EINTR)
                  continue;
                err = errno;
                break;
              }
              done += static_cast<size_t>(w);
            }
            ::close(fd);
            if (err == 0)
              return {};
            ::unlinkat(dir, TAG_NAME, 0);
            return map_write_error(
                err, BR::DIR_SET_BACKUP_EXCLUDED_ACCESS_DENIED,
                BR::DIR_SET_BACKUP_EXCLUDED_NO_SPACE,
                BR::DIR_SET_BACKUP_EXCLUDED_SYSTEM_ERROR);
          }
          const int e = errno;
          if (e != EEXIST)
            return map_write_error(
                e, BR::DIR_SET_BACKUP_EXCLUDED_ACCESS_DENIED,
                BR::DIR_SET_BACKUP_EXCLUDED_NO_SPACE,
                BR::DIR_SET_BACKUP_EXCLUDED_SYSTEM_ERROR);
          switch (read_tag(dir))
          {
          case tag_state::OURS:
            return {};
          case tag_state::MISSING: // removed meanwhile: create it again
            break;
          case tag_state::OTHER:
            return fail(BR::DIR_SET_BACKUP_EXCLUDED_CONFLICT);
          case tag_state::FAILED:
            return map_write_error(
                errno, BR::DIR_SET_BACKUP_EXCLUDED_ACCESS_DENIED,
                BR::DIR_SET_BACKUP_EXCLUDED_NO_SPACE,
                BR::DIR_SET_BACKUP_EXCLUDED_SYSTEM_ERROR);
          }
        }
        return fail(BR::DIR_SET_BACKUP_EXCLUDED_CONFLICT);
      }

      status<BR> remove_tag(int dir) noexcept
      {
        switch (read_tag(dir))
        {
        case tag_state::MISSING:
          return {};
        case tag_state::OTHER:
          return fail(BR::DIR_SET_BACKUP_EXCLUDED_CONFLICT);
        case tag_state::FAILED:
          break;
        case tag_state::OURS:
          if (::unlinkat(dir, TAG_NAME, 0) == 0 || errno == ENOENT)
            return {};
          break;
        }
        return map_write_error(
            errno, BR::DIR_SET_BACKUP_EXCLUDED_ACCESS_DENIED,
            BR::DIR_SET_BACKUP_EXCLUDED_NO_SPACE,
            BR::DIR_SET_BACKUP_EXCLUDED_SYSTEM_ERROR);
      }
#endif

#if OSPRIM_OS_APPLE
      // NSCocoaErrorDomain codes (Foundation's FoundationErrors.h)
      constexpr CFIndex COCOA_NO_SUCH_FILE        = 4;
      constexpr CFIndex COCOA_READ_NO_PERMISSION  = 257;
      constexpr CFIndex COCOA_READ_NO_SUCH_FILE   = 260;
      constexpr CFIndex COCOA_WRITE_NO_PERMISSION = 513;
      constexpr CFIndex COCOA_WRITE_OUT_OF_SPACE  = 640;
      constexpr CFIndex COCOA_WRITE_READ_ONLY     = 642;

      status<BR> cf_error(CFErrorRef err) noexcept
      {
        status<BR> r = fail(BR::DIR_SET_BACKUP_EXCLUDED_SYSTEM_ERROR);
        if (err == nullptr)
          return r;
        const CFIndex code       = CFErrorGetCode(err);
        const CFStringRef domain = CFErrorGetDomain(err);
        if (domain != nullptr && CFEqual(domain, kCFErrorDomainPOSIX))
        {
          const int e = static_cast<int>(code);
          r = e == ENOENT ? fail(BR::DIR_SET_BACKUP_EXCLUDED_DELETED, e)
                          : map_write_error(
                                e, BR::DIR_SET_BACKUP_EXCLUDED_ACCESS_DENIED,
                                BR::DIR_SET_BACKUP_EXCLUDED_NO_SPACE,
                                BR::DIR_SET_BACKUP_EXCLUDED_SYSTEM_ERROR);
        }
        else if (domain != nullptr && CFEqual(domain, kCFErrorDomainCocoa))
        {
          if (code == COCOA_NO_SUCH_FILE || code == COCOA_READ_NO_SUCH_FILE)
            r = fail(BR::DIR_SET_BACKUP_EXCLUDED_DELETED);
          else if (
              code == COCOA_READ_NO_PERMISSION || code == COCOA_WRITE_NO_PERMISSION
              || code == COCOA_WRITE_READ_ONLY)
            r = fail(BR::DIR_SET_BACKUP_EXCLUDED_ACCESS_DENIED);
          else if (code == COCOA_WRITE_OUT_OF_SPACE)
            r = fail(BR::DIR_SET_BACKUP_EXCLUDED_NO_SPACE);
        }
        CFRelease(err);
        return r;
      }
#endif
    } // namespace

    namespace detail
    {
      bool platform_known_dir_normalize(
          const char* path, size_t size, char* out, size_t* out_size) noexcept
      {
        if (path[0] != '/')
          return false;
        const size_t n = trim(path, size);
        ::memcpy(out, path, n);
        out[n]    = '\0';
        *out_size = n;
        return true;
      }

      void platform_known_dir_initialize(known_dir_table& t) noexcept
      {
#if OSPRIM_OS_LINUX
        const char* home    = nullptr;
        const size_t home_n = env_dir("HOME", &home);
        xdg(t, known_dir::KNOWN_DIR_DATA, "XDG_DATA_HOME", home, home_n,
            "/.local/share");
        xdg(t, known_dir::KNOWN_DIR_STATE, "XDG_STATE_HOME", home, home_n,
            "/.local/state");
        xdg(t, known_dir::KNOWN_DIR_CACHE, "XDG_CACHE_HOME", home, home_n,
            "/.cache");
        known_dir_slot& temp = slot(t, known_dir::KNOWN_DIR_TEMP);
        if (!temp.set)
        {
          const char* v  = nullptr;
          const size_t n = env_dir("TMPDIR", &v);
          if (n != 0)
            known_dir_set(temp, v, n);
          else
            known_dir_set(temp, "/tmp", 4);
        }
        if (env_set("FLATPAK_ID") || env_set("SNAP"))
          t.flags |= KNOWN_DIR_FLAG_SANDBOXED;
#elif OSPRIM_OS_ANDROID
        char root[MAX_PATH_BYTES + 1];
        size_t root_n        = 0;
        bool from_override   = false;
        known_dir_slot& data = slot(t, known_dir::KNOWN_DIR_DATA);
        if (data.overridden && last_is(data.path, data.size, "files"))
        {
          root_n = parent_size(data.path, data.size);
          ::memcpy(root, data.path, root_n);
          root[root_n]  = '\0';
          from_override = true;
        }
        else
        {
          const char* tmp = nullptr;
          const size_t n  = env_dir("TMPDIR", &tmp);
          if (n != 0 && last_is(tmp, n, "cache"))
          {
            char cache[MAX_PATH_BYTES + 1];
            ::memcpy(cache, tmp, n);
            cache[n]        = '\0';
            const size_t pn = parent_size(tmp, n);
            ::memcpy(root, tmp, pn);
            root[pn] = '\0';
            if (owned_dir(cache) && owned_dir(root))
              root_n = pn;
          }
        }
        auto fill = [&](known_dir d, const char* suffix) noexcept
        {
          known_dir_slot& s = slot(t, d);
          if (s.set)
            return;
          if (root_n == 0)
          {
            s.error = GR::KNOWN_DIR_GET_NOT_CONFIGURED;
            return;
          }
          known_dir_join(s, root, root_n, suffix);
          s.overridden = from_override && s.set;
        };
        fill(known_dir::KNOWN_DIR_DATA, "/files");
        fill(known_dir::KNOWN_DIR_STATE, "/no_backup");
        fill(known_dir::KNOWN_DIR_CACHE, "/cache");
        fill(known_dir::KNOWN_DIR_TEMP, "/cache");
        for (known_dir d :
             {known_dir::KNOWN_DIR_EXECUTABLE, known_dir::KNOWN_DIR_RESOURCES})
          if (!slot(t, d).set)
            slot(t, d).error = GR::KNOWN_DIR_GET_UNSUPPORTED;
        t.flags |= KNOWN_DIR_FLAG_SANDBOXED;
#else // Apple
        const char* home    = nullptr;
        const size_t home_n = env_dir("HOME", &home);
        auto under_home     = [&](known_dir d, const char* suffix) noexcept
        {
          known_dir_slot& s = slot(t, d);
          if (s.set)
            return;
          if (home_n == 0)
            s.error = GR::KNOWN_DIR_GET_NOT_CONFIGURED;
          else
            known_dir_join(s, home, home_n == 1 ? 0 : home_n, suffix);
        };
        under_home(known_dir::KNOWN_DIR_DATA, "/Library/Application Support");
        under_home(known_dir::KNOWN_DIR_STATE, "/Library/Application Support");
        under_home(known_dir::KNOWN_DIR_CACHE, "/Library/Caches");
  #if TARGET_OS_OSX
        if (env_set("APP_SANDBOX_CONTAINER_ID"))
          t.flags |= KNOWN_DIR_FLAG_SANDBOXED;
  #else
        t.flags |= KNOWN_DIR_FLAG_SANDBOXED;
  #endif
#endif
      }

      void platform_known_dir_deinitialize() noexcept
      {
      }

      status<known_dir_get_result> platform_known_dir_resolve(
          known_dir which, char* out, size_t* size) noexcept
      {
#if OSPRIM_OS_LINUX
        if (which == known_dir::KNOWN_DIR_EXECUTABLE
            || which == known_dir::KNOWN_DIR_RESOURCES)
          return executable_dir(out, size);
        return fail(GR::KNOWN_DIR_GET_NOT_CONFIGURED);
#elif OSPRIM_OS_ANDROID
        (void)which;
        (void)out;
        (void)size;
        return fail(GR::KNOWN_DIR_GET_NOT_CONFIGURED);
#else // Apple
        switch (which)
        {
        case known_dir::KNOWN_DIR_TEMP:
        {
          char buffer[MAX_PATH_BYTES + 2];
          const size_t r =
              ::confstr(_CS_DARWIN_USER_TEMP_DIR, buffer, sizeof(buffer));
          if (r == 0)
            return fail(GR::KNOWN_DIR_GET_SYSTEM_ERROR, errno);
          if (r > sizeof(buffer))
            return fail(GR::KNOWN_DIR_GET_PATH_TOO_LONG);
          return report_path(buffer, trim(buffer, r - 1), out, size);
        }
        case known_dir::KNOWN_DIR_EXECUTABLE:
        {
          char raw[PATH_MAX];
          uint32_t capacity = sizeof(raw);
          if (::_NSGetExecutablePath(raw, &capacity) != 0)
            return fail(GR::KNOWN_DIR_GET_PATH_TOO_LONG);
          return real_dir(raw, true, out, size);
        }
        case known_dir::KNOWN_DIR_RESOURCES:
        {
          const CFBundleRef bundle = CFBundleGetMainBundle();
          if (bundle == nullptr)
            return fail(GR::KNOWN_DIR_GET_SYSTEM_ERROR);
          const CFURLRef url = CFBundleCopyResourcesDirectoryURL(bundle);
          if (url == nullptr)
            return fail(GR::KNOWN_DIR_GET_SYSTEM_ERROR);
          UInt8 fs[PATH_MAX];
          const Boolean ok = CFURLGetFileSystemRepresentation(
              url, true, fs, static_cast<CFIndex>(sizeof(fs)));
          CFRelease(url);
          if (!ok)
            return fail(GR::KNOWN_DIR_GET_PATH_TOO_LONG);
          return real_dir(reinterpret_cast<const char*>(fs), false, out, size);
        }
        default:
          return fail(GR::KNOWN_DIR_GET_NOT_CONFIGURED);
        }
#endif
      }

      void platform_known_dir_traits(
          known_dir which, uint32_t flags, known_dir_info* info) noexcept
      {
        const bool install   = which == known_dir::KNOWN_DIR_EXECUTABLE
                               || which == known_dir::KNOWN_DIR_RESOURCES;
        const bool sandboxed = (flags & KNOWN_DIR_FLAG_SANDBOXED) != 0;
#if OSPRIM_OS_LINUX
        // TEMP stays shared: whether a sandbox gives a private /tmp is not
        // known here
        info->app_private =
            install || (sandboxed && which != known_dir::KNOWN_DIR_TEMP);
        info->backed_up     = false;
        info->may_be_purged = which == known_dir::KNOWN_DIR_TEMP;
#elif OSPRIM_OS_ANDROID
        (void)sandboxed;
        (void)install;
        info->app_private   = true;
        info->backed_up     = which == known_dir::KNOWN_DIR_DATA;
        info->may_be_purged = which == known_dir::KNOWN_DIR_CACHE
                              || which == known_dir::KNOWN_DIR_TEMP;
#else // Apple
        info->app_private   = install || sandboxed;
        info->backed_up     = which == known_dir::KNOWN_DIR_DATA
                              || which == known_dir::KNOWN_DIR_STATE;
        info->may_be_purged = which == known_dir::KNOWN_DIR_CACHE
                              || which == known_dir::KNOWN_DIR_TEMP;
#endif
      }

      bool platform_known_dir_same(
          const char* a, size_t an, const char* b, size_t bn) noexcept
      {
        return an == bn && ::memcmp(a, b, an) == 0;
      }

      mkdir_outcome platform_known_dir_mkdir(
          const char* path, size_t, status<known_dir_open_result>* error) noexcept
      {
        if (::mkdir(path, 0700) == 0)
          return mkdir_outcome::CREATED;
        const int e = errno;
        switch (e)
        {
        case EEXIST:
          return mkdir_outcome::EXISTS;
        case ENOENT:
          return mkdir_outcome::PARENT_MISSING;
        case ENOTDIR:
          *error = fail(OR::KNOWN_DIR_OPEN_NOT_A_DIRECTORY, e);
          return mkdir_outcome::FAILED;
        case ENAMETOOLONG:
          *error = fail(OR::KNOWN_DIR_OPEN_PATH_TOO_LONG, e);
          return mkdir_outcome::FAILED;
        default:
          *error = map_write_error(
              e, OR::KNOWN_DIR_OPEN_ACCESS_DENIED, OR::KNOWN_DIR_OPEN_NO_SPACE,
              OR::KNOWN_DIR_OPEN_SYSTEM_ERROR);
          return mkdir_outcome::FAILED;
        }
      }

      status<dir_set_backup_excluded_result> platform_dir_set_backup_excluded(
          native_dir_t d, bool excluded) noexcept
      {
#if OSPRIM_OS_LINUX
        return excluded ? write_tag(d) : remove_tag(d);
#elif OSPRIM_OS_ANDROID
        (void)d;
        (void)excluded;
        return fail(BR::DIR_SET_BACKUP_EXCLUDED_UNSUPPORTED);
#else // Apple
        char path[MAXPATHLEN];
        if (::fcntl(d, F_GETPATH, path) == -1)
        {
          const int e = errno;
          return e == ENOENT ? fail(BR::DIR_SET_BACKUP_EXCLUDED_DELETED, e)
                             : fail(BR::DIR_SET_BACKUP_EXCLUDED_SYSTEM_ERROR, e);
        }
        // The name must still lead to this directory
        struct stat by_handle{};
        struct stat by_name{};
        if (::fstat(d, &by_handle) != 0)
          return fail(BR::DIR_SET_BACKUP_EXCLUDED_SYSTEM_ERROR, errno);
        if (::lstat(path, &by_name) != 0 || by_name.st_dev != by_handle.st_dev
            || by_name.st_ino != by_handle.st_ino)
          return fail(BR::DIR_SET_BACKUP_EXCLUDED_DELETED);
        const CFURLRef url = CFURLCreateFromFileSystemRepresentation(
            kCFAllocatorDefault, reinterpret_cast<const UInt8*>(path),
            static_cast<CFIndex>(::strlen(path)), true);
        if (url == nullptr)
          return fail(BR::DIR_SET_BACKUP_EXCLUDED_SYSTEM_ERROR);
        CFErrorRef err   = nullptr;
        const Boolean ok = CFURLSetResourcePropertyForKey(
            url, kCFURLIsExcludedFromBackupKey,
            excluded ? kCFBooleanTrue : kCFBooleanFalse, &err);
        CFRelease(url);
        if (ok)
          return {};
        return cf_error(err);
#endif
      }
    } // namespace detail
  } // namespace v0_1
} // namespace osprim
