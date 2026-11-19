/*****************************************************************/ /**
 * @file   known_dir.hpp
 * @brief  Known directories: where the platform wants an application to
 * keep its data, its state, its caches and its temporary files, and where
 * the application itself is installed.
 *
 * Which function do I need?
 *
 * | Goal                                              | Use                                  |
 * |---------------------------------------------------|--------------------------------------|
 * | Work inside a known directory without races       | known_dir_open(), then the *_at calls|
 * | The path, for logs, display or another API        | known_dir_get()                      |
 * | Add my own vendor/game subdirectory, or not?      | known_dir_info::app_private          |
 * | Do two purposes share one directory?              | known_dir_info::same_as              |
 * | Keep downloaded content out of backups            | dir_set_backup_excluded() (path.hpp) |
 * | Redirect a directory (tests, portable installs,   | init_desc::known_dir_overrides       |
 * | Android before 13)                                |                                      |
 * | Where the code calling me lives (a DLL, a plugin) | dynlib_path_of() (dynlib.hpp)        |
 *
 * Purposes, not folders: each platform has its own set of special folders,
 * and osprim maps every purpose to the one the platform intends for it.
 *
 * | known_dir  | Windows           | macOS, iOS               | Linux                  | Android        |
 * |------------|-------------------|--------------------------|------------------------|----------------|
 * | DATA       | FOLDERID_Local-   | ~/Library/Application    | $XDG_DATA_HOME, else   | <root>/files   |
 * |            | AppData           | Support                  | ~/.local/share         |                |
 * | STATE      | (the same)        | (the same)               | $XDG_STATE_HOME, else  | <root>/        |
 * |            |                   |                          | ~/.local/state         | no_backup      |
 * | CACHE      | (the same)        | ~/Library/Caches         | $XDG_CACHE_HOME, else  | <root>/cache   |
 * |            |                   |                          | ~/.cache               |                |
 * | TEMP       | GetTempPath2W,    | confstr(_CS_DARWIN_      | $TMPDIR, else /tmp     | <root>/cache   |
 * |            | else GetTempPathW | USER_TEMP_DIR)           |                        |                |
 * | EXECUTABLE | directory of the  | directory of the         | directory of           | UNSUPPORTED    |
 * |            | .exe              | executable               | /proc/self/exe         |                |
 * | RESOURCES  | (the same)        | macOS: the main bundle's | (the same as           | UNSUPPORTED    |
 * |            |                   | Resources; iOS: the      | EXECUTABLE)            | (AAssetManager)|
 * |            |                   | bundle                   |                        |                |
 *
 * - DATA: what the user would miss (saves, settings, profiles). Included in
 *   the platform's backup where there is one.
 * - STATE: what the application keeps but can rebuild or download again
 *   (downloaded content, shader caches, logs). Never meant for backups:
 *   where STATE is also DATA (Windows, Apple), put it in a subdirectory
 *   and call dir_set_backup_excluded() on it.
 * - CACHE: what the application can lose at any time.
 * - TEMP: files for the current run. Never shared safely between
 *   processes: create entries with unique names, without replacing.
 * - EXECUTABLE, RESOURCES: read-only. Writing there fails (Program Files,
 *   /usr), breaks the code signature (Apple bundles) or is refused (iOS).
 *
 * Android: every directory lives under the application's data root, which
 * only Java knows. From Android 13 (API 33) the framework exports $TMPDIR =
 * <root>/cache to native code, and osprim takes the root from it (checked:
 * its last component is "cache", and it and its parent belong to the
 * process's user). Before Android 13, give the root through an override:
 * DATA = ANativeActivity::internalDataPath (or GameActivity's), which is
 * <root>/files; STATE, CACHE and TEMP are then derived from its parent
 * unless overridden too (only when its last component is "files": any
 * other DATA override leaves them to $TMPDIR or NOT_CONFIGURED). Without
 * either, every purpose is NOT_CONFIGURED.
 * Direct Boot applications running before the first unlock get
 * ACCESS_DENIED from known_dir_open(); isolated processes get
 * NOT_CONFIGURED.
 *
 * Snapshot: initialize() copies the overrides and the environment it needs
 * (HOME, XDG_DATA_HOME, XDG_STATE_HOME, XDG_CACHE_HOME, TMPDIR, Flatpak and
 * Snap markers, the Apple sandbox marker) into a static table. Later
 * changes to the environment are ignored: set them before initialize(), or
 * use overrides. Every function here may be called from any thread.
 * Windows folders are resolved at each call (they come from the shell, not
 * the environment); macOS and iOS paths come from HOME, so a sandboxed
 * process gets its container.
 *
 * The rules in a nutshell:
 * - Add your own vendor/game subdirectory where app_private is false.
 * - Use a subdirectory per purpose where same_as is not 0.
 * - Never delete a known directory itself, only entries you created in it
 *   (CACHE is LocalAppData on Windows, shared with every application).
 *********************************************************************/
