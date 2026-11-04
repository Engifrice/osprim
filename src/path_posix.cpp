/*****************************************************************/ /**
 * @file   path_posix.cpp
 * @brief  Paths and directories on Linux, Android and Apple.
 *
 * - Every operation is an *at call on the parent of the last component.
 *   NORMAL lets the kernel resolve the whole name. NO_SYMLINKS opens the
 *   parent first without following any symlink, then acts on the last
 *   component without following it:
 *   - Linux: openat2(RESOLVE_NO_SYMLINKS) (5.6+), else
 *   - Apple: openat(O_NOFOLLOW_ANY) (macOS 11, iOS 14),
 *   - otherwise (Android, older kernels, seccomp refusals): one component
 *     at a time with O_NOFOLLOW. Android never calls openat2: bionic does
 *     not expose it, and the app seccomp filter may kill the process.
 * - Directory handles: O_RDONLY | O_DIRECTORY | O_CLOEXEC.
 * - Rename without replacing: renameat2(RENAME_NOREPLACE) (Linux 3.15,
 *   Android 11), else link then unlink (files only); Apple: renameatx_np
 *   (RENAME_EXCL). Durable: fsync of both parents (Apple: F_FULLFSYNC).
 * - Listing: getdents64 (Linux, Android), getattrlistbulk (Apple), into
 *   the caller's buffer.
 * - Canonical path: readlink of /proc/self/fd/N (Linux), F_GETPATH (Apple).
 *********************************************************************/
#if !defined(_GNU_SOURCE) && !defined(__APPLE__)
  #define _GNU_SOURCE 1 // O_PATH
#endif

#include "internal.hpp"

#if OSPRIM_OS_WINDOWS
  #error "path_posix.cpp is for POSIX platforms only"
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>

#if OSPRIM_OS_APPLE
  #include <sys/attr.h>
  #include <sys/param.h>
  #ifndef O_NOFOLLOW_ANY
    #define O_NOFOLLOW_ANY 0x20000000
  #endif
  #ifndef RENAME_EXCL
    #define RENAME_EXCL 0x00000004
  #endif
extern "C" int renameatx_np(int, const char*, int, const char*, unsigned int);
#else
  #include <stdlib.h>
  #include <sys/syscall.h>
#endif
#if OSPRIM_OS_ANDROID
  #include <sys/system_properties.h>
#endif

