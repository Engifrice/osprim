/*****************************************************************/ /**
 * @file   internal.hpp
 * @brief  Internal: state and helpers shared by the library sources.
 *
 * Not installed. The tests include it to exercise the tracking registry
 * directly.
 *********************************************************************/
#ifndef OSPRIM_SRC_INTERNAL_HPP
#define OSPRIM_SRC_INTERNAL_HPP

#include "platform.hpp"

#include <osprim/base.hpp>
#include <osprim/clock.hpp>
#include <osprim/dynlib.hpp>
#include <osprim/file.hpp>
#include <osprim/path.hpp>
#include <osprim/power.hpp>
#include <osprim/region.hpp>
#include <osprim/thread.hpp>
#include <osprim/topology.hpp>
#include <osprim/wait.hpp>

#include <stddef.h>
#include <stdint.h>

#include <source_location>

#if OSPRIM_ARCH_X86_64
  #if defined(_MSC_VER)
    #include <intrin.h>
  #else
    #include <x86intrin.h>
  #endif
#elif OSPRIM_ARCH_ARM64 && defined(_MSC_VER)
  #include <intrin.h>
#endif

namespace osprim
{
  inline namespace v0_1
  {
    namespace detail
    {
      /*****************************/
      /* LIBRARY STATE             */
      /*****************************/

      /// @brief True between initialize() and deinitialize().
      bool is_initialized() noexcept;

      /// @brief Allocates through the hooks given to initialize().
      /// @return null on failure
      void* allocate(size_t size, size_t align) noexcept;
      /// @brief Frees memory returned by allocate() with the same size and alignment.
      void deallocate(void* ptr, size_t size, size_t align) noexcept;

      /// @brief Sends a diagnostic to the callback given to initialize(), if any.
      void report(const diagnostic& d) noexcept;

      /// @brief A process-unique non-zero id, for handles whose native
      /// value is not unique (or absent) and must still be tracked.
      uint64_t next_handle_id() noexcept;

      /*****************************/
      /* HANDLE TRACKING           */
      /*****************************/

      // Every module that creates handles calls track_open() after a
      // successful creation and track_close() before releasing the handle.
      // A handle is identified by its type name and native value. These are
      // no-ops when tracking is not compiled in or disabled at runtime.

      /// @brief What a module records with a handle for its Debug checks
      /// (meaning chosen by the module).
      struct track_meta
      {
        uint64_t words[5];
      };

      /// @brief Selects handles in track_find().
      using track_match_fn =
          bool (*)(const track_meta& meta, const void* ctx) noexcept;

#if OSPRIM_TRACKING
      /// @brief Records an open handle, with optional metadata.
      void track_open(
          const char* type, uint64_t native, std::source_location loc,
          const track_meta* meta = nullptr) noexcept;
      /// @brief Forgets a handle (one reference of a shared one), reporting
      /// DIAGNOSTIC_HANDLE_MISUSE if it is not open.
      void track_close(
          const char* type, uint64_t native, std::source_location loc) noexcept;
      /// @brief Records one more reference to a handle the OS shares between
      /// opens (dynamic libraries): opening it again is not misuse.
      void track_open_shared(
          const char* type, uint64_t native, std::source_location loc) noexcept;
      /// @brief Number of handles currently recorded.
      size_t tracked_count() noexcept;
      /// @brief The metadata of a recorded handle.
      /// @return false if not recorded (or tracking is off)
      bool track_get(const char* type, uint64_t native, track_meta* out) noexcept;
      /// @brief The first recorded handle of `type` that `match` accepts
      /// (called under the registry lock: it must not call osprim).
      /// @return false if none
      bool track_find(
          const char* type, track_match_fn match, const void* ctx, uint64_t* native,
          std::source_location* created_at) noexcept;
#else
      inline void track_open(
          const char*, uint64_t, std::source_location,
          const track_meta* = nullptr) noexcept
      {
      }
      inline void track_close(const char*, uint64_t, std::source_location) noexcept
      {
      }
      inline void track_open_shared(
          const char*, uint64_t, std::source_location) noexcept
      {
      }
      inline size_t tracked_count() noexcept
      {
        return 0;
      }
      inline bool track_get(const char*, uint64_t, track_meta*) noexcept
      {
        return false;
      }
      inline bool track_find(
          const char*, track_match_fn, const void*, uint64_t*,
          std::source_location*) noexcept
      {
        return false;
      }
#endif

      /*****************************/
      /* CLOCKS                    */
      /*****************************/

      /// @brief Clock state, written by clock_initialize() only.
      struct clock_state
      {
        /// @brief ticks_to_ns(t) = (t * ticks_mult) >> ticks_shift (128-bit product)
        uint64_t ticks_mult;
        uint32_t ticks_shift;
        uint64_t ticks_frequency_hz;
        /// @brief clock_resolution_ns(), indexed by clock_id
        int64_t resolution_ns[4];
        tick_source ticks_source;
        bool ticks_frequency_exact;
#if OSPRIM_OS_APPLE
        /// @brief mach_timebase_info: ns = ticks * numer / denom
        uint32_t mach_numer;
        uint32_t mach_denom;
#endif
      };

      extern clock_state g_clock;

      /// @brief Fixed-point factor converting ticks to nanoseconds.
      struct ticks_conversion
      {
        uint64_t mult;
        uint32_t shift;
      };

      /// @brief The factor for a counter running at `hz`: mult is
      /// ceil(1e9 * 2^shift / hz), with shift as large as possible (<= 63).
      /// Rounding up makes the conversion exact (equal to
      /// floor(t * 1e9 / hz)) for every t below 2^shift / (hz / gcd(hz, 1e9)):
      /// decades for every real counter (127 years at 24 MHz, 39 at
      /// 19.2 MHz, 97 at 3 GHz). Beyond, it is a few nanoseconds too large at most.
      /// @pre hz != 0
      ticks_conversion make_ticks_conversion(uint64_t hz) noexcept;

      /// @brief Fills g_clock: platform clocks, then the tick counter.
      status<initialize_result> clock_initialize() noexcept;

      // Implemented by the platform source (clock_linux.cpp, ...):

      /// @brief Platform setup: resolutions of the awake, continuous and wall
      /// clocks, plus anything awake_now() needs.
      status<initialize_result> platform_clock_initialize() noexcept;
      /// @brief Sleeps for about `ns` nanoseconds (used for calibration only).
      void platform_sleep_ns(int64_t ns) noexcept;

      /*****************************/
      /* RAW TICK COUNTER          */
      /*****************************/

#if OSPRIM_ARCH_ARM64 && defined(_MSC_VER)
  // Values of ARM64_SYSREG() from winnt.h, without including windows.h
  #define OSPRIM_ARM64_SYSREG(op0, op1, crn, crm, op2)               \
    ((((op0) & 1) << 14) | (((op1) & 7) << 11) | (((crn) & 15) << 7) \
     | (((crm) & 15) << 3) | ((op2) & 7))
  #define OSPRIM_ARM64_CNTVCT_EL0 OSPRIM_ARM64_SYSREG(3, 3, 14, 0, 2)
  #define OSPRIM_ARM64_CNTFRQ_EL0 OSPRIM_ARM64_SYSREG(3, 3, 14, 0, 0)
#endif

      /// @brief Reads the hardware counter (TSC or CNTVCT_EL0), no fence.
      inline uint64_t read_hardware_ticks() noexcept
      {
#if OSPRIM_ARCH_X86_64
        return __rdtsc();
#elif defined(_MSC_VER)
        return static_cast<uint64_t>(_ReadStatusReg(OSPRIM_ARM64_CNTVCT_EL0));
#else
        uint64_t v;
        __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
        return v;
#endif
      }

#if OSPRIM_OS_APPLE
      /// @brief Mach ticks reaching at least `ns` awake nanoseconds (ns >= 0).
      /// @return false if not representable (centuries away)
      bool awake_ns_to_mach_ceil(int64_t ns, uint64_t* ticks) noexcept;
#endif

      /*****************************/
      /* THREADS                   */
      /*****************************/

      /// @brief Body of every thread, called by the native trampoline:
      /// publishes the id and the states, runs the entry function.
      void run_thread(thread_context* ctx) noexcept;

      // Implemented by the platform source (thread_posix.cpp, ...):

      /// @brief Starts a native thread running run_thread(&ctx), sets ctx.native.
      status<thread_create_result> platform_thread_create(
          thread_context& ctx, size_t stack_size) noexcept;
      /// @brief Releases a thread whose state is EXITED (waits for the last
      /// few instructions of the trampoline).
      void platform_thread_join(uint64_t native) noexcept;
      /// @brief Releases a thread that may still run.
      void platform_thread_detach(uint64_t native) noexcept;

      /*****************************/
      /* POWER                     */
      /*****************************/

      /// @brief Set by power_initialize(), constant until deinitialize().
      extern power_features g_power_features;

      /// @brief Detects the supported goals (never fails: unsupported is false).
      void power_initialize() noexcept;
      /// @brief Releases what power_initialize() acquired.
      void power_deinitialize() noexcept;

      // Implemented by the platform source (power_windows.cpp, ...):

      void platform_power_initialize(power_features* out) noexcept;
      void platform_power_deinitialize() noexcept;
      status<timer_resolution_result> platform_timer_resolution_begin(
          int64_t period_ns, uint32_t* period_ms) noexcept;
      void platform_timer_resolution_end(uint32_t period_ms) noexcept;
      /// @param reason UTF-8, at most ACTIVITY_REASON_MAX bytes
      status<activity_result> platform_activity_begin(
          const activity_desc& desc, uint64_t* native,
          activity_desc* granted) noexcept;
      void platform_activity_end(
          uint64_t native, const activity_desc& granted) noexcept;
      /// @param tids `count` thread ids, count in [1, PERF_SESSION_MAX_THREADS]
      status<perf_session_result> platform_perf_session_create(
          const uint64_t* tids, size_t count, int64_t target_ns,
          uint64_t* native) noexcept;
      status<perf_session_result> platform_perf_session_set_target(
          uint64_t native, int64_t target_ns) noexcept;
      status<perf_session_result> platform_perf_session_report(
          uint64_t native, int64_t actual_ns) noexcept;
      void platform_perf_session_close(uint64_t native) noexcept;
      status<thermal_result> platform_thermal_level(thermal_level* out) noexcept;
      status<thermal_result> platform_thermal_watch_begin(
          std::atomic<wait_word_t>& counter, uint64_t* native,
          int32_t* token) noexcept;
      void platform_thermal_watch_end(uint64_t native, int32_t token) noexcept;
      status<thermal_result> platform_thermal_headroom(
          int64_t forecast_ns, float* out) noexcept;

      /*****************************/
      /* TOPOLOGY                  */
      /*****************************/

      /// @brief One logical CPU as a platform source describes it. Keys only
      /// need to be equal for equal things: dense indices are derived later.
      struct cpu_record
      {
        uint64_t id;
        uint64_t core_key;
        uint64_t package_key;
        uint64_t llc_key;
        uint64_t node_key;
        /// @brief Higher is faster; equal ranks are one class
        uint64_t rank;
        /// @brief Free for the platform source (Windows: group << 8 | index)
        uint64_t location;
        bool available;
      };

      /// @brief Working storage of topology_query() and of the affinity
      /// functions, used under topology_lock (the library does not allocate).
      struct topology_scratch
      {
        cpu_record records[TOPOLOGY_MAX_CPUS];
        uint8_t classes[TOPOLOGY_MAX_CPUS];
        uint32_t dense[TOPOLOGY_MAX_CPUS];
      };

      extern topology_scratch g_topology_scratch;

      /// @brief Guards g_topology_scratch and the platform scratch buffers.
      struct topology_lock
      {
        topology_lock() noexcept;
        ~topology_lock();
        topology_lock(const topology_lock&)            = delete;
        topology_lock& operator=(const topology_lock&) = delete;
      };

      /// @brief Sorts records by id, assigns classes (distinct ranks, highest
      /// first, beyond MAX_CORE_CLASSES merged into the last), fills the
      /// summary counts and writes the caller's entries with dense indices.
      /// The caches of `out->classes` are left to the caller.
      /// @param multi_source Source when there are several classes
      /// @param first_of_class Receives the index of one record per class
      void finish_topology(
          cpu_record* records, uint8_t* classes, size_t n,
          core_class_source multi_source, topology_summary* out, cpu_desc* cpus,
          size_t capacity, size_t first_of_class[MAX_CORE_CLASSES]) noexcept;

      /// @brief Saves what set_current_thread_allowed_cpus() gives back for an
      /// empty list (Linux, Android: the calling thread's CPUs).
      void topology_initialize() noexcept;

      // Implemented by the platform source (topology_linux.cpp, ...):

      status<cpu_budget_result> platform_cpu_budget(cpu_budget* out) noexcept;
      status<topology_result> platform_topology_query(
          topology_summary* out, cpu_desc* cpus, size_t capacity,
          size_t* count) noexcept;
      status<thread_affinity_result> platform_preferred_cpus(
          const uint64_t* ids, size_t count) noexcept;
      status<thread_affinity_result> platform_allowed_cpus(
          const uint64_t* ids, size_t count) noexcept;
      void platform_topology_initialize() noexcept;

#if OSPRIM_OS_LINUX || OSPRIM_OS_ANDROID
      /// @brief A set of CPU numbers (as the kernel's cpu_set_t, larger).
      struct cpu_bits
      {
        uint64_t words[TOPOLOGY_MAX_CPUS / 64];

        void set(size_t cpu) noexcept
        {
          words[cpu / 64] |= uint64_t{1} << (cpu % 64);
        }
        bool test(size_t cpu) const noexcept
        {
          return cpu < TOPOLOGY_MAX_CPUS
                 && ((words[cpu / 64] >> (cpu % 64)) & 1u) != 0;
        }
        size_t count() const noexcept;
      };

      /// @brief The topology under `root` (empty: the real one; tests pass a
      /// directory holding a fake sys/ tree), with `allowed` as the CPUs a new
      /// thread may use.
      status<topology_result> linux_topology(
          const char* root, const cpu_bits& allowed, topology_summary* out,
          cpu_desc* cpus, size_t capacity, size_t* count) noexcept;

      /// @brief The tightest cgroup CPU quota of this process, read from
      /// `root`/proc/self/cgroup and `root`/sys/fs/cgroup (empty: the real ones).
      void linux_cgroup_quota(
          const char* root, uint32_t* millicpus, int64_t* period_ns) noexcept;
#endif

#if OSPRIM_OS_WINDOWS
      /// @brief Records from a GetSystemCpuSetInformation buffer, with the
      /// process's allowed groups and primary-group mask (`primary_mask` 0:
      /// every CPU of the allowed groups). Exposed for tests.
      size_t windows_parse_cpu_sets(
          const void* buffer, size_t size, const uint16_t* allowed_groups,
          size_t group_count, uint16_t primary_group, uint64_t primary_mask,
          cpu_record* records, size_t capacity) noexcept;
#endif

      /*****************************/
      /* FILES AND REGIONS         */
      /*****************************/

      /// @brief Tracking type names.
      inline constexpr const char* FILE_TRACK_TYPE   = "file";
      inline constexpr const char* REGION_TRACK_TYPE = "region";

      /// @brief The native handle as a tracking key.
      inline uint64_t file_key(native_file_t native) noexcept
      {
#if OSPRIM_OS_WINDOWS
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(native));
#else
        return static_cast<uint64_t>(static_cast<int64_t>(native));
#endif
      }

      /// @brief True if `p[0, size)` is UTF-8 without NUL, overlong forms,
      /// surrogates or code points above U+10FFFF.
      bool utf8_valid(const char* p, size_t size) noexcept;

      /// @brief Caches the page size and the mapping granularity.
      void file_initialize() noexcept;
      /// @brief Releases what the file backend holds (Windows: the events).
      void file_deinitialize() noexcept;

      // Implemented by the platform source (file_posix.cpp, file_windows.cpp).
      // Arguments are validated by file.cpp.

      void platform_file_initialize(size_t* page_size, size_t* granularity) noexcept;
      void platform_file_deinitialize() noexcept;
      /// @param path Valid UTF-8 without NUL, at most MAX_PATH_BYTES bytes
      status<file_open_result> platform_file_open(
          const char* path, size_t size, const file_open_desc& desc,
          native_file_t* out) noexcept;
      void platform_file_close(native_file_t native) noexcept;
      /// @param unbuffered null: no detection wanted
      status<file_adopt_result> platform_file_adopt(
          native_file_t native, bool* unbuffered) noexcept;
      status<file_read_result> platform_file_read(
          native_file_t native, uint64_t offset, void* buffer, size_t size,
          size_t* transferred) noexcept;
      status<file_write_result> platform_file_write(
          native_file_t native, uint64_t offset, const void* buffer, size_t size,
          size_t* transferred) noexcept;
      status<file_flush_result> platform_file_flush(native_file_t native) noexcept;
      status<file_info_result> platform_file_info(
          native_file_t native, file_info* out) noexcept;
      status<file_set_size_result> platform_file_set_size(
          native_file_t native, uint64_t size, file_size_mode mode) noexcept;
      status<file_alignment_result> platform_file_alignment(
          native_file_t native, file_alignment* out) noexcept;
      /// @brief Whether [offset, offset + size) has unreserved space (Debug check).
      /// @return false if it could not be determined (no report)
      bool platform_file_has_hole(
          native_file_t native, uint64_t offset, size_t size) noexcept;

      /// @brief Maps `length` bytes from `aligned_offset` (a multiple of the
      /// granularity), after checking that `end` is within the file.
      status<region_map_result> platform_region_map(
          native_file_t native, uint64_t aligned_offset, size_t length, uint64_t end,
          region_access access, void** base) noexcept;
      void platform_region_unmap(void* base, size_t length) noexcept;
      /// @param address Page-aligned
      status<region_flush_result> platform_region_flush(
          void* address, size_t length) noexcept;

      /// @brief The page size (region_flush alignment).
      size_t page_size() noexcept;

#if OSPRIM_OS_WINDOWS
      /// @brief Number of events in the I/O event pool. Calls beyond it
      /// create a temporary event each.
      inline constexpr size_t FILE_EVENT_POOL_SIZE = 64;

      /// @brief An event for one blocking I/O call.
      struct file_event
      {
        void* handle;
        /// @brief Pool slot, or UINT32_MAX for a temporary event
        uint32_t slot;
      };

      /// @brief Takes an event from the pool, or creates a temporary one.
      /// @return handle null on failure (GetLastError() set)
      file_event file_event_acquire() noexcept;
      /// @brief Returns a pooled event, or closes a temporary one.
      void file_event_release(file_event e) noexcept;

      /// @brief Converts a validated UTF-8 path to the \\?\ form CreateFileW
      /// gets (exposed for tests). `out` holds `capacity` UTF-16 units.
      status<file_open_result> windows_file_path(
          const char* path, size_t size, wchar_t* out, size_t capacity,
          bool* verbatim) noexcept;
#endif

      /*****************************/
      /* PATHS                     */
      /*****************************/

      inline constexpr const char* DIR_TRACK_TYPE = "dir";

      /// @brief The native directory handle as a tracking key.
      inline uint64_t dir_key(native_dir_t native) noexcept
      {
#if OSPRIM_OS_WINDOWS
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(native));
#else
        return static_cast<uint64_t>(static_cast<int64_t>(native));
#endif
      }