#ifndef OSPRIM_KNOWN_DIR_HPP
#define OSPRIM_KNOWN_DIR_HPP

#include <osprim/base.hpp>
#include <osprim/path.hpp>
#include <osprim/platform.hpp>

#include <stddef.h>
#include <stdint.h>

#include <source_location>

namespace osprim
{
  inline namespace v0_1
  {
    /*****************************/
    /* PURPOSES                  */
    /*****************************/

    /// @brief What a directory is for (see the table above).
    enum class known_dir : uint8_t
    {
      KNOWN_DIR_DATA,       ///< What the user would miss; backed up
      KNOWN_DIR_STATE,      ///< Kept, rebuilt or downloaded again; not for backups
      KNOWN_DIR_CACHE,      ///< Can be lost at any time
      KNOWN_DIR_TEMP,       ///< For the current run
      KNOWN_DIR_EXECUTABLE, ///< The executable's directory; read-only
      KNOWN_DIR_RESOURCES,  ///< The application's bundled files; read-only
    };

    /// @brief Number of known_dir values.
    inline constexpr size_t KNOWN_DIR_COUNT = 6;

    /// @brief The bit of `d` in known_dir_info::same_as.
    constexpr uint8_t known_dir_bit(known_dir d) noexcept
    {
      return static_cast<uint8_t>(1u << static_cast<unsigned>(d));
    }

    /// @brief What the platform does with a known directory.
    ///
    /// | Flag          | true on                                                          |
    /// |---------------|------------------------------------------------------------------|
    /// | app_private   | iOS, Android, sandboxed macOS, Flatpak, Snap; EXECUTABLE and     |
    /// |               | RESOURCES everywhere; overrides                                  |
    /// | backed_up     | DATA and STATE on macOS (Time Machine) and iOS (iCloud, computer |
    /// |               | backups); DATA on Android (Auto Backup)                          |
    /// | may_be_purged | TEMP everywhere (Storage Sense, systemd-tmpfiles, reboots);      |
    /// |               | CACHE on Apple and Android                                       |
    struct known_dir_info
    {
      /// @brief Already specific to this application: do not add a
      /// vendor/game subdirectory. False whenever osprim cannot tell
      /// (false only costs an extra subdirectory).
      bool app_private;
      /// @brief The platform's own backup includes it. False for
      /// EXECUTABLE and RESOURCES.
      bool backed_up;
      /// @brief The system may delete its entries without the
      /// application's involvement (low storage, age, reboot).
      bool may_be_purged;
      /// @brief Set by init_desc::known_dir_overrides (for Android, also
      /// when derived from an overridden DATA). backed_up and
      /// may_be_purged then keep the purpose's values: the override is
      /// trusted to be a directory of the same kind.
      bool overridden;
      /// @brief known_dir_bit() of every other purpose resolving to the
      /// same path, compared as text after normalization (Windows:
      /// GetFullPathNameW, case ignored; elsewhere byte for byte): osprim's
      /// own mappings give identical text for the purposes they share. One directory reached through two spellings
      /// (symlinks, case on a case-insensitive volume, 8.3 names, bind
      /// mounts) can be missed; two directories are never reported as one.
      /// Nesting is not reported (Windows TEMP is inside LocalAppData).
      /// For a certain answer, open both and compare identities with
      /// path_info_at(dir, ".", ...).
      uint8_t same_as;
    };

    /*****************************/
    /* OVERRIDES                 */
    /*****************************/

    /// @brief Replaces a purpose's directory, through
    /// init_desc::known_dir_overrides. Copied by initialize(): the strings
    /// need not outlive the call. Not checked for existence.
    struct known_dir_override
    {
      known_dir which;
      /// @brief Absolute, UTF-8, at most MAX_PATH_BYTES, normalized on
      /// Windows as by file_open()
      const char* path;
      size_t path_size;
    };

    /*****************************/
    /* QUERIES                   */
    /*****************************/

    /// @brief Result of known_dir_get().
    enum class known_dir_get_result : uint8_t
    {
      KNOWN_DIR_GET_OK,
      KNOWN_DIR_GET_BUFFER_TOO_SMALL, ///< `needed` holds the size required
      KNOWN_DIR_GET_UNSUPPORTED, ///< The purpose does not exist here (Android EXECUTABLE, RESOURCES)
      KNOWN_DIR_GET_NOT_CONFIGURED, ///< Could not be determined: HOME unset, Android before 13
      ///< without an override, an isolated Android process
      KNOWN_DIR_GET_PATH_TOO_LONG, ///< Longer than MAX_PATH_BYTES
      KNOWN_DIR_GET_SYSTEM_ERROR,  ///< See native_code
    };

    /// @brief The directory of a purpose: absolute, UTF-8, without
    /// terminator or trailing separator. Windows: C:\x or \\server\share\x,
    /// the \\?\ form only when the plain one is not valid. The path is
    /// usable as a NO_DIRECTORY name. Nothing is created or opened.
    /// @param info Optional: what the platform does with it
    OSPRIM_API status<known_dir_get_result> known_dir_get(
        known_dir which, char* buffer, size_t capacity, size_t& needed,
        known_dir_info* info = nullptr) noexcept;

    /// @brief Result of known_dir_open().
    enum class known_dir_open_result : uint8_t
    {
      KNOWN_DIR_OPEN_OK,
      KNOWN_DIR_OPEN_UNSUPPORTED,    ///< As known_dir_get()
      KNOWN_DIR_OPEN_NOT_CONFIGURED, ///< As known_dir_get()
      KNOWN_DIR_OPEN_NOT_FOUND, ///< Missing, and `create` false or not applicable
      KNOWN_DIR_OPEN_NOT_A_DIRECTORY, ///< The path names something else
      KNOWN_DIR_OPEN_ACCESS_DENIED, ///< Permissions; Android Direct Boot before the first unlock
      KNOWN_DIR_OPEN_PATH_TOO_LONG,
      KNOWN_DIR_OPEN_TOO_MANY_OPEN,
      KNOWN_DIR_OPEN_NO_SPACE,     ///< `create` and the filesystem is full
      KNOWN_DIR_OPEN_SYSTEM_ERROR, ///< See native_code
    };

    /// @brief Opens the directory of a purpose (symlinks in its path are
    /// followed: ordinary home directories contain them). With `create`,
    /// missing components are created first (POSIX: mode 0700, as the XDG
    /// specification asks; Windows: the parent's inherited ACL): Linux XDG
    /// directories, Application Support and Android's no_backup often do
    /// not exist yet. `create` is ignored for EXECUTABLE and RESOURCES.
    /// @param info Optional: what the platform does with it
    OSPRIM_API status<known_dir_open_result> known_dir_open(
        known_dir which, bool create, dir_handle& out,
        known_dir_info* info     = nullptr,
        std::source_location loc = std::source_location::current()) noexcept;
  } // namespace v0_1
} // namespace osprim

#endif // !OSPRIM_KNOWN_DIR_HPP