namespace osprim
{
  inline namespace v0_1
  {
    namespace
    {
#if OSPRIM_OS_APPLE
      // enum vtype values of sys/vnode.h (not in the iOS SDK)
      constexpr fsobj_type_t APPLE_VREG = 1;
      constexpr fsobj_type_t APPLE_VDIR = 2;
      constexpr fsobj_type_t APPLE_VLNK = 5;
#endif

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

      bool nofollow(path_resolve r) noexcept
      {
        return r == path_resolve::PATH_RESOLVE_NO_SYMLINKS;
      }

#if OSPRIM_OS_LINUX || OSPRIM_OS_ANDROID
      /// @brief Android API level (0 elsewhere).
      int android_sdk() noexcept
      {
  #if OSPRIM_OS_ANDROID
        static const int sdk = []
        {
          char v[PROP_VALUE_MAX] = {};
          return ::__system_property_get("ro.build.version.sdk", v) > 0 ? ::atoi(v)
                                                                        : 0;
        }();
        return sdk;
  #else
        return 0;
  #endif
      }
#endif

      /// @brief The parent of the last component, and that component.
      struct parent
      {
        int fd           = AT_FDCWD;
        bool owned       = false;
        const char* last = ".";
        char storage[MAX_PATH_BYTES + 1];

        parent() noexcept                = default;
        parent(const parent&)            = delete;
        parent& operator=(const parent&) = delete;
        ~parent()
        {
          if (owned)
            ::close(fd);
        }
      };

      /// @brief Opens directory `name` (terminated, relative to `at`, or
      /// absolute) without following any symlink in it.
      /// @return the descriptor, or -1 with errno (ELOOP: a symlink)
      int open_dir_nofollow(int at, const char* name, int flags) noexcept
      {
#if OSPRIM_OS_APPLE
        int fd = -1;
        do
        {
          fd = ::openat(at, name, flags | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW_ANY);
        } while (fd < 0 && errno == EINTR);
        return fd;
#else
  #if OSPRIM_OS_LINUX && defined(__NR_openat2)
        // struct open_how: flags, mode, resolve (u64 each)
        static std::atomic<bool> no_openat2{false};
        if (!no_openat2.load(std::memory_order_relaxed))
        {
          const uint64_t how[3] = {
              static_cast<uint64_t>(flags | O_DIRECTORY | O_CLOEXEC), 0,
              0x04 /* RESOLVE_NO_SYMLINKS */};
          for (int attempt = 0; attempt < 16; ++attempt)
          {
            const long fd = ::syscall(__NR_openat2, at, name, how, sizeof(how));
            if (fd >= 0)
              return static_cast<int>(fd);
            if (errno == EINTR || errno == EAGAIN)
              continue;
            if (errno == ENOSYS || errno == EPERM)
            {
              no_openat2.store(
                  true,
                  std::memory_order_relaxed); // old kernel, or a seccomp filter
              break;
            }
            return -1;
          }
          if (!no_openat2.load(std::memory_order_relaxed))
            return -1;
        }
  #endif
        // One component at a time
        const bool absolute = name[0] == '/';
        int cur = absolute ? ::open("/", O_PATH | O_DIRECTORY | O_CLOEXEC) : at;
        bool cur_owned = absolute;
        if (cur < 0)
          return -1;
        char comp[MAX_PATH_BYTES + 1];
        const char* p = name;
        for (;;)
        {
          while (*p == '/')
            ++p;
          const char* end = p;
          while (*end != '\0' && *end != '/')
            ++end;
          const size_t n   = static_cast<size_t>(end - p);
          const bool final = *end == '\0' || end[strspn(end, "/")] == '\0';
          if (n == 0)
            break;
          ::memcpy(comp, p, n);
          comp[n]  = '\0';
          int next = -1;
          do
          {
            next = ::openat(
                cur, comp,
                (final ? flags : O_PATH) | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
          } while (next < 0 && errno == EINTR);
          if (next < 0)
          {
            int e = errno;
            struct stat st{};
            if (e == ENOTDIR && ::fstatat(cur, comp, &st, AT_SYMLINK_NOFOLLOW) == 0
                && S_ISLNK(st.st_mode))
              e = ELOOP;
            if (cur_owned)
              ::close(cur);
            errno = e;
            return -1;
          }
          if (cur_owned)
            ::close(cur);
          cur       = next;
          cur_owned = true;
          p         = end;
          if (final)
            break;
        }
        if (!cur_owned)
        {
          // Empty name: the directory itself
          return ::openat(at, ".", flags | O_DIRECTORY | O_CLOEXEC);
        }
        return cur;
#endif
      }

      /// @brief Resolves the parent of the last component of `p`.
      /// @return 0, or an errno (ELOOP: a symlink under NO_SYMLINKS)
      int resolve_parent(
          const detail::path_ref& p, path_resolve resolve, parent* out) noexcept
      {
        const int base    = p.absolute ? AT_FDCWD : p.dir;
        const char* name  = p.size != 0 ? p.name : ".";
        const char* slash = ::strrchr(name, '/');
        if (!nofollow(resolve) || slash == nullptr || (p.absolute && p.size == 1))
        {
          // NORMAL: the kernel resolves the whole name. One component: the parent is `dir`.
          out->fd   = base;
          out->last = name;
          return 0;
        }
        // NO_SYMLINKS with several components: open the parent safely
        const size_t parent_size =
            slash == name ? 1 : static_cast<size_t>(slash - name);
        ::memcpy(out->storage, name, parent_size);
        out->storage[parent_size] = '\0';
        const int fd              = open_dir_nofollow(base, out->storage, O_RDONLY);
        if (fd < 0)
          return errno;
        out->fd    = fd;
        out->owned = true;
        out->last  = slash + 1;
        return 0;
      }

      path_type type_of(mode_t m) noexcept
      {
        if (S_ISREG(m))
          return path_type::PATH_TYPE_FILE;
        if (S_ISDIR(m))
          return path_type::PATH_TYPE_DIRECTORY;
        if (S_ISLNK(m))
          return path_type::PATH_TYPE_SYMLINK;
        return path_type::PATH_TYPE_OTHER;
      }

      int64_t timespec_ns(const struct timespec& t) noexcept
      {
        return static_cast<int64_t>(t.tv_sec) * 1'000'000'000
               + static_cast<int64_t>(t.tv_nsec);
      }

      /// @brief Makes the entries of the directory holding `last` (relative
      /// to `fd`, or absolute) durable: fsync, F_FULLFSYNC on Apple.
      /// @return 0 or an errno
      int sync_parent(int fd, const char* last) noexcept
      {
        char buf[MAX_PATH_BYTES + 1];
        const char* slash = ::strrchr(last, '/');
        const char* dir   = ".";
        if (slash != nullptr)
        {
          const size_t n = slash == last ? 1 : static_cast<size_t>(slash - last);
          ::memcpy(buf, last, n);
          buf[n] = '\0';
          dir    = buf;
        }
        // Reopened: O_PATH descriptors (Linux walks) cannot be synced
        const int d = ::openat(fd, dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (d < 0)
          return errno;
        int r = 0;
#if OSPRIM_OS_APPLE
        r = ::fcntl(d, F_FULLFSYNC);
        if (r != 0 && (errno == ENOTSUP || errno == ENOTTY || errno == EINVAL))
          r = ::fsync(d);
#else
        do
        {
          r = ::fsync(d);
        } while (r != 0 && errno == EINTR);
#endif
        const int e = r != 0 ? errno : 0;
        ::close(d);
        return e;
      }
    } // namespace

    namespace detail
    {
      /*****************************/
      /* DIRECTORY HANDLES         */
      /*****************************/

      status<dir_open_result> platform_dir_open(
          const path_ref& p, path_resolve resolve, native_dir_t* out) noexcept
      {
        using R = dir_open_result;
        int fd  = -1;
        if (nofollow(resolve))
          fd = open_dir_nofollow(
              p.absolute ? AT_FDCWD : p.dir, p.size != 0 ? p.name : ".", O_RDONLY);
        else
        {
          do
          {
            fd = ::openat(
                p.absolute ? AT_FDCWD : p.dir, p.size != 0 ? p.name : ".",
                O_RDONLY | O_DIRECTORY | O_CLOEXEC);
          } while (fd < 0 && errno == EINTR);
        }
        if (fd >= 0)
        {
          *out = fd;
          return {};
        }
        const int e = errno;
        switch (e)
        {
        case ENOENT:
          return fail(R::DIR_OPEN_NOT_FOUND, e);
        case ENOTDIR:
        {
          // The last component is not a directory, or a parent is not
          struct stat st{};
          if (::fstatat(
                  p.absolute ? AT_FDCWD : p.dir, p.name, &st,
                  nofollow(resolve) ? AT_SYMLINK_NOFOLLOW : 0)
              == 0)
          {
            if (S_ISLNK(st.st_mode))
              return fail(R::DIR_OPEN_SYMLINK, e);
            return fail(R::DIR_OPEN_NOT_A_DIRECTORY, e);
          }
          return fail(R::DIR_OPEN_NOT_FOUND, e);
        }
        case ELOOP:
          return fail(
              nofollow(resolve) ? R::DIR_OPEN_SYMLINK : R::DIR_OPEN_INVALID_PATH, e);
        case EACCES:
        case EPERM:
          return fail(R::DIR_OPEN_ACCESS_DENIED, e);
        case ENAMETOOLONG:
          return fail(R::DIR_OPEN_PATH_TOO_LONG, e);
        case EMFILE:
        case ENFILE:
          return fail(R::DIR_OPEN_TOO_MANY_OPEN, e);
        case EILSEQ:
          return fail(R::DIR_OPEN_INVALID_PATH, e);
        default:
          return fail(R::DIR_OPEN_SYSTEM_ERROR, e);
        }
      }

      void platform_dir_close(native_dir_t d) noexcept
      {
        ::close(d);
      }

      status<dir_adopt_result> platform_dir_adopt(native_dir_t d) noexcept
      {
        struct stat st{};
        if (::fstat(d, &st) != 0)
          return fail(dir_adopt_result::DIR_ADOPT_SYSTEM_ERROR, errno);
        if (!S_ISDIR(st.st_mode))
          return fail(dir_adopt_result::DIR_ADOPT_NOT_A_DIRECTORY);
        return {};
      }

      /*****************************/
      /* FILES                     */
      /*****************************/

      status<file_open_result> platform_file_open_at(
          const path_ref& p, path_resolve resolve, const file_open_desc& desc,
          native_file_t* out) noexcept
      {
        using R = file_open_result;
        parent par;
        const int pe = resolve_parent(p, resolve, &par);
        if (pe != 0)
          return pe == ELOOP ? fail(R::FILE_OPEN_SYMLINK, pe)
                             : posix_open_error(pe, desc.unbuffered);
        const int flags =
            posix_file_flags(desc) | (nofollow(resolve) ? O_NOFOLLOW : 0);
        int fd = -1;
        do
        {
          fd = ::openat(par.fd, par.last, flags, 0666);
        } while (fd < 0 && errno == EINTR);
        if (fd < 0)
        {
          const int e = errno;
          if (e == ELOOP && nofollow(resolve))
            return fail(R::FILE_OPEN_SYMLINK, e);
          return posix_open_error(e, desc.unbuffered);
        }
        return posix_file_finish(fd, desc, out);
      }

      /*****************************/
      /* ENTRY INFORMATION         */
      /*****************************/

      status<path_info_result> platform_path_info(
          const path_ref& p, path_resolve resolve, path_info* out) noexcept
      {
        using R    = path_info_result;
        auto error = [](int e) noexcept -> status<R>
        {
          switch (e)
          {
          case ENOENT:
          case ENOTDIR:
            return fail(R::PATH_INFO_NOT_FOUND, e);
          case EACCES:
          case EPERM:
            return fail(R::PATH_INFO_ACCESS_DENIED, e);
          case ELOOP:
            return fail(R::PATH_INFO_SYMLINK, e);
          case ENAMETOOLONG:
            return fail(R::PATH_INFO_PATH_TOO_LONG, e);
          case EILSEQ:
            return fail(R::PATH_INFO_INVALID_PATH, e);
          default:
            return fail(R::PATH_INFO_SYSTEM_ERROR, e);
          }
        };
        parent par;
        const int pe = resolve_parent(p, resolve, &par);
        if (pe != 0)
          return error(pe);
        struct stat st{};
        if (::fstatat(
                par.fd, par.last, &st, nofollow(resolve) ? AT_SYMLINK_NOFOLLOW : 0)
            != 0)
        {
          const int e = errno;
          if (e == ELOOP && !nofollow(resolve))
            return fail(R::PATH_INFO_INVALID_PATH, e); // a symlink loop
          return error(e);
        }
        out->type = type_of(st.st_mode);
        out->size = S_ISREG(st.st_mode) ? static_cast<uint64_t>(st.st_size) : 0;
#if OSPRIM_OS_APPLE
        out->last_write = wall_instant{timespec_ns(st.st_mtimespec)};
#else
        out->last_write = wall_instant{timespec_ns(st.st_mtim)};
#endif
        out->identity = file_identity{
            static_cast<uint64_t>(st.st_dev), static_cast<uint64_t>(st.st_ino), 0};
        return {};
      }

      /*****************************/
      /* CREATE, REMOVE, RENAME    */
      /*****************************/

      status<dir_create_result> platform_dir_create(
          const path_ref& p, path_resolve resolve, native_dir_t* out) noexcept
      {
        using R    = dir_create_result;
        auto error = [](int e) noexcept -> status<R>
        {
          switch (e)
          {
          case EEXIST:
            return fail(R::DIR_CREATE_ALREADY_EXISTS, e);
          case ENOENT:
          case ENOTDIR:
            return fail(R::DIR_CREATE_NOT_FOUND, e);
          case EACCES:
          case EPERM:
          case EROFS:
            return fail(R::DIR_CREATE_ACCESS_DENIED, e);
          case ELOOP:
            return fail(R::DIR_CREATE_SYMLINK, e);
          case ENAMETOOLONG:
            return fail(R::DIR_CREATE_PATH_TOO_LONG, e);
          case ENOSPC:
          case EDQUOT:
            return fail(R::DIR_CREATE_NO_SPACE, e);
          case EILSEQ:
            return fail(R::DIR_CREATE_INVALID_PATH, e);
          default:
            return fail(R::DIR_CREATE_SYSTEM_ERROR, e);
          }
        };
        parent par;
        const int pe = resolve_parent(p, resolve, &par);
        if (pe != 0)
          return error(pe);
        if (::mkdirat(par.fd, par.last, 0777) != 0)
        {
          const int e = errno;
          if (e == ELOOP && !nofollow(resolve))
            return fail(R::DIR_CREATE_INVALID_PATH, e);
          return error(e);
        }
        if (out != nullptr)
        {
          int fd = -1;
          do
          {
            fd = ::openat(
                par.fd, par.last, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
          } while (fd < 0 && errno == EINTR);
          if (fd < 0)
            return fail(R::DIR_CREATE_SYSTEM_ERROR, errno);
          *out = fd;
        }
        return {};
      }

      status<path_remove_result> platform_path_remove(
          const path_ref& p, path_resolve resolve, path_kind kind) noexcept
      {
        using R = path_remove_result;
        parent par;
        const int pe = resolve_parent(p, resolve, &par);
        int e        = pe;
        if (pe == 0)
        {
          const int flag = kind == path_kind::PATH_KIND_DIRECTORY ? AT_REMOVEDIR : 0;
          if (::unlinkat(par.fd, par.last, flag) == 0)
            return {};
          e = errno;
        }
        switch (e)
        {
        case ENOENT:
          return fail(R::PATH_REMOVE_NOT_FOUND, e);
        case EISDIR:
          return fail(R::PATH_REMOVE_WRONG_KIND, e);
        case ENOTDIR:
        {
          if (pe != 0)
            return fail(R::PATH_REMOVE_NOT_FOUND, e);
          struct stat st{};
          if (::fstatat(par.fd, par.last, &st, AT_SYMLINK_NOFOLLOW) == 0
              && !S_ISDIR(st.st_mode))
            return fail(R::PATH_REMOVE_WRONG_KIND, e);
          return fail(R::PATH_REMOVE_NOT_FOUND, e);
        }
        case ENOTEMPTY:
        case EEXIST:
          return fail(R::PATH_REMOVE_NOT_EMPTY, e);
        case EPERM:
        {
          // Apple: unlink of a directory; elsewhere: permissions (sticky bit)
          struct stat st{};
          if (pe == 0 && kind == path_kind::PATH_KIND_FILE
              && ::fstatat(par.fd, par.last, &st, AT_SYMLINK_NOFOLLOW) == 0
              && S_ISDIR(st.st_mode))
            return fail(R::PATH_REMOVE_WRONG_KIND, e);
          return fail(R::PATH_REMOVE_ACCESS_DENIED, e);
        }
        case EACCES:
        case EROFS:
          return fail(R::PATH_REMOVE_ACCESS_DENIED, e);
        case EBUSY:
          return fail(R::PATH_REMOVE_BUSY, e);
        case ELOOP:
          return fail(
              nofollow(resolve) ? R::PATH_REMOVE_SYMLINK
                                : R::PATH_REMOVE_INVALID_PATH,
              e);
        case ENAMETOOLONG:
          return fail(R::PATH_REMOVE_PATH_TOO_LONG, e);
        case EINVAL:
        case EILSEQ:
          return fail(R::PATH_REMOVE_INVALID_PATH, e);
        default:
          return fail(R::PATH_REMOVE_SYSTEM_ERROR, e);
        }
      }

      status<path_rename_result> platform_path_rename(
          const path_ref& from, const path_ref& to,
          const path_rename_desc& desc) noexcept
      {
        using R    = path_rename_result;
        auto error = [&](int e) noexcept -> status<R>
        {
          switch (e)
          {
          case ENOENT:
            return fail(R::PATH_RENAME_NOT_FOUND, e);
          case EEXIST:
            return fail(
                desc.mode == path_rename_mode::PATH_RENAME_MODE_NO_REPLACE
                    ? R::PATH_RENAME_ALREADY_EXISTS
                    : R::PATH_RENAME_NOT_EMPTY,
                e);
          case ENOTEMPTY:
            return fail(R::PATH_RENAME_NOT_EMPTY, e);
          case EISDIR:
          case ENOTDIR:
            return fail(R::PATH_RENAME_WRONG_KIND, e);
          case EXDEV:
            return fail(R::PATH_RENAME_CROSS_DEVICE, e);
          case EACCES:
          case EPERM:
          case EROFS:
            return fail(R::PATH_RENAME_ACCESS_DENIED, e);
          case EBUSY:
            return fail(R::PATH_RENAME_BUSY, e);
          case ELOOP:
            return fail(
                nofollow(desc.resolve) ? R::PATH_RENAME_SYMLINK
                                       : R::PATH_RENAME_INVALID_PATH,
                e);
          case ENAMETOOLONG:
            return fail(R::PATH_RENAME_PATH_TOO_LONG, e);
          case ENOSPC:
          case EDQUOT:
            return fail(R::PATH_RENAME_NO_SPACE, e);
          case EINVAL:
          case EILSEQ:
            return fail(R::PATH_RENAME_INVALID_PATH, e);
          default:
            return fail(R::PATH_RENAME_SYSTEM_ERROR, e);
          }
        };
        parent a;
        parent b;
        int pe = resolve_parent(from, desc.resolve, &a);
        if (pe == 0)
          pe = resolve_parent(to, desc.resolve, &b);
        if (pe != 0)
          return error(pe);

        if (desc.mode == path_rename_mode::PATH_RENAME_MODE_REPLACE)
        {
          if (::renameat(a.fd, a.last, b.fd, b.last) != 0)
            return error(errno);
        }
        else
        {
#if OSPRIM_OS_APPLE
          if (::renameatx_np(a.fd, a.last, b.fd, b.last, RENAME_EXCL) != 0)
          {
            const int e = errno;
            if (e == ENOTSUP)
              return fail(R::PATH_RENAME_UNSUPPORTED, e);
            return error(e);
          }
#else
          bool done = false;
  #if defined(__NR_renameat2)
          // bionic exposes renameat2 (and seccomp allows it) from Android 11
          if (!OSPRIM_OS_ANDROID || android_sdk() >= 30)
          {
            if (::syscall(
                    __NR_renameat2, a.fd, a.last, b.fd, b.last,
                    1u /* RENAME_NOREPLACE */)
                == 0)
              done = true;
            else if (errno == EINVAL)
              return fail(
                  R::PATH_RENAME_UNSUPPORTED,
                  errno); // the filesystem refuses the flag
            else if (errno != ENOSYS && errno != EPERM)
              return error(errno);
          }
  #endif
          if (!done)
          {
            // Link then unlink: atomic "no replace" for files
            if (::linkat(a.fd, a.last, b.fd, b.last, 0) != 0)
            {
              const int e = errno;
              if (e == EEXIST)
                return fail(R::PATH_RENAME_ALREADY_EXISTS, e);
              if (e == EPERM || e == EOPNOTSUPP || e == EMLINK)
                return fail(
                    R::PATH_RENAME_UNSUPPORTED,
                    e); // directories, filesystems without hard links
              return error(e);
            }
            if (::unlinkat(a.fd, a.last, 0) != 0)
              return error(errno);
          }
#endif
        }

        if (desc.durable)
        {
          // Both directories: the old name's removal and the new name's entry
          int e = sync_parent(b.fd, b.last);
          if (e == 0)
            e = sync_parent(a.fd, a.last);
          if (e != 0)
            return fail(R::PATH_RENAME_NOT_DURABLE, e);
        }
        return {};
      }

      /*****************************/
      /* SYMLINKS                  */
      /*****************************/

      status<path_read_link_result> platform_path_read_link(
          const path_ref& p, path_resolve resolve, char* buffer, size_t capacity,
          size_t* needed, link_info* info) noexcept
      {
        using R    = path_read_link_result;
        auto error = [&](int e) noexcept -> status<R>
        {
          switch (e)
          {
          case ENOENT:
          case ENOTDIR:
            return fail(R::PATH_READ_LINK_NOT_FOUND, e);
          case EINVAL:
            return fail(R::PATH_READ_LINK_NOT_A_LINK, e);
          case EACCES:
          case EPERM:
            return fail(R::PATH_READ_LINK_ACCESS_DENIED, e);
          case ELOOP:
            return fail(
                nofollow(resolve) ? R::PATH_READ_LINK_SYMLINK
                                  : R::PATH_READ_LINK_INVALID_PATH,
                e);
          case ENAMETOOLONG:
            return fail(R::PATH_READ_LINK_PATH_TOO_LONG, e);
          default:
            return fail(R::PATH_READ_LINK_SYSTEM_ERROR, e);
          }
        };
        parent par;
        const int pe = resolve_parent(p, resolve, &par);
        if (pe != 0)
          return error(pe);
        char target[MAX_PATH_BYTES + 2];
        const ssize_t n = ::readlinkat(par.fd, par.last, target, sizeof(target));
        if (n < 0)
          return error(errno);
        info->relative = n == 0 || target[0] != '/';
        if (!copy_out(target, static_cast<size_t>(n), buffer, capacity, needed))
          return fail(R::PATH_READ_LINK_BUFFER_TOO_SMALL);
        return {};
      }

      /*****************************/
      /* LISTING                   */
      /*****************************/

      status<dir_list_result> platform_dir_list_begin(
          native_dir_t d, dir_reader&) noexcept
      {
        if (::lseek(d, 0, SEEK_SET) < 0)
          return fail(dir_list_result::DIR_LIST_SYSTEM_ERROR, errno);
        return {};
      }

      status<dir_list_result> platform_dir_list_next(
          native_dir_t d, dir_reader& r, dir_entry* out) noexcept
      {
        using R    = dir_list_result;
        auto* base = static_cast<unsigned char*>(r.buffer);
        for (;;)
        {
#if OSPRIM_OS_APPLE
          // internal_end: records left in the buffer
          if (r.internal_end == 0)
          {
            struct attrlist al{};
            al.bitmapcount = ATTR_BIT_MAP_COUNT;
            al.commonattr =
                ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_NAME | ATTR_CMN_OBJTYPE;
            int n = -1;
            do
            {
              n = ::getattrlistbulk(d, &al, r.buffer, r.size, 0);
            } while (n < 0 && errno == EINTR);
            if (n < 0)
              return fail(R::DIR_LIST_SYSTEM_ERROR, errno);
            if (n == 0)
              return fail(R::DIR_LIST_END);
            r.internal_end    = static_cast<size_t>(n);
            r.internal_offset = 0;
          }
          unsigned char* rec = base + r.internal_offset;
          uint32_t length    = 0;
          ::memcpy(&length, rec, 4);
          attribute_set_t returned{};
          ::memcpy(&returned, rec + 4, sizeof(returned));
          unsigned char* field = rec + 4 + sizeof(returned);
          const char* name     = nullptr;
          if ((returned.commonattr & ATTR_CMN_NAME) != 0)
          {
            attrreference_t ref{};
            ::memcpy(&ref, field, sizeof(ref));
            name = reinterpret_cast<const char*>(field) + ref.attr_dataoffset;
            field += sizeof(ref);
          }
          path_type type = path_type::PATH_TYPE_UNKNOWN;
          if ((returned.commonattr & ATTR_CMN_OBJTYPE) != 0)
          {
            fsobj_type_t t = 0;
            ::memcpy(&t, field, sizeof(t));
            type = t == APPLE_VREG   ? path_type::PATH_TYPE_FILE
                   : t == APPLE_VDIR ? path_type::PATH_TYPE_DIRECTORY
                   : t == APPLE_VLNK ? path_type::PATH_TYPE_SYMLINK
                                     : path_type::PATH_TYPE_OTHER;
          }
          r.internal_offset += length;
          --r.internal_end;
#else
          if (r.internal_offset >= r.internal_end)
          {
            long n = -1;
            do
            {
              n = ::syscall(__NR_getdents64, d, r.buffer, r.size);
            } while (n < 0 && errno == EINTR);
            if (n < 0)
              return fail(R::DIR_LIST_SYSTEM_ERROR, errno);
            if (n == 0)
              return fail(R::DIR_LIST_END);
            r.internal_offset = 0;
            r.internal_end    = static_cast<size_t>(n);
          }
          // struct linux_dirent64: d_ino (8), d_off (8), d_reclen (2), d_type (1), d_name
          unsigned char* rec = base + r.internal_offset;
          uint16_t reclen    = 0;
          ::memcpy(&reclen, rec + 16, 2);
          const unsigned char dt = rec[18];
          const char* name       = reinterpret_cast<const char*>(rec + 19);
          r.internal_offset += reclen;
          const path_type type = dt == DT_REG       ? path_type::PATH_TYPE_FILE
                                 : dt == DT_DIR     ? path_type::PATH_TYPE_DIRECTORY
                                 : dt == DT_LNK     ? path_type::PATH_TYPE_SYMLINK
                                 : dt == DT_UNKNOWN ? path_type::PATH_TYPE_UNKNOWN
                                                    : path_type::PATH_TYPE_OTHER;
#endif
          if (name == nullptr)
            continue;
          if (name[0] == '.'
              && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
            continue;
          out->name      = name;
          out->name_size = ::strlen(name);
          out->type      = type;
          return {};
        }
      }

      /*****************************/
      /* CANONICAL PATHS           */
      /*****************************/

      status<path_canonical_result> platform_canonical(
          native_file_t native, char* buffer, size_t capacity,
          size_t* needed) noexcept
      {
        using R = path_canonical_result;
        struct stat st{};
        if (::fstat(native, &st) != 0)
          return fail(R::PATH_CANONICAL_SYSTEM_ERROR, errno);
        if (st.st_nlink == 0)
          return fail(R::PATH_CANONICAL_DELETED);
#if OSPRIM_OS_APPLE
        char path[MAXPATHLEN];
        if (::fcntl(native, F_GETPATH, path) != 0)
          return fail(R::PATH_CANONICAL_SYSTEM_ERROR, errno);
        const size_t n = ::strlen(path);
#else
        char link[64];
        ::snprintf(link, sizeof(link), "/proc/self/fd/%d", native);
        char path[MAX_PATH_BYTES + 2];
        const ssize_t got = ::readlink(link, path, sizeof(path));
        if (got < 0)
          return fail(R::PATH_CANONICAL_SYSTEM_ERROR, errno);
        if (static_cast<size_t>(got) >= sizeof(path) || got == 0 || path[0] != '/')
          return fail(R::PATH_CANONICAL_SYSTEM_ERROR); // no /proc, or not a path
        const size_t n = static_cast<size_t>(got);
#endif
        if (!copy_out(path, n, buffer, capacity, needed))
          return fail(R::PATH_CANONICAL_BUFFER_TOO_SMALL);
        return {};
      }

      /*****************************/
      /* CURRENT DIRECTORY         */
      /*****************************/

      status<current_directory_get_result> platform_current_directory_get(
          char* buffer, size_t capacity, size_t* needed) noexcept
      {
        using R = current_directory_get_result;
        char path[MAX_PATH_BYTES + 1];
        if (::getcwd(path, sizeof(path)) == nullptr)
        {
          const int e = errno;
          if (e == ERANGE || e == ENAMETOOLONG)
            return fail(R::CURRENT_DIRECTORY_GET_PATH_TOO_LONG, e);
          if (e == ENOENT)
            return fail(R::CURRENT_DIRECTORY_GET_DELETED, e);
          return fail(R::CURRENT_DIRECTORY_GET_SYSTEM_ERROR, e);
        }
        if (!copy_out(path, ::strlen(path), buffer, capacity, needed))
          return fail(R::CURRENT_DIRECTORY_GET_BUFFER_TOO_SMALL);
        return {};
      }

      status<current_directory_set_result> platform_current_directory_set(
          const char* path, size_t) noexcept
      {
        using R = current_directory_set_result;
        if (::chdir(path) == 0)
          return {};
        const int e = errno;
        switch (e)
        {
        case ENOENT:
          return fail(R::CURRENT_DIRECTORY_SET_NOT_FOUND, e);
        case ENOTDIR:
          return fail(R::CURRENT_DIRECTORY_SET_NOT_A_DIRECTORY, e);
        case EACCES:
        case EPERM:
          return fail(R::CURRENT_DIRECTORY_SET_ACCESS_DENIED, e);
        case ENAMETOOLONG:
          return fail(R::CURRENT_DIRECTORY_SET_PATH_TOO_LONG, e);
        case ELOOP:
        case EILSEQ:
          return fail(R::CURRENT_DIRECTORY_SET_INVALID_PATH, e);
        default:
          return fail(R::CURRENT_DIRECTORY_SET_SYSTEM_ERROR, e);
        }
      }
    } // namespace detail
  } // namespace v0_1
} // namespace osprim
