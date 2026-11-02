/*****************************************************************/ /**
 * @file   base.hpp
 * @brief  Library lifetime, results, native errors and diagnostics.
 *
 * Rules:
 * - osprim::initialize() is called exactly once before any other osprim
 *   function, and osprim::deinitialize() exactly once after the last one.
 *   initialize() may be called again after deinitialize().
 * - Neither is thread-safe: the caller orders them with everything else.
 * - Every function is noexcept and never throws.
 * - The library never allocates, except for handle tracking, which goes
 *   through the allocator given to initialize().
 * - Preconditions are checked with OSPRIM_ASSERT (default: assert()),
 *   which can be overridden through OSPRIM_USER_CONFIG.
 * - Headers and library of different versions do not link together
 *   (every symbol lives in a versioned inline namespace).
 *********************************************************************/
#ifndef OSPRIM_BASE_HPP
#define OSPRIM_BASE_HPP

#ifdef OSPRIM_USER_CONFIG
  #include OSPRIM_USER_CONFIG
#endif
#ifndef OSPRIM_ASSERT
  #include <cassert>
  /// @brief Precondition check. Override in OSPRIM_USER_CONFIG.
  #define OSPRIM_ASSERT(expr, msg) assert((expr) && (msg))
#endif
#ifndef OSPRIM_TRACKING
  /// @brief 1 if handle tracking is compiled in (set by CMake, PUBLIC).
  #define OSPRIM_TRACKING 0
#endif

#include <stddef.h>
#include <stdint.h>
#include <source_location>

namespace osprim
{
  inline namespace v0_1
  {
    /*****************************/
    /* RESULTS                   */
    /*****************************/

    /// @brief Which OS error space a native error code belongs to.
    enum class native_error_kind : uint8_t
    {
      NATIVE_ERROR_NONE,        ///< The OS provided no code
      NATIVE_ERROR_POSIX_ERRNO, ///< POSIX errno
      NATIVE_ERROR_WIN32,       ///< Win32 GetLastError()
      NATIVE_ERROR_NT_STATUS,   ///< NTSTATUS
      NATIVE_ERROR_MACH_KERN,   ///< Mach kern_return_t
    };

    /// @brief Outcome of a fallible operation, returned by value.
    /// `Result` is the operation's own enum, whose value 0 means success.
    /// Fits in 8 bytes and is returned in a single register.
    /// @tparam Result The operation's result enum (enum class : uint8_t)
    template<class Result>
    struct status
    {
      /// @brief The operation's result, `Result{}` (0) on success
      Result result;
      /// @brief Space of `native_code`, NATIVE_ERROR_NONE if the OS gave no code
      native_error_kind native_kind;
      /// @brief The OS error code behind `result` (errno, Win32, NTSTATUS, ...)
      int32_t native_code;

      /// @brief True on success
      constexpr bool ok() const noexcept { return result == Result{}; }
    };

    /*****************************/
    /* DIAGNOSTICS               */
    /*****************************/

    /// @brief What a diagnostic reports.
    enum class diagnostic_kind : uint8_t
    {
      DIAGNOSTIC_HANDLE_LEAK, ///< Still open at deinitialize()
      DIAGNOSTIC_HANDLE_MISUSE, ///< Closed twice, or used after being closed (best effort)
      /// @brief A READ_WRITE region maps unreserved space (a hole): writing
      /// there crashes the process if the disk fills up. The handle is the
      /// region (native_value: its data pointer).
      DIAGNOSTIC_REGION_OVER_HOLE,
      /// @brief A file was shrunk below a live region: touching the lost
      /// pages crashes the process (POSIX). The handle is the region.
      DIAGNOSTIC_REGION_TRUNCATED,
    };

    /// @brief One diagnostic, only valid during the callback.
    struct diagnostic
    {
      diagnostic_kind kind;
      /// @brief Static name of the handle type, never null
      const char* handle_type;
      /// @brief The native handle value
      uint64_t native_value;
      /// @brief Where the handle was created (default-constructed if unknown)
      std::source_location created_at;
      /// @brief Where the problem was detected (equal to created_at for leaks)
      std::source_location reported_at;
    };

    /// @brief Receives diagnostics. Must not call back into osprim.
    using diagnostic_fn = void (*)(void* user, const diagnostic& d) noexcept;

    /*****************************/
    /* INITIALIZATION            */
    /*****************************/

    /// @brief Allocation hooks, used only by handle tracking.
    struct allocator
    {
      void* user = nullptr;
      /// @brief null: malloc-based default
      void* (*alloc_fn)(void* user, size_t size, size_t align) noexcept = nullptr;
      /// @brief null: free-based default
      void (*free_fn)(void* user, void* ptr, size_t size, size_t align) noexcept =
          nullptr;
    };

    /// @brief Parameters of initialize().
    struct init_desc
    {
      /// @brief Receives leaks and misuse (null: dropped)
      diagnostic_fn on_diagnostic = nullptr;
      void* diagnostic_user       = nullptr;
      /// @brief Used only by handle tracking
      allocator alloc{};
      /// @brief Track handles (ignored if OSPRIM_TRACKING is 0)
      bool track_handles = true;
    };

    /// @brief Result of initialize().
    enum class initialize_result : uint8_t
    {
      INITIALIZE_OK,
      INITIALIZE_OUT_OF_MEMORY, ///< Tracking bookkeeping could not be allocated
      INITIALIZE_SYSTEM_ERROR,  ///< The OS refused a setup step (see native_code)
    };

    /// @brief Initializes the library. Does all one-off work (clock calibration,
    /// capability detection, platform setup). Can take ~10-20 ms on x86-64,
    /// where the tick frequency is calibrated.
    /// @pre Not already initialized.
    status<initialize_result> initialize(const init_desc& desc) noexcept;

    /// @brief Tears down the library and reports leaked handles.
    /// @pre Initialized, and no other osprim call in progress.
    void deinitialize() noexcept;

    /*****************************/
    /* BUILD INFORMATION         */
    /*****************************/

    /// @brief True if handle tracking is compiled in.
    inline constexpr bool tracking_compiled = OSPRIM_TRACKING != 0;
  } // namespace v0_1
} // namespace osprim

#endif // !OSPRIM_BASE_HPP
