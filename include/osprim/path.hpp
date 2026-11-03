/*****************************************************************/ /**
 * @file   path.hpp
 * @brief  Paths and directories: directory handles, operations relative
 * to them, entry information, creation, removal, renaming, listing, the
 * current directory and canonical paths.
 *
 * Which function do I need?
 *
 * | Goal                                          | Use                                  |
 * |-----------------------------------------------|--------------------------------------|
 * | Work inside a directory without races         | dir_open_at(), then the *_at calls   |
 * | Open a file in a directory                    | file_open_at()                       |
 * | What is this entry (type, size, identity)?    | path_info_at()                       |
 * | Create a directory                            | dir_create_at()                      |
 * | Remove a file, a symlink or an empty directory| path_remove_at()                     |
 * | Replace a file atomically (saves)             | path_rename_at(), REPLACE + durable  |
 * | Rename without overwriting                    | path_rename_at(), NO_REPLACE         |
 * | List a directory                              | dir_list_begin(), dir_list_next()    |
 * | Where a symlink points                        | path_read_link_at()                  |
 * | The real path of an open file or directory    | path_canonical()                     |
 * | The current directory                         | current_directory_get() / _set()     |
 *
 * Avoiding races (TOCTOU): a path is a name resolved again on every call,
 * and what it names can change between two calls (editors, version
 * control, sync clients, users, or an attacker).
 * - Do not check then act: act, and read the result (ALREADY_EXISTS,
 *   NOT_FOUND, ...). Creating, opening and renaming without replacing are
 *   atomic.
 * - Do not resolve a path twice: open the directory once, then name its
 *   entries relative to the handle. The handle stays on that directory
 *   even if it or its parents are renamed.
 * - Walking a tree (removal, copy): open each directory with
 *   PATH_RESOLVE_NO_SYMLINKS and never recurse into a SYMLINK entry.
 * - Untrusted names (mods, archives, user content): open the root
 *   directory normally, then use PATH_RESOLVE_NO_SYMLINKS for every name
 *   inside it. A name relative to a handle cannot climb above it (".." is
 *   refused), so symlinks are the only way out; mount points are crossed
 *   in every mode.
 *
 * Names:
 * - With a directory handle, a name is relative: one or more components
 *   separated by '/' (and '\' on Windows), "." components skipped, ".."
 *   refused (INVALID_PATH). On Windows, names Win32 cannot handle are
 *   refused too (reserved device names such as NUL or COM1, a trailing
 *   dot or space, the characters < > : " | ? * and control characters),
 *   because the relative open would otherwise create entries most tools
 *   cannot open or delete.
 * - With NO_DIRECTORY, a name is an absolute path, under the rules of
 *   file_open() (UTF-8, at most MAX_PATH_BYTES, normalized on Windows).
 *   Ordinary absolute paths contain symlinks (/var on Apple, /sdcard on
 *   Android, /lib on many Linux systems): PATH_RESOLVE_NO_SYMLINKS is meant
 *   for names relative to a trusted directory.
 *
 * Symlinks:
 * - SYMLINK entries are symbolic links; on Windows, every name-surrogate
 *   reparse point: symbolic links, WSL symlinks and junctions. Volume
 *   mount points are directories. Other reparse points (OneDrive and
 *   iCloud placeholders, deduplicated and compressed files) are ordinary
 *   files and directories, and the library never triggers a download.
 * - Removing or renaming a symlink acts on the link, never its target.
 * - The library reads symlinks but does not create them.
 *
 * Handles: a dir_handle is the native handle (a descriptor opened with
 * O_DIRECTORY, a Windows directory handle with full sharing), never
 * inherited by child processes, and holds no other state. The position
 * of a listing lives in the handle: one listing at a time per handle.
 *********************************************************************/
#ifndef OSPRIM_PATH_HPP
#define OSPRIM_PATH_HPP

#include <osprim/base.hpp>
#include <osprim/clock.hpp>
#include <osprim/file.hpp>
#include <osprim/platform.hpp>

#include <stddef.h>
#include <stdint.h>

#include <source_location>