      /// @brief A name checked by path.cpp: either an absolute path as the
      /// caller gave it (`absolute`, `dir` is NO_DIRECTORY), or a relative
      /// name normalized to components joined by '/', without "." or empty
      /// components and without ".." (`size` 0: the directory itself).
      /// Always terminated.
      struct path_ref
      {
        native_dir_t dir;
        const char* name;
        size_t size;
        bool absolute;
      };

#if !OSPRIM_OS_WINDOWS
      /// @brief open(2) flags for a file_open_desc (POSIX).
      int posix_file_flags(const file_open_desc& desc) noexcept;
      /// @brief Maps an open(2) errno (POSIX).
      status<file_open_result> posix_open_error(int e, bool unbuffered) noexcept;
      /// @brief Checks the type and applies the hints of a just-opened file
      /// (POSIX); closes `fd` on failure.
      status<file_open_result> posix_file_finish(
          int fd, const file_open_desc& desc, native_file_t* out) noexcept;
#endif

      /// @brief Records a file opened with `desc` (file.cpp).
      void file_track_open(
          native_file_t native, const file_open_desc& desc,
          std::source_location loc) noexcept;

      /// @brief Copies `n` bytes into a caller buffer if they fit; `*needed` = n.
      bool copy_out(
          const char* src, size_t n, char* buffer, size_t capacity,
          size_t* needed) noexcept;

#if OSPRIM_OS_WINDOWS
      /// @brief A name component a relative NT open would accept but Win32
      /// tools cannot handle (device names, trailing dot or space, < > : " | ? *).
      bool windows_component_refused(const char* c, size_t n) noexcept;
#endif

