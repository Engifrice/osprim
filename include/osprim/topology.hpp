/*****************************************************************/ /**
 * @file   topology.hpp
 * @brief  CPU topology: how much parallelism the process may use, which
 * cores are which, the caches, and thread affinity.
 *
 * Which function do I need?
 *
 * | Goal                                        | Use                                   |
 * |---------------------------------------------|---------------------------------------|
 * | How many worker threads to create           | get_cpu_budget(), parallelism_*()     |
 * | Which cores are fast, which are efficient   | topology_query()                      |
 * | Cache sizes, cores sharing them             | topology_query()                      |
 * | Padding against false sharing               | FALSE_SHARING_SIZE                    |
 * | Keep a thread on fast or efficient cores    | set_current_thread_class() first;     |
 * |                                             | then preferred, then allowed CPUs     |
 *
 * - The budget is cheap and can change at any time (affinity changes,
 *   containers resized, Android apps moved to the background): call it
 *   again every few seconds, and on lifecycle events such as resume. No OS
 *   notifies these changes, so polling is the only way.
 * - The topology is a snapshot of the machine: costly (it reads OS tables
 *   or sysfs), never taken by initialize(), cached by the caller.
 * - Affinity overrides the OS scheduler and its hardware hints: Intel and
 *   Google both advise against it. Prefer thread classes, then soft
 *   preferences; restrict hard only when measured to help, once, at
 *   thread start.
 *********************************************************************/
#ifndef OSPRIM_TOPOLOGY_HPP
#define OSPRIM_TOPOLOGY_HPP

#include <osprim/base.hpp>
#include <osprim/platform.hpp>

#include <stddef.h>
#include <stdint.h>

