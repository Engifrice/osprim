/*****************************************************************/ /**
 * @file   file_posix.cpp
 * @brief  Files and regions on Linux, Android and Apple.
 *
 * - open: O_CLOEXEC | O_NOCTTY | O_NONBLOCK (a FIFO would otherwise block
 *   the open before its type can be checked; the flag has no effect on
 *   regular files), then fstat refuses anything but a regular file.
 * - unbuffered: O_DIRECT (Linux, Android), F_NOCACHE right after open
 *   (Apple). Hints: posix_fadvise (Linux, Android), F_RDAHEAD (Apple).
 * - flush: fdatasync (Linux, Android), F_FULLFSYNC falling back to fsync
 *   where the filesystem refuses it (Apple).
 * - RESERVE: fallocate (Linux, Android), F_PREALLOCATE then ftruncate (Apple).
 * - alignment: statx STATX_DIOALIGN (Linux 6.1+; Android 11+ only, older
 *   seccomp policies kill the process on unknown system calls), else the
 *   page size; Apple: the filesystem block size.
 * - holes (Debug check): the allocated block count first, then FIEMAP
 *   (Linux, Android: reserved but unwritten extents count as reserved),
 *   else SEEK_HOLE.
 *********************************************************************/
#if !defined(_GNU_SOURCE) && !defined(__APPLE__)
  #define _GNU_SOURCE 1 // O_DIRECT, fallocate
#endif

#include "internal.hpp"

#if OSPRIM_OS_WINDOWS
  #error "file_posix.cpp is for POSIX platforms only"
#endif

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if OSPRIM_OS_APPLE
  #include <sys/mount.h>
#else
  #include <linux/fiemap.h>
  #include <linux/fs.h>
  #include <sys/ioctl.h>
  #include <sys/syscall.h>
#endif
#if OSPRIM_OS_ANDROID
  #include <stdlib.h>
  #include <sys/system_properties.h>
#endif