      // Implemented by the platform source (path_posix.cpp, path_windows.cpp)

      status<dir_open_result> platform_dir_open(
          const path_ref& p, path_resolve resolve, native_dir_t* out) noexcept;
      void platform_dir_close(native_dir_t d) noexcept;
      status<dir_adopt_result> platform_dir_adopt(native_dir_t d) noexcept;
      /// @param p Never the directory itself
      status<file_open_result> platform_file_open_at(
          const path_ref& p, path_resolve resolve, const file_open_desc& desc,
          native_file_t* out) noexcept;
      status<path_info_result> platform_path_info(
          const path_ref& p, path_resolve resolve, path_info* out) noexcept;
      /// @param p Never the directory itself
      status<dir_create_result> platform_dir_create(
          const path_ref& p, path_resolve resolve, native_dir_t* out) noexcept;
      /// @param p Never the directory itself
      status<path_remove_result> platform_path_remove(
          const path_ref& p, path_resolve resolve, path_kind kind) noexcept;
      /// @param from, to Never a directory itself
      status<path_rename_result> platform_path_rename(
          const path_ref& from, const path_ref& to,
          const path_rename_desc& desc) noexcept;
      /// @param p Never the directory itself
      status<path_read_link_result> platform_path_read_link(
          const path_ref& p, path_resolve resolve, char* buffer, size_t capacity,
          size_t* needed, link_info* info) noexcept;
      status<dir_list_result> platform_dir_list_begin(
          native_dir_t d, dir_reader& r) noexcept;
      status<dir_list_result> platform_dir_list_next(
          native_dir_t d, dir_reader& r, dir_entry* out) noexcept;
      /// @param native A file or directory handle (the same native type)
      status<path_canonical_result> platform_canonical(
          native_file_t native, char* buffer, size_t capacity,
          size_t* needed) noexcept;
      status<current_directory_get_result> platform_current_directory_get(
          char* buffer, size_t capacity, size_t* needed) noexcept;
      /// @param path Absolute, terminated
      status<current_directory_set_result> platform_current_directory_set(
          const char* path, size_t size) noexcept;

