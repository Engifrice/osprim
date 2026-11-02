/*****************************************************************/ /**
 * @file   file.hpp
 * @brief  Files: open, positioned read and write, flush, size, identity,
 * adoption of native handles, unbuffered I/O.
 *
 * Which function do I need?
 *
 * | Goal                                         | Use                                     |
 * |----------------------------------------------|-----------------------------------------|
 * | Open or create a file by path                | file_open()                             |
 * | Read or write at an offset                   | file_read_at(), file_write_at()         |
 * | Make written data survive a power loss       | file_flush()                            |
 * | Size, last write time, "same file?"          | file_info_of()                          |
 * | Grow or shrink a file                        | file_set_size()                         |
 * | Use a descriptor from another API            | file_adopt()                            |
 * | Give a handle to an API that will close it   | file_release()                          |
 * | Call a platform API on the handle            | file_native()                           |
 * | Stream large data without the OS cache       | file_open_desc::unbuffered,             |
 * |                                              | file_unbuffered_alignment()             |
 * | Random access into large read-only data      | region_map() (region.hpp)               |
 *
 * Rules:
 * - A file_handle is the native handle and nothing more (an fd, a HANDLE).
 *   It is trivially copyable; copies name the same open file. The library
 *   keeps no state for it outside handle tracking.
 * - Every call is one OS call (plus EINTR retries), never a loop: a read is
 *   short only at the end of the file, a write only when space runs out.
 *   Loops ("read exactly", "write everything") belong to the caller.
 * - There is no file position: every read and write names its offset, and
 *   any number of threads may use the same handle at once. Closing a
 *   handle while another thread uses it is undefined.
 * - Regular files only: directories, devices, pipes and sockets are refused.
 * - Paths are UTF-8 (validated on every platform), absolute, given as
 *   pointer and size (no terminator needed), at most MAX_PATH_BYTES.
 *   Names reserved by Windows (NUL, CON, COM1, ...), the characters
 *   < > : " | ? * and case differences are documented, not checked: names
 *   that work on one platform can fail or collide on another.
 * - Handles are never inherited by child processes.
 * - Windows: files are opened with full sharing (read, write, delete), so
 *   they can be renamed or deleted while open, as on POSIX. Handles are
 *   overlapped; the library waits internally, so calls stay blocking.
 *   Paths are normalized (GetFullPathNameW: / becomes \, . and .. are
 *   resolved, trailing dots and spaces are stripped) and given the \\?\
 *   prefix, so the 260-character limit never applies. file_open() uses
 *   about 16 KiB of stack there (two UTF-16 path buffers).
 *
 * Durability and write failures (Linux drops the data of failed
 * background writes and marks it clean; other systems are no better):
 * 1. Keep the same handle open from the first write to file_flush(): a
 *    handle opened after a write failure may never see it.
 * 2. A failed flush poisons the whole file: do not retry the flush (it can
 *    succeed without writing anything) and do not read the data back to
 *    check it (the OS cache may hold data the disk does not). Recover by
 *    writing a complete new copy.
 * 3. Errors can surface at the write, at the flush, or both (delayed
 *    allocation reports a full disk at flush time).
 * 4. Unbuffered writes report device errors in the write itself, but the
 *    size and other metadata still need file_flush().
 * Saving safely: write a temporary file in the same directory, flush it,
 * rename it over the old file, make the rename durable, and checksum the
 * content (see the README).
 *********************************************************************/
#ifndef OSPRIM_FILE_HPP
#define OSPRIM_FILE_HPP

#include <osprim/base.hpp>
#include <osprim/clock.hpp>
#include <osprim/platform.hpp>

#include <stddef.h>
#include <stdint.h>

#include <source_location>