namespace osprim
{
  inline namespace v0_1
  {
    /*****************************/
    /* DIRECTORY HANDLES         */
    /*****************************/

#if OSPRIM_OS_WINDOWS
    /// @brief Native directory handle: a HANDLE.
    using native_dir_t = void*;
#else
    /// @brief Native directory handle: a file descriptor.
    using native_dir_t = int;
#endif

    /// @brief An open directory: the native handle and nothing more.
    struct dir_handle
    {
      native_dir_t native;
    };

    /// @brief "No directory": the name is an absolute path.
#if OSPRIM_OS_WINDOWS
    inline constexpr dir_handle NO_DIRECTORY = {nullptr};
#else
    inline constexpr dir_handle NO_DIRECTORY = {-1};
#endif

    /// @brief The native handle, for platform calls. Ownership stays with `d`.
    constexpr native_dir_t dir_native(dir_handle d) noexcept
    {
      return d.native;
    }

    /// @brief How the components of a name are resolved.
    ///
    /// | resolve     | symlinks                       | Linux             | Apple          | Windows            |
    /// |-------------|--------------------------------|-------------------|----------------|--------------------|
    /// | NORMAL      | followed (the OS's rules)      | openat            | openat         | NtCreateFile       |
    /// | NO_SYMLINKS | refused anywhere in the name;  | openat2 (5.6+),   | O_NOFOLLOW_ANY | one component at a |
    /// |             | the last component is not      | else one component|                | time, name         |
    /// |             | followed (SYMLINK entries)     | at a time         |                | surrogates refused |
    enum class path_resolve : uint8_t
    {
      PATH_RESOLVE_NORMAL,
      PATH_RESOLVE_NO_SYMLINKS,
    };

    /// @brief Result of dir_open_at().
    enum class dir_open_result : uint8_t
    {
      DIR_OPEN_OK,
      DIR_OPEN_NOT_FOUND,       ///< The directory or a parent is missing
      DIR_OPEN_NOT_A_DIRECTORY, ///< The name exists but is not a directory
      DIR_OPEN_ACCESS_DENIED,   ///< Permissions, iOS data protection
      DIR_OPEN_SYMLINK,         ///< NO_SYMLINKS and a symlink in the name
      DIR_OPEN_INVALID_PATH, ///< Not UTF-8, "..", absolute name with a directory, refused name
      DIR_OPEN_PATH_NOT_ABSOLUTE, ///< NO_DIRECTORY and a relative path
      DIR_OPEN_PATH_TOO_LONG, ///< Above MAX_PATH_BYTES, or a name too long for the filesystem
      DIR_OPEN_TOO_MANY_OPEN, ///< Process or system limit on open handles
      DIR_OPEN_SYSTEM_ERROR,  ///< Any other refusal (see native_code)
    };

    /// @brief Opens the directory `name` relative to `dir` (or the absolute
    /// path `name` with NO_DIRECTORY). "." opens `dir` again.
    /// @pre `name_size` > 0
    status<dir_open_result> dir_open_at(
        dir_handle dir, const char* name, size_t name_size, path_resolve resolve,
        dir_handle& out,
        std::source_location loc = std::source_location::current()) noexcept;

    /// @brief Closes the handle.
    /// @pre Open, and no other thread using it
    void dir_close(
        dir_handle d,
        std::source_location loc = std::source_location::current()) noexcept;

    /// @brief Result of dir_adopt().
    enum class dir_adopt_result : uint8_t
    {
      DIR_ADOPT_OK,
      DIR_ADOPT_NOT_A_DIRECTORY, ///< The handle is not a directory
      DIR_ADOPT_SYSTEM_ERROR, ///< The handle could not be checked (see native_code)
    };

    /// @brief Takes ownership of a directory handle opened elsewhere. On
    /// failure ownership stays with the caller.
    status<dir_adopt_result> dir_adopt(
        native_dir_t native, dir_handle& out,
        std::source_location loc = std::source_location::current()) noexcept;

    /// @brief Gives up ownership without closing.
    native_dir_t dir_release(
        dir_handle d,
        std::source_location loc = std::source_location::current()) noexcept;

    /*****************************/
    /* FILES                     */
    /*****************************/

    /// @brief Opens or creates the regular file `name` relative to `dir`,
    /// as file_open() does (file_open(path) is file_open_at(NO_DIRECTORY,
    /// path, NORMAL)). With NO_SYMLINKS, a symlink anywhere in the name,
    /// the last component included, gives FILE_OPEN_SYMLINK.
    /// @pre `name_size` > 0; the truncating dispositions need write access
    status<file_open_result> file_open_at(
        dir_handle dir, const char* name, size_t name_size, path_resolve resolve,
        const file_open_desc& desc, file_handle& out,
        std::source_location loc = std::source_location::current()) noexcept;

    /*****************************/
    /* ENTRY INFORMATION         */
    /*****************************/

    /// @brief What an entry is.
    enum class path_type : uint8_t
    {
      PATH_TYPE_FILE, ///< Regular file (including placeholders and compressed files on Windows)
      PATH_TYPE_DIRECTORY, ///< Directory (including volume mount points on Windows)
      PATH_TYPE_SYMLINK, ///< Symbolic link; on Windows any name surrogate (including junctions)
      PATH_TYPE_OTHER, ///< Device, FIFO, socket
      PATH_TYPE_UNKNOWN, ///< dir_list_next() only: the filesystem did not say, or (Windows) a
      ///< junction or volume mount point: query it with path_info_at()
    };

    /// @brief What path_info_at() reports.
    struct path_info
    {
      path_type type;
      uint64_t size; ///< Files: size in bytes; others: 0
      wall_instant last_write;
      file_identity identity;
    };

    /// @brief Result of path_info_at().
    enum class path_info_result : uint8_t
    {
      PATH_INFO_OK,
      PATH_INFO_NOT_FOUND, ///< Missing (or, followed, a dangling symlink)
      PATH_INFO_ACCESS_DENIED,
      PATH_INFO_SYMLINK, ///< NO_SYMLINKS and a symlink before the last component
      PATH_INFO_INVALID_PATH,
      PATH_INFO_PATH_NOT_ABSOLUTE,
      PATH_INFO_PATH_TOO_LONG,
      PATH_INFO_SYSTEM_ERROR,
    };

    /// @brief Type, size, last write time and identity of an entry. NORMAL
    /// follows a final symlink (stat); NO_SYMLINKS reports it as SYMLINK
    /// (lstat). Never opens file contents (no cloud download on Windows).
    status<path_info_result> path_info_at(
        dir_handle dir, const char* name, size_t name_size, path_resolve resolve,
        path_info& out) noexcept;

    /*****************************/
    /* CREATE, REMOVE, RENAME    */
    /*****************************/

    /// @brief Result of dir_create_at().
    enum class dir_create_result : uint8_t
    {
      DIR_CREATE_OK,
      DIR_CREATE_ALREADY_EXISTS, ///< Any entry has this name (a directory or not)
      DIR_CREATE_NOT_FOUND,      ///< A parent is missing
      DIR_CREATE_ACCESS_DENIED,
      DIR_CREATE_SYMLINK,
      DIR_CREATE_INVALID_PATH,
      DIR_CREATE_PATH_NOT_ABSOLUTE,
      DIR_CREATE_PATH_TOO_LONG,
      DIR_CREATE_NO_SPACE,
      DIR_CREATE_SYSTEM_ERROR,
    };

    /// @brief Creates the directory `name` (POSIX: 0777 minus the umask;
    /// Windows: the parent's inherited ACL). With `out` non-null, also
    /// opens it: in one step on Windows; on POSIX right after, without
    /// following a symlink (another directory swapped in between cannot be
    /// detected there).
    status<dir_create_result> dir_create_at(
        dir_handle dir, const char* name, size_t name_size, path_resolve resolve,
        dir_handle* out          = nullptr,
        std::source_location loc = std::source_location::current()) noexcept;

    /// @brief What path_remove_at() removes.
    enum class path_kind : uint8_t
    {
      PATH_KIND_FILE, ///< A file, a symlink (any target) or another non-directory
      PATH_KIND_DIRECTORY, ///< An empty directory
    };

    /// @brief Result of path_remove_at().
    enum class path_remove_result : uint8_t
    {
      PATH_REMOVE_OK,
      PATH_REMOVE_NOT_FOUND,
      PATH_REMOVE_WRONG_KIND, ///< A directory removed as FILE, or the reverse
      PATH_REMOVE_NOT_EMPTY,  ///< DIRECTORY and entries remain
      PATH_REMOVE_ACCESS_DENIED,
      PATH_REMOVE_BUSY, ///< Windows: held open by a program without delete sharing
      PATH_REMOVE_SYMLINK, ///< NO_SYMLINKS and a symlink before the last component
      PATH_REMOVE_INVALID_PATH,
      PATH_REMOVE_PATH_NOT_ABSOLUTE,
      PATH_REMOVE_PATH_TOO_LONG,
      PATH_REMOVE_SYSTEM_ERROR,
    };

    /// @brief Removes the entry `name`; the last component is never
    /// followed (removing a symlink removes the link). Windows: removed by
    /// handle with POSIX semantics (the name disappears at once, even
    /// while the file is open) and regardless of the read-only attribute;
    /// on filesystems without POSIX semantics (FAT, network shares) the
    /// name lingers until the last handle closes.
    status<path_remove_result> path_remove_at(
        dir_handle dir, const char* name, size_t name_size, path_resolve resolve,
        path_kind kind) noexcept;

    /// @brief What path_rename_at() does to an existing target.
    enum class path_rename_mode : uint8_t
    {
      PATH_RENAME_MODE_REPLACE, ///< Atomically replace an existing target
      PATH_RENAME_MODE_NO_REPLACE, ///< Fail with ALREADY_EXISTS (atomic: no check then act)
    };

    /// @brief Parameters of path_rename_at().
    struct path_rename_desc
    {
      path_rename_mode mode = path_rename_mode::PATH_RENAME_MODE_REPLACE;
      /// @brief Applies to both names (the last components are never followed)
      path_resolve resolve = path_resolve::PATH_RESOLVE_NORMAL;
      /// @brief Return once the rename is on stable storage: POSIX: fsync
      /// of both directories (Apple: then F_FULLFSYNC); Windows: flush of
      /// the target directory. Slow, as file_flush().
      bool durable = false;
    };

    /// @brief Result of path_rename_at().
    enum class path_rename_result : uint8_t
    {
      PATH_RENAME_OK,
      PATH_RENAME_NOT_FOUND, ///< The source, or a parent of either name, is missing
      PATH_RENAME_ALREADY_EXISTS, ///< NO_REPLACE and the target exists
      PATH_RENAME_WRONG_KIND,     ///< A file onto a directory, or the reverse
      PATH_RENAME_NOT_EMPTY,      ///< The target is a non-empty directory
      PATH_RENAME_CROSS_DEVICE,   ///< Different filesystems: rename is never a copy
      PATH_RENAME_ACCESS_DENIED,
      PATH_RENAME_BUSY, ///< Windows: held open by a program without delete sharing
      PATH_RENAME_SYMLINK, ///< NO_SYMLINKS and a symlink before a last component
      PATH_RENAME_INVALID_PATH,
      PATH_RENAME_PATH_NOT_ABSOLUTE,
      PATH_RENAME_PATH_TOO_LONG,
      PATH_RENAME_NO_SPACE,
      PATH_RENAME_UNSUPPORTED, ///< NO_REPLACE refused by this filesystem (nothing renamed)
      PATH_RENAME_NOT_DURABLE, ///< Renamed, but making it durable failed: see file_flush()
      PATH_RENAME_SYSTEM_ERROR,
    };

    /// @brief Renames `from` (relative to `from_dir`) to `to` (relative to
    /// `to_dir`), on one filesystem. Renaming a symlink renames the link.
    /// Replacing an existing directory differs: POSIX replaces an empty
    /// one, Windows refuses (ACCESS_DENIED).
    status<path_rename_result> path_rename_at(
        dir_handle from_dir, const char* from, size_t from_size, dir_handle to_dir,
        const char* to, size_t to_size, const path_rename_desc& desc) noexcept;

    /*****************************/
    /* SYMLINKS                  */
    /*****************************/

    /// @brief What path_read_link_at() reports besides the target.
    struct link_info
    {
      bool relative; ///< The target is relative to the link's directory
      bool
          directory; ///< Windows: a directory symlink or a junction (false elsewhere)
      bool junction; ///< Windows: a junction (false elsewhere)
    };

    /// @brief Result of path_read_link_at().
    enum class path_read_link_result : uint8_t
    {
      PATH_READ_LINK_OK,
      PATH_READ_LINK_NOT_FOUND,
      PATH_READ_LINK_NOT_A_LINK,       ///< The entry exists but is not a SYMLINK
      PATH_READ_LINK_BUFFER_TOO_SMALL, ///< `needed` holds the size required
      PATH_READ_LINK_ACCESS_DENIED,
      PATH_READ_LINK_SYMLINK, ///< NO_SYMLINKS and a symlink before the last component
      PATH_READ_LINK_INVALID_PATH,
      PATH_READ_LINK_PATH_NOT_ABSOLUTE,
      PATH_READ_LINK_PATH_TOO_LONG,
      PATH_READ_LINK_SYSTEM_ERROR,
    };

    /// @brief The target of the symlink `name`, as stored (relative stays
    /// relative), in UTF-8, without terminator. Windows: \??\C:\x is
    /// reported as C:\x and \??\UNC\s\x as \\s\x.
    status<path_read_link_result> path_read_link_at(
        dir_handle dir, const char* name, size_t name_size, path_resolve resolve,
        char* buffer, size_t capacity, size_t& needed, link_info& info) noexcept;

    /*****************************/
    /* LISTING                   */
    /*****************************/

#if OSPRIM_OS_WINDOWS
    /// @brief Smallest listing buffer: one entry with the longest name
    /// (255 UTF-16 units) in the largest record the library asks for
    /// (FILE_ID_BOTH_DIR_INFO, the fallback on older filesystems).
    inline constexpr size_t DIR_LIST_MIN_BUFFER = 616;
    /// @brief A good listing buffer: on network shares each call is a round
    /// trip, and 64 KiB is the most Windows 8.1 and earlier accept there
    /// (the library never asks Windows for more, whatever the size).
    inline constexpr size_t DIR_LIST_RECOMMENDED_BUFFER = 65536;
#elif OSPRIM_OS_APPLE
    /// @brief Smallest listing buffer: one getattrlistbulk record (36 bytes)
    /// with the longest name (HFS+: 255 UTF-16 units, 765 bytes of UTF-8).
    inline constexpr size_t DIR_LIST_MIN_BUFFER = 808;
    /// @brief A good listing buffer (glibc's default for readdir). Locally,
    /// any size from 4 KiB up lists at the same speed; on network shares
    /// each call can be a round trip.
    inline constexpr size_t DIR_LIST_RECOMMENDED_BUFFER = 32768;
#else
    /// @brief Smallest listing buffer: one dirent64 record (19 bytes) with
    /// the longest name (255 UTF-16 units of NTFS, exFAT or SMB mounts,
    /// 765 bytes of UTF-8).
    inline constexpr size_t DIR_LIST_MIN_BUFFER = 792;
    /// @brief A good listing buffer (glibc's default for readdir). Locally,
    /// any size from 4 KiB up lists at the same speed (the kernel's work
    /// per entry dominates); on network shares and FUSE (Android shared
    /// storage) each call can be a round trip.
    inline constexpr size_t DIR_LIST_RECOMMENDED_BUFFER = 32768;
#endif

    /// @brief Caller-owned listing state (the library does not allocate).
    struct dir_reader
    {
      /// @brief Caller-owned, 8-byte aligned, at least DIR_LIST_MIN_BUFFER
      /// bytes, alive until the listing ends
      void* buffer = nullptr;
      size_t size  = 0;
      /// @brief Library state: do not touch
      size_t internal_offset  = 0;
      size_t internal_end     = 0;
      uint32_t internal_flags = 0;
#if OSPRIM_OS_WINDOWS
      /// @brief Library state: the current name, converted to UTF-8
      char internal_name[768];
#endif
    };

    /// @brief One entry of a listing, valid until the next call on the reader.
    struct dir_entry
    {
      const char* name; ///< Not terminated. Linux: raw bytes, possibly not UTF-8
      size_t name_size;
      path_type type; ///< UNKNOWN when the filesystem does not say
    };

    /// @brief Result of dir_list_begin() and dir_list_next().
    enum class dir_list_result : uint8_t
    {
      DIR_LIST_OK,            ///< `out` holds an entry
      DIR_LIST_END,           ///< No more entries
      DIR_LIST_NAME_TOO_LONG, ///< Windows: a name above 255 UTF-16 units (skipped)
      DIR_LIST_SYSTEM_ERROR,  ///< See native_code
    };

    /// @brief Starts (or restarts) a listing of `d` from the beginning.
    /// @pre `r.buffer` and `r.size` set as documented
    status<dir_list_result> dir_list_begin(dir_handle d, dir_reader& r) noexcept;

    /// @brief The next entry ("." and ".." are skipped). Unordered, and not
    /// a snapshot: entries created or removed during the listing may or may
    /// not appear.
    /// @pre dir_list_begin() was called with `d` and `r`
    status<dir_list_result> dir_list_next(
        dir_handle d, dir_reader& r, dir_entry& out) noexcept;

    /*****************************/
    /* CANONICAL PATHS           */
    /*****************************/

    /// @brief Result of path_canonical().
    enum class path_canonical_result : uint8_t
    {
      PATH_CANONICAL_OK,
      PATH_CANONICAL_BUFFER_TOO_SMALL, ///< `needed` holds the size required
      PATH_CANONICAL_DELETED,          ///< The entry no longer has a name
      PATH_CANONICAL_SYSTEM_ERROR,     ///< See native_code
    };

    /// @brief The absolute path of an open file or directory, with the case
    /// stored on disk, in UTF-8, without terminator (readlink of
    /// /proc/self/fd, F_GETPATH, GetFinalPathNameByHandleW). Windows: C:\x
    /// or \\server\share\x, or the \\?\ form when the plain one is not
    /// valid. A snapshot for logs, display and case checks: the name can
    /// change at any time, so do not reopen by it.
    status<path_canonical_result> path_canonical(
        file_handle h, char* buffer, size_t capacity, size_t& needed) noexcept;
    /// @copydoc path_canonical(file_handle, char*, size_t, size_t&)
    status<path_canonical_result> path_canonical(
        dir_handle d, char* buffer, size_t capacity, size_t& needed) noexcept;

    /*****************************/
    /* CURRENT DIRECTORY         */
    /*****************************/

    /// @brief Result of current_directory_get().
    enum class current_directory_get_result : uint8_t
    {
      CURRENT_DIRECTORY_GET_OK,
      CURRENT_DIRECTORY_GET_BUFFER_TOO_SMALL, ///< `needed` holds the size required
      CURRENT_DIRECTORY_GET_PATH_TOO_LONG,    ///< Longer than MAX_PATH_BYTES
      CURRENT_DIRECTORY_GET_DELETED,          ///< It was removed
      CURRENT_DIRECTORY_GET_SYSTEM_ERROR,
    };

    /// @brief The process's current directory, in UTF-8, without terminator.
    /// The library never uses it itself.
    status<current_directory_get_result> current_directory_get(
        char* buffer, size_t capacity, size_t& needed) noexcept;

    /// @brief Result of current_directory_set().
    enum class current_directory_set_result : uint8_t
    {
      CURRENT_DIRECTORY_SET_OK,
      CURRENT_DIRECTORY_SET_NOT_FOUND,
      CURRENT_DIRECTORY_SET_NOT_A_DIRECTORY,
      CURRENT_DIRECTORY_SET_ACCESS_DENIED,
      CURRENT_DIRECTORY_SET_INVALID_PATH,
      CURRENT_DIRECTORY_SET_PATH_NOT_ABSOLUTE,
      CURRENT_DIRECTORY_SET_PATH_TOO_LONG, ///< Windows: also above 259 characters without long-path opt-in
      CURRENT_DIRECTORY_SET_SYSTEM_ERROR,
    };

    /// @brief Changes the process's current directory. Process-wide: it
    /// changes relative paths for every thread and every library at once.
    status<current_directory_set_result> current_directory_set(
        const char* path, size_t path_size) noexcept;
  } // namespace v0_1
} // namespace osprim

#endif // !OSPRIM_PATH_HPP