      /*****************************/
      /* DYNAMIC LIBRARIES         */
      /*****************************/

      inline constexpr const char* DYNLIB_TRACK_TYPE = "dynlib";

      // Implemented by the platform source (dynlib_posix.cpp, dynlib_windows.cpp)

      /// @param name Validated by dynlib.cpp, terminated: an absolute path
      /// (PATH) or a bare name (SYSTEM)
      status<dynlib_open_result> platform_dynlib_open(
          const char* name, size_t size, const dynlib_open_desc& desc,
          native_dynlib_t* out, size_t* message_size) noexcept;
      void platform_dynlib_close(native_dynlib_t lib) noexcept;
      /// @param name Terminated
      void* platform_dynlib_symbol(native_dynlib_t lib, const char* name) noexcept;

      /*****************************/
      /* WAIT                      */
      /*****************************/

      /// @brief The storage of a wait word, as the OS primitives take it.
      inline wait_word_t* wait_address(const std::atomic<wait_word_t>& word) noexcept
      {
        return const_cast<wait_word_t*>(reinterpret_cast<const wait_word_t*>(&word));
      }

      /*****************************/
      /* ARITHMETIC                */
      /*****************************/

      /// @brief High and low halves of a 64x64 bit product.
      inline uint64_t mul_64x64(uint64_t a, uint64_t b, uint64_t* hi) noexcept
      {
#if defined(__SIZEOF_INT128__)
        // __extension__: __int128 is not ISO C++ (-Wpedantic)
        __extension__ using u128 = unsigned __int128;
        const u128 p             = static_cast<u128>(a) * b;
        *hi                      = static_cast<uint64_t>(p >> 64);
        return static_cast<uint64_t>(p);
#elif defined(_MSC_VER) && defined(_M_X64)
        return _umul128(a, b, hi);
#elif defined(_MSC_VER) && defined(_M_ARM64)
        *hi = __umulh(a, b);
        return a * b;
#else
  #error "osprim: no 64x64->128 bit multiplication for this compiler"
#endif
      }
    } // namespace detail
  } // namespace v0_1
} // namespace osprim

#endif // !OSPRIM_SRC_INTERNAL_HPP