namespace osprim
{
  inline namespace v0_1
  {
    /*****************************/
    /* LIMITS                    */
    /*****************************/

#if OSPRIM_OS_APPLE
    /// @brief Longest path accepted by file_open() (bytes, without
    /// terminator): MAXPATHLEN - 1 on Apple.
    inline constexpr size_t MAX_PATH_BYTES = 1023;
#else
    /// @brief Longest path accepted by file_open() (bytes, without
    /// terminator): PATH_MAX - 1 on Linux and Android; on Windows, the size
    /// of the library's stack buffer (the OS allows about 32,760 UTF-16
    /// units).
    inline constexpr size_t MAX_PATH_BYTES = 4095;
#endif

    /// @brief Longest path accepted on every supported platform: what
    /// content meant to ship everywhere should stay under. Not enforced.
    inline constexpr size_t PORTABLE_MAX_PATH_BYTES = 1023;

#if OSPRIM_OS_WINDOWS
    /// @brief Largest size of a single read or write (bytes): a DWORD on
    /// Windows. The OS limit, not a recommendation: stream in chunks of
    /// 64 KiB to a few MiB.
    inline constexpr size_t FILE_IO_MAX = 0xffffffff;
#elif OSPRIM_OS_APPLE
    /// @brief Largest size of a single read or write (bytes): INT_MAX on
    /// Apple. The OS limit, not a recommendation: stream in chunks of
    /// 64 KiB to a few MiB.
    inline constexpr size_t FILE_IO_MAX = 0x7fffffff;
#else
    /// @brief Largest size of a single read or write (bytes): the kernel
    /// cap on Linux and Android (INT_MAX rounded down to a page). The OS
    /// limit, not a recommendation: stream in chunks of 64 KiB to a few MiB.
    inline constexpr size_t FILE_IO_MAX = 0x7ffff000;
#endif

    /// @brief Largest single read or write accepted on every supported
    /// platform. Not enforced.
    inline constexpr size_t PORTABLE_FILE_IO_MAX = 0x7ffff000;

    /*****************************/
    /* HANDLES                   */
    /*****************************/

#if OSPRIM_OS_WINDOWS
    /// @brief Native file handle: a HANDLE.
    using native_file_t = void*;
#else
    /// @brief Native file handle: a file descriptor.
    using native_file_t = int;
#endif

    /// @brief An open regular file: the native handle and nothing more.
    /// Holds no valid value until a successful file_open() or file_adopt().
    struct file_handle
    {
      native_file_t native;
    };

    /// @brief The native handle, for platform calls. Ownership stays with
    /// `h`: do not close it.
    constexpr native_file_t file_native(file_handle h) noexcept
    {
      return h.native;
    }

    /*****************************/
    /* OPENING                   */
    /*****************************/

    /// @brief What the handle may do.
    enum class file_access : uint8_t
    {
      FILE_ACCESS_READ,
      FILE_ACCESS_WRITE,
      FILE_ACCESS_READ_WRITE,
    };

    /// @brief What to do when the file exists or does not.
    ///
    /// | disposition        | file exists      | file missing     |
    /// |--------------------|------------------|------------------|
    /// | OPEN_EXISTING      | open             | NOT_FOUND        |
    /// | CREATE_NEW         | ALREADY_EXISTS   | create           |
    /// | OPEN_OR_CREATE     | open             | create           |
    /// | CREATE_OR_TRUNCATE | truncate to 0    | create           |
    /// | TRUNCATE_EXISTING  | truncate to 0    | NOT_FOUND        |
    ///
    /// Created files get the default permissions (POSIX: 0666 minus the
    /// umask; Windows: the parent directory's inherited ACL).
    enum class file_disposition : uint8_t
    {
      FILE_DISPOSITION_OPEN_EXISTING,
      FILE_DISPOSITION_CREATE_NEW,
      FILE_DISPOSITION_OPEN_OR_CREATE,
      FILE_DISPOSITION_CREATE_OR_TRUNCATE,
      FILE_DISPOSITION_TRUNCATE_EXISTING,
    };

    /// @brief Access pattern hint: tunes read-ahead (Windows: open flags;
    /// Linux and Android: posix_fadvise; Apple: F_RDAHEAD).
    ///
    /// | pattern    | effect                                                    |
    /// |------------|-----------------------------------------------------------|
    /// | NORMAL     | the OS default                                            |
    /// | SEQUENTIAL | larger read-ahead; Windows also drops pages already read  |
    /// | RANDOM     | no read-ahead: scattered reads do not fetch unused data   |
    enum class file_pattern : uint8_t
    {
      FILE_PATTERN_NORMAL,
      FILE_PATTERN_SEQUENTIAL,
      FILE_PATTERN_RANDOM,
    };

    /// @brief Parameters of file_open().
    struct file_open_desc
    {
      file_access access = file_access::FILE_ACCESS_READ;
      file_disposition disposition =
          file_disposition::FILE_DISPOSITION_OPEN_EXISTING;
      file_pattern pattern = file_pattern::FILE_PATTERN_NORMAL;
      /// @brief Bypass the OS cache where the OS can (best effort: Linux
      /// silently falls back to the cache on some filesystems, such as
      /// encrypted ones without inline encryption). Every read and write
      /// must then follow file_unbuffered_alignment(), on every platform.
      bool unbuffered = false;
    };

    /// @brief Result of file_open().
    enum class file_open_result : uint8_t
    {
      FILE_OPEN_OK,
      FILE_OPEN_NOT_FOUND,      ///< File or a parent directory missing
      FILE_OPEN_ALREADY_EXISTS, ///< CREATE_NEW and the file exists
      FILE_OPEN_ACCESS_DENIED, ///< Permissions, read-only filesystem, iOS data protection
      FILE_OPEN_NOT_A_FILE, ///< Directory, device (including Windows NUL), pipe, socket
      FILE_OPEN_INVALID_PATH, ///< Not UTF-8, embedded NUL, character refused by the filesystem
      FILE_OPEN_PATH_NOT_ABSOLUTE, ///< Relative, drive-relative (C:x) or rooted (\x) path
      FILE_OPEN_PATH_TOO_LONG, ///< Above MAX_PATH_BYTES, or a name too long for the filesystem
      FILE_OPEN_BUSY,          ///< Windows: another program holds it without sharing
      FILE_OPEN_TOO_MANY_OPEN, ///< Process or system limit on open files
      FILE_OPEN_NO_SPACE,      ///< Creating: no space or quota left
      FILE_OPEN_UNSUPPORTED,   ///< The filesystem refuses unbuffered I/O
      FILE_OPEN_SYSTEM_ERROR,  ///< Any other refusal (see native_code)
    };

    /// @brief Opens or creates the regular file at `path`.
    /// @param path UTF-8, absolute, `path_size` bytes (no terminator needed)
    /// @pre `path_size` > 0; the truncating dispositions need write access
    status<file_open_result> file_open(
        const char* path, size_t path_size, const file_open_desc& desc,
        file_handle& out,
        std::source_location loc = std::source_location::current()) noexcept;

    /// @brief Closes the handle. Close errors are not reported: they say
    /// nothing reliable (the handle is released anyway); call file_flush()
    /// first when the data matters.
    /// @pre Open, and no other thread using it
    void file_close(
        file_handle h,
        std::source_location loc = std::source_location::current()) noexcept;

    /*****************************/
    /* ADOPTION                  */
    /*****************************/

    /// @brief Result of file_adopt().
    enum class file_adopt_result : uint8_t
    {
      FILE_ADOPT_OK,
      FILE_ADOPT_NOT_A_FILE, ///< Directory, device, pipe or socket
      FILE_ADOPT_APPEND_MODE, ///< POSIX O_APPEND: positioned writes would land at the end
      FILE_ADOPT_SYSTEM_ERROR, ///< The handle could not be checked (see native_code)
    };

    /// @brief Takes ownership of a native handle opened elsewhere (Android
    /// assets, document pickers, Background Assets, other libraries): the
    /// library closes it in file_close(). On failure ownership stays with
    /// the caller. Windows: synchronous handles work, but their reads and
    /// writes run one at a time. Tracked builds detect unbuffered handles
    /// (O_DIRECT, FILE_FLAG_NO_BUFFERING) for the alignment checks; Apple
    /// cannot report F_NOCACHE, so adopted handles count as buffered there.
    /// @pre `native` is open and not owned by another file_handle
    status<file_adopt_result> file_adopt(
        native_file_t native, file_handle& out,
        std::source_location loc = std::source_location::current()) noexcept;

    /// @brief Gives up ownership without closing: the caller (or the API it
    /// hands the native handle to) closes it.
    /// @pre Open, and no other thread using it
    native_file_t file_release(
        file_handle h,
        std::source_location loc = std::source_location::current()) noexcept;

    /*****************************/
    /* READ AND WRITE            */
    /*****************************/

    /// @brief Result of file_read_at().
    enum class file_read_result : uint8_t
    {
      FILE_READ_OK,         ///< `transferred` < `size` only at the end of the file
      FILE_READ_IO_ERROR,   ///< The device failed
      FILE_READ_MISALIGNED, ///< Unbuffered handle and unaligned offset, size or buffer
      FILE_READ_SYSTEM_ERROR, ///< Any other refusal (see native_code)
    };

    /// @brief Reads up to `size` bytes at `offset`. Reading at or past the
    /// end of the file gives OK with `transferred` 0.
    /// @pre Open with read access; `size` <= FILE_IO_MAX; `offset` + `size`
    /// <= INT64_MAX; unbuffered: aligned (checked in tracked builds)
    status<file_read_result> file_read_at(
        file_handle h, uint64_t offset, void* buffer, size_t size,
        size_t& transferred) noexcept;

    /// @brief Result of file_write_at().
    enum class file_write_result : uint8_t
    {
      FILE_WRITE_OK,       ///< `transferred` < `size` only when space ran out midway
      FILE_WRITE_NO_SPACE, ///< Disk full or quota reached
      FILE_WRITE_FILE_TOO_LARGE, ///< Above the filesystem's file size limit (FAT32: 4 GiB - 1)
      ///< or RLIMIT_FSIZE (POSIX also sends SIGXFSZ, fatal by default)
      FILE_WRITE_IO_ERROR, ///< The device failed
      FILE_WRITE_MISALIGNED, ///< Unbuffered handle and unaligned offset, size or buffer
      FILE_WRITE_SYSTEM_ERROR, ///< Any other refusal (see native_code)
    };

    /// @brief Writes `size` bytes at `offset`. Writing past the end grows
    /// the file; the gap reads as zero. Windows: the gap is zero-filled
    /// first, synchronously, at a cost proportional to its size.
    /// @pre Open with write access; `size` <= FILE_IO_MAX; `offset` + `size`
    /// <= INT64_MAX; unbuffered: aligned (checked in tracked builds)
    status<file_write_result> file_write_at(
        file_handle h, uint64_t offset, const void* buffer, size_t size,
        size_t& transferred) noexcept;

    /*****************************/
    /* DURABILITY                */
    /*****************************/

    /// @brief Result of file_flush().
    enum class file_flush_result : uint8_t
    {
      FILE_FLUSH_OK,
      FILE_FLUSH_NO_SPACE,     ///< Space for delayed writes ran out
      FILE_FLUSH_IO_ERROR,     ///< The device failed
      FILE_FLUSH_SYSTEM_ERROR, ///< Any other refusal (see native_code)
    };

    /// @brief Returns once everything written through this file (any
    /// handle, and regions after region_flush()) is on stable storage
    /// (see "Durability and write failures" above):
    /// fdatasync (Linux, Android), F_FULLFSYNC (Apple), FlushFileBuffers
    /// (Windows). Slow: milliseconds, and much more on Apple. Filesystems
    /// that refuse F_FULLFSYNC (network shares, some external drives) get
    /// fsync, the strongest flush they offer.
    /// On failure, treat everything written since the last successful flush
    /// as lost: the OS may have dropped it, and flushing again can succeed
    /// without bringing it back.
    /// @pre Open with write access
    status<file_flush_result> file_flush(file_handle h) noexcept;

    /*****************************/
    /* SIZE AND IDENTITY         */
    /*****************************/

    /// @brief Identifies a file regardless of its path (POSIX: device and
    /// inode; Windows: volume serial and 128-bit file id). Equal identities
    /// mean the same file. Not stable across reboots on every filesystem.
    struct file_identity
    {
      uint64_t volume;
      uint64_t id_low;
      uint64_t id_high; ///< 0 on POSIX
      friend constexpr bool operator==(
          const file_identity&, const file_identity&) noexcept = default;
    };

    /// @brief What file_info_of() reports.
    struct file_info
    {
      uint64_t size;
      wall_instant last_write;
      file_identity identity;
    };

    /// @brief Result of file_info_of().
    enum class file_info_result : uint8_t
    {
      FILE_INFO_OK,
      FILE_INFO_SYSTEM_ERROR, ///< Any refusal (see native_code)
    };

    /// @brief Size, last write time and identity (fstat; Windows: two
    /// GetFileInformationByHandleEx calls).
    /// @pre Open
    status<file_info_result> file_info_of(file_handle h, file_info& out) noexcept;

    /// @brief How file_set_size() treats storage when growing. Shrinking
    /// ignores it. The new bytes read as zero in both modes.
    ///
    /// | mode    | Linux, Android, APFS | Windows (NTFS)             |
    /// |---------|----------------------|----------------------------|
    /// | FAST    | hole: instant, no    | space allocated, not       |
    /// |         | space reserved       | zeroed: instant            |
    /// | RESERVE | fallocate,           | same as FAST               |
    /// |         | F_PREALLOCATE        |                            |
    ///
    /// Writing through a region into unreserved space can crash the process
    /// when the disk is full (a store has no error return): grow with
    /// RESERVE before writing through a region. Copy-on-write filesystems
    /// (btrfs, APFS with snapshots) can still need space to overwrite, so
    /// data that must survive a full disk is written with file_write_at().
    enum class file_size_mode : uint8_t
    {
      FILE_SIZE_MODE_FAST,    ///< Cheapest: space may or may not be reserved
      FILE_SIZE_MODE_RESERVE, ///< Space for the grown range reserved, or the call fails
    };

    /// @brief Result of file_set_size().
    enum class file_set_size_result : uint8_t
    {
      FILE_SET_SIZE_OK,
      FILE_SET_SIZE_NO_SPACE,       ///< Disk full or quota reached (size unchanged)
      FILE_SET_SIZE_FILE_TOO_LARGE, ///< Above the filesystem's file size limit
      FILE_SET_SIZE_MAPPED,         ///< Windows: shrinking below a mapped range
      FILE_SET_SIZE_UNSUPPORTED, ///< RESERVE not possible on this filesystem (size unchanged)
      FILE_SET_SIZE_IO_ERROR,     ///< The device failed
      FILE_SET_SIZE_SYSTEM_ERROR, ///< Any other refusal (see native_code)
    };

    /// @brief Grows or shrinks the file to `size` bytes. Shrinking below a
    /// live region is reported in tracked builds; on POSIX, touching the
    /// lost pages crashes the process (SIGBUS).
    /// @pre Open with write access; `size` <= INT64_MAX
    status<file_set_size_result> file_set_size(
        file_handle h, uint64_t size, file_size_mode mode,
        std::source_location loc = std::source_location::current()) noexcept;

    /*****************************/
    /* UNBUFFERED I/O            */
    /*****************************/

    /// @brief Alignment unbuffered I/O needs for one file (bytes, powers of
    /// two). Reads and writes: offset and size multiples of `io`, buffer
    /// address a multiple of `memory`. 64 KiB satisfies every platform.
    struct file_alignment
    {
      uint32_t memory;
      uint32_t io;
    };

    /// @brief Result of file_unbuffered_alignment().
    enum class file_alignment_result : uint8_t
    {
      FILE_ALIGNMENT_OK,
      FILE_ALIGNMENT_UNSUPPORTED, ///< The filesystem has no unbuffered I/O for this file (Linux)
      FILE_ALIGNMENT_SYSTEM_ERROR, ///< Any other refusal (see native_code)
    };

    /// @brief The alignment unbuffered I/O on this file needs, on any
    /// handle: statx STATX_DIOALIGN (Linux 6.1+, else the page size),
    /// the larger sector size (Windows), the filesystem block size (Apple,
    /// which needs none but is held to it). At the end of the file, read a
    /// full aligned block (the read is short); write a full block, then
    /// trim with file_set_size().
    /// @pre Open
    status<file_alignment_result> file_unbuffered_alignment(
        file_handle h, file_alignment& out) noexcept;
  } // namespace v0_1
} // namespace osprim

#endif // !OSPRIM_FILE_HPP