namespace osprim
{
  inline namespace v0_1
  {
    namespace
    {
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

      bool is_io_error(int e) noexcept
      {
        return e == EIO;
      }
      bool is_no_space(int e) noexcept
      {
        return e == ENOSPC || e == EDQUOT;
      }

      status<file_open_result> open_error(int e, bool unbuffered) noexcept
      {
        using R = file_open_result;
        switch (e)
        {
        case ENOENT:
        case ENOTDIR:
          return fail(R::FILE_OPEN_NOT_FOUND, e);
        case EEXIST:
          return fail(R::FILE_OPEN_ALREADY_EXISTS, e);
        case EACCES:
        case EPERM:
        case EROFS:
          return fail(R::FILE_OPEN_ACCESS_DENIED, e);
        case EISDIR:
        case ENXIO: // FIFO opened for writing without a reader
        case ENODEV:
          return fail(R::FILE_OPEN_NOT_A_FILE, e);
        case ENAMETOOLONG:
          return fail(R::FILE_OPEN_PATH_TOO_LONG, e);
        case EILSEQ: // APFS: name not valid UTF-8
          return fail(R::FILE_OPEN_INVALID_PATH, e);
        case ETXTBSY:
          return fail(R::FILE_OPEN_BUSY, e);
        case EMFILE:
        case ENFILE:
          return fail(R::FILE_OPEN_TOO_MANY_OPEN, e);
        case ENOSPC:
        case EDQUOT:
          return fail(R::FILE_OPEN_NO_SPACE, e);
        case EINVAL:
          if (unbuffered) // O_DIRECT refused by the filesystem
            return fail(R::FILE_OPEN_UNSUPPORTED, e);
          return fail(R::FILE_OPEN_SYSTEM_ERROR, e);
        default:
          return fail(R::FILE_OPEN_SYSTEM_ERROR, e);
        }
      }

#if OSPRIM_OS_LINUX || OSPRIM_OS_ANDROID
      /// @brief statx() may be called (Android: seccomp allows it from 11).
      bool statx_allowed() noexcept
      {
  #if OSPRIM_OS_ANDROID
        static const bool allowed = []
        {
          char sdk[PROP_VALUE_MAX] = {};
          return ::__system_property_get("ro.build.version.sdk", sdk) > 0
                 && ::atoi(sdk) >= 30;
        }();
        return allowed;
  #else
        return true;
  #endif
      }

      constexpr unsigned STATX_DIOALIGN_MASK = 0x00002000U;

      /// @brief STATX_DIOALIGN through the raw system call (the libc
      /// headers may predate it). Layout of struct statx: stx_mask at 0,
      /// stx_dio_mem_align at 152, stx_dio_offset_align at 156.
      /// @return false if not available
      bool statx_dio(int fd, uint32_t* mem, uint32_t* offset) noexcept
      {
  #if defined(__NR_statx)
        if (!statx_allowed())
          return false;
        alignas(8) unsigned char buf[256] = {};
        if (::syscall(__NR_statx, fd, "", AT_EMPTY_PATH, STATX_DIOALIGN_MASK, buf)
            != 0)
          return false;
        uint32_t mask = 0;
        ::memcpy(&mask, buf, 4);
        if ((mask & STATX_DIOALIGN_MASK) == 0)
          return false;
        ::memcpy(mem, buf + 152, 4);
        ::memcpy(offset, buf + 156, 4);
        return true;
  #else
        (void)fd;
        (void)mem;
        (void)offset;
        return false;
  #endif
      }
#endif

      int64_t timespec_ns(const struct timespec& t) noexcept
      {
        return static_cast<int64_t>(t.tv_sec) * 1'000'000'000
               + static_cast<int64_t>(t.tv_nsec);
      }
    } // namespace

    namespace detail
    {
      void platform_file_initialize(size_t* page, size_t* granularity) noexcept
      {
        const long p = ::sysconf(_SC_PAGESIZE);
        *page        = p > 0 ? static_cast<size_t>(p) : 4096;
        *granularity = *page;
      }

      void platform_file_deinitialize() noexcept
      {
      }

      int posix_file_flags(const file_open_desc& desc) noexcept
      {
        int flags = O_CLOEXEC | O_NOCTTY | O_NONBLOCK;
        switch (desc.access)
        {
        case file_access::FILE_ACCESS_READ:
          flags |= O_RDONLY;
          break;
        case file_access::FILE_ACCESS_WRITE:
          flags |= O_WRONLY;
          break;
        case file_access::FILE_ACCESS_READ_WRITE:
          flags |= O_RDWR;
          break;
        }
        switch (desc.disposition)
        {
        case file_disposition::FILE_DISPOSITION_OPEN_EXISTING:
          break;
        case file_disposition::FILE_DISPOSITION_CREATE_NEW:
          flags |= O_CREAT | O_EXCL;
          break;
        case file_disposition::FILE_DISPOSITION_OPEN_OR_CREATE:
          flags |= O_CREAT;
          break;
        case file_disposition::FILE_DISPOSITION_CREATE_OR_TRUNCATE:
          flags |= O_CREAT | O_TRUNC;
          break;
        case file_disposition::FILE_DISPOSITION_TRUNCATE_EXISTING:
          flags |= O_TRUNC;
          break;
        }
#if !OSPRIM_OS_APPLE
        if (desc.unbuffered)
          flags |= O_DIRECT;
#endif
        return flags;
      }

      status<file_open_result> posix_open_error(int e, bool unbuffered) noexcept
      {
        return open_error(e, unbuffered);
      }

      status<file_open_result> posix_file_finish(
          int fd, const file_open_desc& desc, native_file_t* out) noexcept
      {
        struct stat st{};
        if (::fstat(fd, &st) != 0)
        {
          const int e = errno;
          ::close(fd);
          return fail(file_open_result::FILE_OPEN_SYSTEM_ERROR, e);
        }
        if (!S_ISREG(st.st_mode))
        {
          ::close(fd);
          return fail(file_open_result::FILE_OPEN_NOT_A_FILE);
        }

#if OSPRIM_OS_APPLE
        if (desc.unbuffered && ::fcntl(fd, F_NOCACHE, 1) != 0)
        {
          const int e = errno;
          ::close(fd);
          return fail(file_open_result::FILE_OPEN_UNSUPPORTED, e);
        }
        if (desc.pattern == file_pattern::FILE_PATTERN_RANDOM)
          ::fcntl(fd, F_RDAHEAD, 0); // a hint: failure is harmless
#else
        if (desc.pattern == file_pattern::FILE_PATTERN_SEQUENTIAL)
          ::posix_fadvise(
              fd, 0, 0, POSIX_FADV_SEQUENTIAL); // a hint: failure is harmless
        else if (desc.pattern == file_pattern::FILE_PATTERN_RANDOM)
          ::posix_fadvise(fd, 0, 0, POSIX_FADV_RANDOM);
#endif
        *out = fd;
        return {};
      }

      status<file_open_result> platform_file_open(
          const char* path, size_t size, const file_open_desc& desc,
          native_file_t* out) noexcept
      {
        if (path[0] != '/')
          return fail(file_open_result::FILE_OPEN_PATH_NOT_ABSOLUTE);
        char buffer[MAX_PATH_BYTES + 1];
        ::memcpy(buffer, path, size);
        buffer[size] = '\0';

        const int flags = posix_file_flags(desc);
        int fd          = -1;
        do
        {
          fd = ::open(buffer, flags, 0666);
        } while (fd < 0 && errno == EINTR);
        if (fd < 0)
          return open_error(errno, desc.unbuffered);
        return posix_file_finish(fd, desc, out);
      }

      void platform_file_close(native_file_t native) noexcept
      {
        // Never retried: the descriptor is released even when close fails
        ::close(native);
      }

      status<file_adopt_result> platform_file_adopt(
          native_file_t native, bool* unbuffered) noexcept
      {
        struct stat st{};
        if (::fstat(native, &st) != 0)
          return fail(file_adopt_result::FILE_ADOPT_SYSTEM_ERROR, errno);
        if (!S_ISREG(st.st_mode))
          return fail(file_adopt_result::FILE_ADOPT_NOT_A_FILE);
        const int fl = ::fcntl(native, F_GETFL);
        if (fl < 0)
          return fail(file_adopt_result::FILE_ADOPT_SYSTEM_ERROR, errno);
        if ((fl & O_APPEND) != 0)
          return fail(file_adopt_result::FILE_ADOPT_APPEND_MODE);
        if (unbuffered != nullptr)
        {
#if OSPRIM_OS_APPLE
          *unbuffered = false; // F_NOCACHE cannot be read back
#else
          *unbuffered = (fl & O_DIRECT) != 0;
#endif
        }
        return {};
      }

      status<file_read_result> platform_file_read(
          native_file_t native, uint64_t offset, void* buffer, size_t size,
          size_t* transferred) noexcept
      {
        ssize_t n = 0;
        do
        {
          n = ::pread(native, buffer, size, static_cast<off_t>(offset));
        } while (n < 0 && errno == EINTR);
        if (n < 0)
        {
          const int e = errno;
          if (is_io_error(e))
            return fail(file_read_result::FILE_READ_IO_ERROR, e);
          if (e == EINVAL)
            return fail(file_read_result::FILE_READ_MISALIGNED, e);
          return fail(file_read_result::FILE_READ_SYSTEM_ERROR, e);
        }
        *transferred = static_cast<size_t>(n);
        return {};
      }

      status<file_write_result> platform_file_write(
          native_file_t native, uint64_t offset, const void* buffer, size_t size,
          size_t* transferred) noexcept
      {
        ssize_t n = 0;
        do
        {
          n = ::pwrite(native, buffer, size, static_cast<off_t>(offset));
        } while (n < 0 && errno == EINTR);
        if (n < 0)
        {
          const int e = errno;
          if (is_no_space(e))
            return fail(file_write_result::FILE_WRITE_NO_SPACE, e);
          if (e == EFBIG)
            return fail(file_write_result::FILE_WRITE_FILE_TOO_LARGE, e);
          if (is_io_error(e))
            return fail(file_write_result::FILE_WRITE_IO_ERROR, e);
          if (e == EINVAL)
            return fail(file_write_result::FILE_WRITE_MISALIGNED, e);
          return fail(file_write_result::FILE_WRITE_SYSTEM_ERROR, e);
        }
        *transferred = static_cast<size_t>(n);
        return {};
      }

      status<file_flush_result> platform_file_flush(native_file_t native) noexcept
      {
        int r = 0;
#if OSPRIM_OS_APPLE
        do
        {
          r = ::fcntl(native, F_FULLFSYNC);
        } while (r != 0 && errno == EINTR);
        if (r != 0 && (errno == ENOTSUP || errno == ENOTTY || errno == EINVAL))
        {
          do
          {
            r = ::fsync(native);
          } while (r != 0 && errno == EINTR);
        }
#else
        do
        {
          r = ::fdatasync(native);
        } while (r != 0 && errno == EINTR);
#endif
        if (r != 0)
        {
          const int e = errno;
          if (is_no_space(e))
            return fail(file_flush_result::FILE_FLUSH_NO_SPACE, e);
          if (is_io_error(e))
            return fail(file_flush_result::FILE_FLUSH_IO_ERROR, e);
          return fail(file_flush_result::FILE_FLUSH_SYSTEM_ERROR, e);
        }
        return {};
      }

      status<file_info_result> platform_file_info(
          native_file_t native, file_info* out) noexcept
      {
        struct stat st{};
        if (::fstat(native, &st) != 0)
          return fail(file_info_result::FILE_INFO_SYSTEM_ERROR, errno);
        out->size = static_cast<uint64_t>(st.st_size);
#if OSPRIM_OS_APPLE
        out->last_write = wall_instant{timespec_ns(st.st_mtimespec)};
#else
        out->last_write = wall_instant{timespec_ns(st.st_mtim)};
#endif
        out->identity = file_identity{
            static_cast<uint64_t>(st.st_dev), static_cast<uint64_t>(st.st_ino), 0};
        return {};
      }

      status<file_set_size_result> platform_file_set_size(
          native_file_t native, uint64_t size, file_size_mode mode) noexcept
      {
        using R    = file_set_size_result;
        auto error = [](int e) noexcept -> status<R>
        {
          if (is_no_space(e))
            return fail(R::FILE_SET_SIZE_NO_SPACE, e);
          if (e == EFBIG)
            return fail(R::FILE_SET_SIZE_FILE_TOO_LARGE, e);
          if (e == EOPNOTSUPP || e == ENOTSUP)
            return fail(R::FILE_SET_SIZE_UNSUPPORTED, e);
          if (is_io_error(e))
            return fail(R::FILE_SET_SIZE_IO_ERROR, e);
          return fail(R::FILE_SET_SIZE_SYSTEM_ERROR, e);
        };

        if (mode == file_size_mode::FILE_SIZE_MODE_RESERVE)
        {
          struct stat st{};
          if (::fstat(native, &st) != 0)
            return error(errno);
          const uint64_t current = static_cast<uint64_t>(st.st_size);
          if (size > current)
          {
#if OSPRIM_OS_APPLE
            fstore_t store{};
            store.fst_flags   = F_ALLOCATECONTIG | F_ALLOCATEALL;
            store.fst_posmode = F_PEOFPOSMODE;
            store.fst_offset  = 0;
            store.fst_length  = static_cast<off_t>(size - current);
            if (::fcntl(native, F_PREALLOCATE, &store) != 0)
            {
              store.fst_flags = F_ALLOCATEALL; // contiguous space not available
              if (::fcntl(native, F_PREALLOCATE, &store) != 0)
                return error(errno);
            }
            // fall through to ftruncate, which takes the reserved space
#else
            int r = 0;
            do
            {
              r = ::fallocate(
                  native, 0, static_cast<off_t>(current),
                  static_cast<off_t>(size - current));
            } while (r != 0 && errno == EINTR);
            if (r != 0)
              return error(errno);
            return {};
#endif
          }
        }
        int r = 0;
        do
        {
          r = ::ftruncate(native, static_cast<off_t>(size));
        } while (r != 0 && errno == EINTR);
        if (r != 0)
          return error(errno);
        return {};
      }

      status<file_alignment_result> platform_file_alignment(
          native_file_t native, file_alignment* out) noexcept
      {
#if OSPRIM_OS_APPLE
        struct statfs fs{};
        if (::fstatfs(native, &fs) != 0)
          return fail(file_alignment_result::FILE_ALIGNMENT_SYSTEM_ERROR, errno);
        const uint32_t block =
            fs.f_bsize > 0 ? static_cast<uint32_t>(fs.f_bsize) : 4096;
        *out = file_alignment{block, block};
        return {};
#else
        uint32_t mem = 0;
        uint32_t off = 0;
        if (statx_dio(native, &mem, &off))
        {
          if (off == 0 || mem == 0)
            return fail(file_alignment_result::FILE_ALIGNMENT_UNSUPPORTED);
          *out = file_alignment{mem, off};
          return {};
        }
        // Older kernels: the page size is a multiple of every block size
        const uint32_t page = static_cast<uint32_t>(page_size());
        *out                = file_alignment{page, page};
        return {};
#endif
      }

      bool platform_file_has_hole(
          native_file_t native, uint64_t offset, size_t size) noexcept
      {
        struct stat st{};
        if (::fstat(native, &st) != 0)
          return false;
        // Every byte has a block (reserved or written): no hole anywhere
        if (static_cast<uint64_t>(st.st_blocks) * 512
            >= static_cast<uint64_t>(st.st_size))
          return false;
        const uint64_t end = offset + size;

#if !OSPRIM_OS_APPLE
        // FIEMAP: extents reserved by fallocate are unwritten, not holes
        constexpr unsigned EXTENTS = 32;
        alignas(8) unsigned char
            req[sizeof(struct fiemap) + EXTENTS * sizeof(struct fiemap_extent)];
        auto* map = reinterpret_cast<struct fiemap*>(req);
        auto* extents =
            reinterpret_cast<struct fiemap_extent*>(req + sizeof(struct fiemap));
        uint64_t pos = offset;
        while (pos < end)
        {
          ::memset(req, 0, sizeof(req));
          map->fm_start        = pos;
          map->fm_length       = end - pos;
          map->fm_extent_count = EXTENTS;
          if (::ioctl(native, FS_IOC_FIEMAP, map) != 0)
            break; // not supported: SEEK_HOLE below
          if (map->fm_mapped_extents == 0)
            return true; // nothing mapped from pos to end
          for (unsigned i = 0; i < map->fm_mapped_extents; ++i)
          {
            const struct fiemap_extent& e = extents[i];
            if (e.fe_logical > pos)
              return true; // gap before this extent
            pos =
                e.fe_logical + e.fe_length > pos ? e.fe_logical + e.fe_length : pos;
            if (pos >= end)
              return false;
            if ((e.fe_flags & FIEMAP_EXTENT_LAST) != 0)
              return true; // no extent after the last one
          }
        }
        if (pos >= end)
          return false;
#endif
        // SEEK_HOLE moves the file position: restore it (adopted handles may use it)
        const off_t saved = ::lseek(native, 0, SEEK_CUR);
        const off_t hole  = ::lseek(native, static_cast<off_t>(offset), SEEK_HOLE);
        if (saved >= 0)
          ::lseek(native, saved, SEEK_SET);
        return hole >= 0 && static_cast<uint64_t>(hole) < end;
      }

      status<region_map_result> platform_region_map(
          native_file_t native, uint64_t aligned_offset, size_t length, uint64_t end,
          region_access access, void** base) noexcept
      {
        using R = region_map_result;
        struct stat st{};
        if (::fstat(native, &st) != 0)
          return fail(R::REGION_MAP_SYSTEM_ERROR, errno);
        if (end > static_cast<uint64_t>(st.st_size))
          return fail(R::REGION_MAP_OUT_OF_RANGE);

        int prot  = PROT_READ;
        int flags = MAP_SHARED;
        if (access == region_access::REGION_ACCESS_READ_WRITE)
          prot |= PROT_WRITE;
        else if (access == region_access::REGION_ACCESS_COPY_ON_WRITE)
        {
          prot |= PROT_WRITE;
          flags = MAP_PRIVATE;
        }
        void* p = ::mmap(
            nullptr, length, prot, flags, native,
            static_cast<off_t>(aligned_offset));
        if (p == MAP_FAILED)
        {
          const int e = errno;
          if (e == EACCES)
            return fail(R::REGION_MAP_ACCESS_DENIED, e);
          if (e == ENOMEM)
            return fail(R::REGION_MAP_OUT_OF_MEMORY, e);
          if (e == ENODEV)
            return fail(R::REGION_MAP_UNSUPPORTED, e);
          return fail(R::REGION_MAP_SYSTEM_ERROR, e);
        }
        *base = p;
        return {};
      }

      void platform_region_unmap(void* base, size_t length) noexcept
      {
        ::munmap(base, length);
      }

      status<region_flush_result> platform_region_flush(
          void* address, size_t length) noexcept
      {
        if (::msync(address, length, MS_SYNC) != 0)
        {
          const int e = errno;
          if (is_io_error(e))
            return fail(region_flush_result::REGION_FLUSH_IO_ERROR, e);
          return fail(region_flush_result::REGION_FLUSH_SYSTEM_ERROR, e);
        }
        return {};
      }
    } // namespace detail
  } // namespace v0_1
} // namespace osprim