namespace osprim
{
  inline namespace v0_1
  {
    /*****************************/
    /* CACHE CONSTANTS           */
    /*****************************/

#if OSPRIM_ARCH_X86_64
    /// @brief Cache line size of the target (bytes).
    inline constexpr size_t CACHE_LINE_SIZE = 64;
    /// @brief Distance that keeps two independently written variables from
    /// slowing each other down. 128 on x86-64: the L2 "adjacent line"
    /// prefetcher fetches aligned pairs of 64-byte lines.
    inline constexpr size_t FALSE_SHARING_SIZE = 128;
#elif OSPRIM_OS_APPLE
    /// @brief Cache line size of the target (bytes): 128 on Apple silicon.
    inline constexpr size_t CACHE_LINE_SIZE = 128;
    /// @brief Distance that keeps two independently written variables from
    /// slowing each other down.
    inline constexpr size_t FALSE_SHARING_SIZE = 128;
#else
    /// @brief Cache line size of the target (bytes): 64 on other arm64 cores
    /// (Android, Linux, Windows on Arm).
    inline constexpr size_t CACHE_LINE_SIZE = 64;
    /// @brief Distance that keeps two independently written variables from
    /// slowing each other down.
    inline constexpr size_t FALSE_SHARING_SIZE = 64;
#endif

    /*****************************/
    /* BUDGET                    */
    /*****************************/

    /// @brief What a thread created now by the calling thread may use.
    ///
    /// New threads inherit their creator's CPUs on Linux and Android, and
    /// the process's on Windows: the budget follows that rule, so it always
    /// matches what new workers really get. Call it from the thread that
    /// creates the pool, usually the main thread.
    struct cpu_budget
    {
      /// @brief Logical CPUs a new thread may run on (affinity, cpusets,
      /// Android background restrictions, Windows CPU sets reserved for
      /// other processes). At least 1.
      uint32_t available_cpus;
      /// @brief CPU-time limit in thousandths of a CPU (1500: 1.5 CPUs),
      /// from container quotas (cgroups) or Windows job objects. 0: none.
      uint32_t quota_millicpus;
      /// @brief Period of the quota (usually 100 ms on Linux). 0: no quota,
      /// or not known (Windows).
      int64_t quota_period_ns;

      /// @brief Threads that can use the whole quota (rounded up, as Go and
      /// .NET do). Under full load, they may be throttled for part of each
      /// period. Without a quota: available_cpus.
      constexpr uint32_t parallelism_full() const noexcept
      {
        if (quota_millicpus == 0)
          return available_cpus;
        const uint32_t q = (quota_millicpus + 999) / 1000;
        return q < available_cpus ? q : available_cpus;
      }

      /// @brief Threads that never exceed the quota at full load (rounded
      /// down, at least 1). Without a quota: available_cpus.
      constexpr uint32_t parallelism_unthrottled() const noexcept
      {
        if (quota_millicpus == 0)
          return available_cpus;
        uint32_t q = quota_millicpus / 1000;
        if (q == 0)
          q = 1;
        return q < available_cpus ? q : available_cpus;
      }
    };

    /// @brief Result of get_cpu_budget().
    enum class cpu_budget_result : uint8_t
    {
      CPU_BUDGET_OK,
      CPU_BUDGET_SYSTEM_ERROR, ///< The OS refused (see native_code)
    };

    /// @brief What a thread created now by the calling thread may use. Cheap
    /// (a few microseconds): safe to call every few seconds.
    ///
    /// Platforms:
    /// - Linux, Android: the calling thread's affinity; cgroup v2 cpu.max or
    ///   v1 cpu.cfs_quota_us, the tightest along the hierarchy.
    /// - Windows: the process's affinity over all processor groups, minus
    ///   CPU sets reserved for other processes; job object CPU rate cap.
    /// - Apple: the active CPU count; no quota.
    OSPRIM_API status<cpu_budget_result> get_cpu_budget(cpu_budget* out) noexcept;

    /*****************************/
    /* TOPOLOGY                  */
    /*****************************/

    /// @brief Most logical CPUs described. Beyond, only the first are.
    inline constexpr size_t TOPOLOGY_MAX_CPUS = 2048;
    /// @brief Most core classes. More are merged into the last (slowest).
    inline constexpr size_t MAX_CORE_CLASSES = 4;

    /// @brief How the core classes were determined.
    enum class core_class_source : uint8_t
    {
      CORE_CLASS_SOURCE_UNIFORM,  ///< All cores alike: one class
      CORE_CLASS_SOURCE_REPORTED, ///< Reported by the OS
      CORE_CLASS_SOURCE_INFERRED, ///< Guessed from maximum frequencies (Linux, Android)
    };

    /// @brief One cache level. A size of 0: absent or not reported.
    struct cache_desc
    {
      uint32_t size;      ///< Bytes
      uint16_t line_size; ///< Bytes
      uint16_t shared_by; ///< Logical CPUs sharing one such cache; 0: not reported
    };

    /// @brief One class of cores (all cores of a class are alike).
    struct core_class_desc
    {
      uint32_t logical_cpus;
      uint32_t physical_cores;
      cache_desc l1d; ///< Level 1 data
      cache_desc l2;
      cache_desc l3;
    };

    /// @brief The machine as a whole.
    struct topology_summary
    {
      uint32_t logical_cpus;
      uint32_t physical_cores;
      uint32_t packages;
      /// @brief Number of entries used in `classes` (at least 1)
      uint32_t class_count;
      core_class_source class_source;
      /// @brief Fastest first: class 0 is the fastest
      core_class_desc classes[MAX_CORE_CLASSES];
    };

    /// @brief One logical CPU. Indices are dense (0, 1, 2, ...) and only
    /// meaningful within one query.
    struct cpu_desc
    {
      /// @brief The OS's own stable number (Linux CPU number, Windows CPU set
      /// id): what the affinity functions take.
      uint64_t id;
      uint32_t core;             ///< Physical core: hyper-threads share it
      uint32_t package;          ///< Physical package (socket)
      uint32_t last_level_cache; ///< CPUs sharing the last cache level share it
      uint32_t numa_node;
      uint8_t core_class; ///< Index into topology_summary::classes
      bool available;     ///< A new thread may run there (as get_cpu_budget())
    };

    /// @brief Result of topology_query().
    enum class topology_result : uint8_t
    {
      TOPOLOGY_OK,
      TOPOLOGY_SYSTEM_ERROR, ///< The OS refused (see native_code)
    };

    /// @brief A snapshot of the machine. Costly: call it when needed, and
    /// keep the result.
    ///
    /// Two-call pattern: with `cpus` null, fills the summary and sets
    /// `*count` to the number of CPU entries; then pass an array of that many.
    /// Entries are sorted by id; with a smaller array, the first `capacity`
    /// are written and `*count` still tells how many exist.
    ///
    /// Platforms:
    /// - Windows: CPU sets (EfficiencyClass), processor and cache relations.
    /// - Linux, Android: sysfs. Classes from cpu_capacity when its values
    ///   differ, else Intel's cpu_core/cpu_atom lists, else maximum
    ///   frequencies (INFERRED; gaps under 15% are not a new class).
    /// - Apple: performance levels (sysctl hw.perflevelN). No CPU entries
    ///   (`*count` is 0): Apple exposes no CPU identities.
    OSPRIM_API status<topology_result> topology_query(
        topology_summary* out, cpu_desc* cpus, size_t capacity,
        size_t* count) noexcept;

    /*****************************/
    /* AFFINITY                  */
    /*****************************/

    /// @brief Result of the affinity functions.
    enum class thread_affinity_result : uint8_t
    {
      THREAD_AFFINITY_OK,
      THREAD_AFFINITY_UNSUPPORTED, ///< Not available on this platform: nothing changed
      THREAD_AFFINITY_INVALID, ///< Unknown ids, or (Windows, hard) ids in several processor groups
      THREAD_AFFINITY_SYSTEM_ERROR, ///< The OS refused (see native_code)
    };

    /// @brief Soft: asks the OS to prefer these CPUs for the calling thread.
    /// The OS may still run it elsewhere (power, thermal, other work).
    ///
    /// Use when: keeping a thread on one class of cores where its thread
    ///   class is not enough, without fighting the scheduler.
    /// Don't use when: set_current_thread_class() does the job.
    /// Platforms:
    /// - Windows: CPU sets (SetThreadSelectedCpuSets), any processor group.
    /// - Linux, Android, Apple: UNSUPPORTED.
    /// @param ids cpu_desc::id values; none (count 0) removes the preference
    OSPRIM_API status<thread_affinity_result> set_current_thread_preferred_cpus(
        const uint64_t* ids, size_t count) noexcept;

    /// @brief Hard: the calling thread runs only on these CPUs (and threads
    /// it creates afterwards on Linux and Android).
    ///
    /// Use when: measured to help, set once at thread start, never changed
    ///   mid-frame.
    /// Don't use when: the OS knows better (hybrid CPUs: hard affinity
    ///   overrides Intel Thread Director and Android's placement), or the
    ///   CPUs may be taken away (Android background cpusets).
    /// Platforms:
    /// - Windows: group affinity: all ids in one processor group.
    /// - Linux, Android: sched_setaffinity (Android may refuse CPUs outside
    ///   the app's current cpuset).
    /// - Apple: UNSUPPORTED (no affinity API).
    /// @param ids cpu_desc::id values; none (count 0) gives back what the
    /// thread had by default (Linux, Android: the CPUs of the thread that
    /// called initialize(); Windows: the process's affinity in its primary
    /// group)
    OSPRIM_API status<thread_affinity_result> set_current_thread_allowed_cpus(
        const uint64_t* ids, size_t count) noexcept;
  } // namespace v0_1
} // namespace osprim

#endif // !OSPRIM_TOPOLOGY_HPP
