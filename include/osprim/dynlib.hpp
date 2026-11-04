/*****************************************************************/ /**
 * @file   dynlib.hpp
 * @brief  Dynamic libraries: load, look up symbols, release.
 *
 * Which function do I need?
 *
 * | Goal                                          | Use                                    |
 * |-----------------------------------------------|----------------------------------------|
 * | Load a plugin from a known file               | dynlib_open(), DYNLIB_SEARCH_PATH      |
 * | Load a system library (Vulkan, libandroid)    | dynlib_open(), DYNLIB_SEARCH_SYSTEM    |
 * | Find a function or variable in it             | dynlib_symbol()                        |
 * | Build a platform file name ("lib" ".so")      | DYNLIB_PREFIX, DYNLIB_SUFFIX           |
 *
 * Rules:
 * - A dynlib_handle is the native handle (a dlopen handle, an HMODULE).
 *   Loading a library again returns the same handle with one more
 *   reference; every dynlib_open() needs its dynlib_close().
 * - Symbols are bound at load time and stay local to the library
 *   (RTLD_NOW | RTLD_LOCAL): a missing symbol fails dynlib_open(), not the
 *   first call, and one library's symbols never satisfy another's
 *   undeclared dependencies (Windows has no other behavior).
 * - Loading runs the library's initializers (constructors, DllMain) under
 *   the loader lock: never load or release libraries from inside them.
 * - Releasing does not guarantee unloading: Apple keeps libraries that
 *   registered Objective-C or Swift code, glibc keeps those with
 *   thread_local destructors or unique symbols, musl never unloads; Windows
 *   unloads at the last release, and any pointer into the library then
 *   crashes. Do not rely on either: to reload changed code, copy it to a
 *   new file name each time (and on Apple, never overwrite a loaded image
 *   in place: replace it with a rename).
 * - Platform rules: iOS loads only libraries inside the app bundle
 *   (Frameworks/) or the OS, signed, never downloaded code. Android loads
 *   only the app's own libraries and the public NDK ones; uncompressed,
 *   aligned libraries load straight from the APK ("base.apk!/lib/arm64-v8a/
 *   libx.so").
 *********************************************************************/
#ifndef OSPRIM_DYNLIB_HPP
#define OSPRIM_DYNLIB_HPP

#include <osprim/base.hpp>
#include <osprim/platform.hpp>

#include <stddef.h>
#include <stdint.h>

#include <source_location>

namespace osprim
{
  inline namespace v0_1
  {
#if OSPRIM_OS_WINDOWS
    /// @brief Library file name prefix and suffix: foo.dll.
    inline constexpr const char* DYNLIB_PREFIX = "";
    inline constexpr const char* DYNLIB_SUFFIX = ".dll";
#elif OSPRIM_OS_APPLE
    /// @brief Library file name prefix and suffix: libfoo.dylib (frameworks:
    /// Foo.framework/Foo).
    inline constexpr const char* DYNLIB_PREFIX = "lib";
    inline constexpr const char* DYNLIB_SUFFIX = ".dylib";
#else
    /// @brief Library file name prefix and suffix: libfoo.so.
    inline constexpr const char* DYNLIB_PREFIX = "lib";
    inline constexpr const char* DYNLIB_SUFFIX = ".so";
#endif

    /// @brief Native library handle: a dlopen handle or an HMODULE.
    using native_dynlib_t = void*;

    /// @brief A loaded library: the native handle and nothing more.
    struct dynlib_handle
    {
      native_dynlib_t native;
    };

    /// @brief Where dynlib_open() looks.
    ///
    /// | search | name          | the library             | its dependencies                          |
    /// |--------|---------------|-------------------------|-------------------------------------------|
    /// | PATH   | absolute path | exactly that file       | Windows: its directory, then the system;  |
    /// |        |               |                         | elsewhere: its RUNPATH / @rpath           |
    /// | SYSTEM | bare name     | the OS's safe search    | the same                                  |
    ///
    /// Windows SYSTEM: the application directory, System32 and directories
    /// added with AddDllDirectory, never the current directory or PATH.
    enum class dynlib_search : uint8_t
    {
      DYNLIB_SEARCH_PATH,
      DYNLIB_SEARCH_SYSTEM,
    };

    /// @brief Parameters of dynlib_open().
    struct dynlib_open_desc
    {
      dynlib_search search = dynlib_search::DYNLIB_SEARCH_PATH;
      /// @brief Optional: receives the loader's message on failure (UTF-8,
      /// not terminated, truncated to `message_capacity`). POSIX: dlerror(),
      /// which names a missing dependency; Windows: the system message for
      /// the error.
      char* message           = nullptr;
      size_t message_capacity = 0;
    };

    /// @brief Result of dynlib_open().
    enum class dynlib_open_result : uint8_t
    {
      DYNLIB_OPEN_OK,
      DYNLIB_OPEN_NOT_FOUND,     ///< PATH: the file does not exist
      DYNLIB_OPEN_ACCESS_DENIED, ///< PATH: the file exists but cannot be read
      DYNLIB_OPEN_LOAD_FAILED, ///< Anything else: a missing dependency or symbol, the wrong
      ///< architecture, a refused library (Android namespaces, iOS
      ///< outside the bundle), a failed initializer, a missing system
      ///< library: see the message
      DYNLIB_OPEN_INVALID_PATH, ///< Not UTF-8; SYSTEM and a name with a separator
      DYNLIB_OPEN_PATH_NOT_ABSOLUTE, ///< PATH and a relative path
      DYNLIB_OPEN_PATH_TOO_LONG,     ///< Above MAX_PATH_BYTES
    };

    /// @brief Loads a library (or takes one more reference to it).
    /// @param message_size Receives the bytes written to `desc.message` (0 on success)
    /// @pre `name_size` > 0
    status<dynlib_open_result> dynlib_open(
        const char* name, size_t name_size, const dynlib_open_desc& desc,
        dynlib_handle& out, size_t* message_size = nullptr,
        std::source_location loc = std::source_location::current()) noexcept;

    /// @brief Releases one reference (the library may stay mapped, or be
    /// unmapped at once: see the rules above).
    /// @pre Opened, and nothing still using pointers into it
    void dynlib_close(
        dynlib_handle lib,
        std::source_location loc = std::source_location::current()) noexcept;

    /// @brief Longest symbol name of the pointer-and-size dynlib_symbol() (bytes).
    inline constexpr size_t DYNLIB_SYMBOL_MAX = 4095;

    /// @brief Result of dynlib_symbol().
    enum class dynlib_symbol_result : uint8_t
    {
      DYNLIB_SYMBOL_OK,
      DYNLIB_SYMBOL_NOT_FOUND,    ///< Not exported by the library
      DYNLIB_SYMBOL_INVALID_NAME, ///< Empty, embedded NUL, or above DYNLIB_SYMBOL_MAX
    };

    /// @brief The address of an exported function or variable. Cast it to
    /// the function pointer type (valid on every supported platform).
    /// @param name Terminated (usually a literal)
    status<dynlib_symbol_result> dynlib_symbol(
        dynlib_handle lib, const char* name, void*& out) noexcept;

    /// @copydoc dynlib_symbol(dynlib_handle, const char*, void*&)
    /// @param name `name_size` bytes, no terminator needed
    status<dynlib_symbol_result> dynlib_symbol(
        dynlib_handle lib, const char* name, size_t name_size, void*& out) noexcept;
  } // namespace v0_1
} // namespace osprim

#endif // !OSPRIM_DYNLIB_HPP
