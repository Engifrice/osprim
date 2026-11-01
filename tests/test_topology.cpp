#include "support.hpp"

#include "internal.hpp"

#include <osprim/thread.hpp>
#include <osprim/topology.hpp>

#include <doctest/doctest.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <set>
#include <thread>
#include <type_traits>
#include <vector>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#elif defined(__linux__) || defined(__ANDROID__)
  #include <ftw.h>
  #include <sched.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

using namespace osprim;
using osprim_test::note;
using osprim_test::session;

/*****************************/
/* COMPILE-TIME PROPERTIES   */
/*****************************/

static_assert(FALSE_SHARING_SIZE >= CACHE_LINE_SIZE);
static_assert((CACHE_LINE_SIZE & (CACHE_LINE_SIZE - 1)) == 0);
static_assert((FALSE_SHARING_SIZE & (FALSE_SHARING_SIZE - 1)) == 0);
static_assert(std::is_aggregate_v<cpu_budget>);
static_assert(std::is_trivially_copyable_v<topology_summary>);
static_assert(std::is_trivially_copyable_v<cpu_desc>);
static_assert(static_cast<int>(cpu_budget_result::CPU_BUDGET_OK) == 0);
static_assert(static_cast<int>(topology_result::TOPOLOGY_OK) == 0);
static_assert(static_cast<int>(thread_affinity_result::THREAD_AFFINITY_OK) == 0);

// No quota: every CPU
static_assert(cpu_budget{8, 0, 0}.parallelism_full() == 8);
static_assert(cpu_budget{8, 0, 0}.parallelism_unthrottled() == 8);
// 1.5 CPUs: 2 to use it all, 1 never to be throttled
static_assert(cpu_budget{8, 1500, 100'000'000}.parallelism_full() == 2);
static_assert(cpu_budget{8, 1500, 100'000'000}.parallelism_unthrottled() == 1);
// Whole CPUs: both agree
static_assert(cpu_budget{8, 3000, 100'000'000}.parallelism_full() == 3);
static_assert(cpu_budget{8, 3000, 100'000'000}.parallelism_unthrottled() == 3);
// Never below 1, never above the available CPUs
static_assert(cpu_budget{8, 500, 100'000'000}.parallelism_unthrottled() == 1);
static_assert(cpu_budget{8, 500, 100'000'000}.parallelism_full() == 1);
static_assert(cpu_budget{2, 6000, 100'000'000}.parallelism_full() == 2);
static_assert(cpu_budget{2, 6000, 100'000'000}.parallelism_unthrottled() == 2);

namespace
{
  const char* source_name(core_class_source s)
  {
    switch (s)
    {
    case core_class_source::CORE_CLASS_SOURCE_UNIFORM:
      return "uniform";
    case core_class_source::CORE_CLASS_SOURCE_REPORTED:
      return "reported";
    case core_class_source::CORE_CLASS_SOURCE_INFERRED:
      return "inferred";
    }
    return "?";
  }

  /// @brief Checks what must hold for any topology.
  void check_consistent(
      const topology_summary& t, const cpu_desc* cpus, size_t count)
  {
    REQUIRE(t.class_count >= 1);
    REQUIRE(t.class_count <= MAX_CORE_CLASSES);
    CHECK(
        (t.class_count == 1)
        == (t.class_source == core_class_source::CORE_CLASS_SOURCE_UNIFORM));
    CHECK(t.logical_cpus >= 1);
    CHECK(t.physical_cores >= 1);
    CHECK(t.physical_cores <= t.logical_cpus);
    CHECK(t.packages >= 1);
    uint32_t logical = 0, physical = 0;
    for (uint32_t k = 0; k < t.class_count; ++k)
    {
      logical += t.classes[k].logical_cpus;
      physical += t.classes[k].physical_cores;
      CHECK(t.classes[k].logical_cpus >= 1);
      CHECK(t.classes[k].physical_cores <= t.classes[k].logical_cpus);
    }
    CHECK(logical == t.logical_cpus);
    CHECK(physical == t.physical_cores);

    std::set<uint64_t> ids;
    for (size_t i = 0; i < count; ++i)
    {
      CAPTURE(i);
      CHECK(cpus[i].core_class < t.class_count);
      CHECK(cpus[i].core < t.physical_cores);
      CHECK(cpus[i].package < t.packages);
      if (i > 0)
        CHECK(cpus[i].id > cpus[i - 1].id); // sorted, unique
      ids.insert(cpus[i].id);
    }
  }

  void print_topology(const topology_summary& t, size_t count)
  {
    note(
        "topology: %u logical CPUs, %u cores, %u package(s), %llu CPU entries, %u "
        "class(es) (%s)",
        t.logical_cpus, t.physical_cores, t.packages,
        static_cast<unsigned long long>(count), t.class_count,
        source_name(t.class_source));
    for (uint32_t k = 0; k < t.class_count; ++k)
    {
      const core_class_desc& c = t.classes[k];
      note(
          "  class %u: %u logical, %u cores; L1d %u B (line %u, shared by %u), L2 "
          "%u B (shared by "
          "%u), L3 %u B (shared by %u)",
          k, c.logical_cpus, c.physical_cores, c.l1d.size, c.l1d.line_size,
          c.l1d.shared_by, c.l2.size, c.l2.shared_by, c.l3.size, c.l3.shared_by);
    }
  }

  /// @brief An integer from the environment (set by CI jobs), or -1.
  long long env_number(const char* name)
  {
#if defined(_MSC_VER)
    char* value = nullptr;
    size_t len  = 0;
    if (_dupenv_s(&value, &len, name) != 0 || value == nullptr)
      return -1;
    const long long v = atoll(value);
    free(value);
    return v;
#else
    const char* value = getenv(name);
    return value != nullptr ? atoll(value) : -1;
#endif
  }
} // namespace

/*****************************/
/* THIS MACHINE              */
/*****************************/

TEST_CASE("topology: budget")
{
  session s;
  cpu_budget b{};
  REQUIRE(get_cpu_budget(&b).ok());
  note(
      "budget: %u available CPUs, quota %u millicpus per %lld ns: parallelism %u "
      "full, %u "
      "unthrottled (hardware_concurrency %u)",
      b.available_cpus, b.quota_millicpus, static_cast<long long>(b.quota_period_ns),
      b.parallelism_full(), b.parallelism_unthrottled(),
      std::thread::hardware_concurrency());
  CHECK(b.available_cpus >= 1);
  CHECK(b.parallelism_full() >= 1);
  CHECK((b.quota_millicpus == 0) == (b.quota_period_ns == 0 || OSPRIM_OS_WINDOWS));

#if OSPRIM_OS_LINUX || OSPRIM_OS_ANDROID
  cpu_set_t set;
  CPU_ZERO(&set);
  REQUIRE(sched_getaffinity(0, sizeof(set), &set) == 0);
  CHECK(b.available_cpus == static_cast<uint32_t>(CPU_COUNT(&set)));
#endif

  // CI jobs that restrict the process say what to expect
  const long long cpus  = env_number("OSPRIM_TEST_EXPECT_CPUS");
  const long long quota = env_number("OSPRIM_TEST_EXPECT_QUOTA");
  if (cpus >= 0)
    CHECK(b.available_cpus == static_cast<uint32_t>(cpus));
  if (quota >= 0)
    CHECK(b.quota_millicpus == static_cast<uint32_t>(quota));
}

TEST_CASE("topology: this machine")
{
  session s;
  topology_summary t{};
  size_t count = 0;
  REQUIRE(topology_query(&t, nullptr, 0, &count).ok());
  std::vector<cpu_desc> cpus(count);
  size_t again = 0;
  REQUIRE(topology_query(&t, cpus.data(), cpus.size(), &again).ok());
  CHECK(again == count);
  print_topology(t, count);
  check_consistent(t, cpus.data(), count);

#if OSPRIM_OS_APPLE
  CHECK(count == 0); // no CPU identities on Apple
#else
  CHECK(count == t.logical_cpus);
  // The entries agree with the budget
  cpu_budget b{};
  REQUIRE(get_cpu_budget(&b).ok());
  uint32_t available = 0;
  for (const cpu_desc& c : cpus)
    available += c.available ? 1u : 0u;
  CHECK(available == b.available_cpus);
#endif

  // The cache line the OS reports fits the compile-time constant
  for (uint32_t k = 0; k < t.class_count; ++k)
    if (t.classes[k].l1d.line_size != 0)
    {
      CAPTURE(k);
      CHECK(t.classes[k].l1d.line_size <= CACHE_LINE_SIZE);
    }

  // A smaller array: the first entries, and the full count
  if (count >= 2)
  {
    cpu_desc one{};
    size_t n = 0;
    REQUIRE(topology_query(&t, &one, 1, &n).ok());
    CHECK(n == count);
    CHECK(one.id == cpus[0].id);
  }
}

TEST_CASE("topology: affinity on this machine")
{
  session s;
  topology_summary t{};
  size_t count = 0;
  REQUIRE(topology_query(&t, nullptr, 0, &count).ok());
  std::vector<cpu_desc> cpus(count);
  REQUIRE(topology_query(&t, cpus.data(), cpus.size(), &count).ok());

#if OSPRIM_OS_APPLE
  const uint64_t any = 0;
  CHECK(
      set_current_thread_allowed_cpus(&any, 1).result
      == thread_affinity_result::THREAD_AFFINITY_UNSUPPORTED);
  CHECK(
      set_current_thread_preferred_cpus(&any, 1).result
      == thread_affinity_result::THREAD_AFFINITY_UNSUPPORTED);
#else
  uint64_t target = UINT64_MAX;
  for (const cpu_desc& c : cpus)
    if (c.available)
    {
      target = c.id;
      break;
    }
  REQUIRE(target != UINT64_MAX);

  std::atomic<int> failures{0};
  std::thread worker(
      [&]
      {
        // Hard: only that CPU
        if (!set_current_thread_allowed_cpus(&target, 1).ok())
          ++failures;
        cpu_budget b{};
        get_cpu_budget(&b);
  #if OSPRIM_OS_WINDOWS
        // New threads get the process's affinity: the budget is unchanged
        GROUP_AFFINITY ga{};
        GetThreadGroupAffinity(GetCurrentThread(), &ga);
        if ((ga.Mask & (ga.Mask - 1)) != 0)
          ++failures; // more than one CPU
        // Soft: preferred CPU sets
        if (!set_current_thread_preferred_cpus(&target, 1).ok())
          ++failures;
        if (!set_current_thread_preferred_cpus(nullptr, 0).ok())
          ++failures;
  #else
    // New threads inherit this thread's CPUs: the budget follows
    if (b.available_cpus != 1)
      ++failures;
    yield_thread();
    if (static_cast<uint64_t>(sched_getcpu()) != target)
      ++failures;
    if (set_current_thread_preferred_cpus(&target, 1).result
        != thread_affinity_result::THREAD_AFFINITY_UNSUPPORTED)
      ++failures;
  #endif
        // Unknown CPUs
        const uint64_t unknown = 100000;
        if (set_current_thread_allowed_cpus(&unknown, 1).result
            != thread_affinity_result::THREAD_AFFINITY_INVALID)
          ++failures;
        // Empty: back to the default
        if (!set_current_thread_allowed_cpus(nullptr, 0).ok())
          ++failures;
        cpu_budget after{};
        get_cpu_budget(&after);
        if (after.available_cpus < b.available_cpus)
          ++failures;
      });
  worker.join();
  CHECK(failures.load() == 0);
#endif
}

/*****************************/
/* LINUX FIXTURES            */
/*****************************/

#if OSPRIM_OS_LINUX || OSPRIM_OS_ANDROID
namespace
{
  /// @brief A temporary directory holding a fake sys/ and proc/ tree.
  struct fake_root
  {
    char path[256];

    fake_root()
    {
      // TMPDIR wins when set. Otherwise: Android has no /tmp (P_tmpdir);
      // what adb shell can write to is /data/local/tmp.
      const char* dir = getenv("TMPDIR");
      if (dir == nullptr || *dir == '\0')
      {
  #if defined(__ANDROID__)
        dir = "/data/local/tmp";
  #else
        dir = P_tmpdir;
  #endif
      }
      snprintf(path, sizeof(path), "%s/osprim_topology_XXXXXX", dir);
      REQUIRE(mkdtemp(path) != nullptr);
    }

    ~fake_root()
    {
      nftw(
          path, [](const char* p, const struct stat*, int, struct FTW*)
          { return remove(p); }, 16, FTW_DEPTH | FTW_PHYS);
    }

    /// @brief Writes `content` to `path` (relative), creating directories.
    void write(const char* rel, const char* content) const
    {
      char full[512];
      snprintf(full, sizeof(full), "%s/%s", path, rel);
      for (char* p = full + strlen(path) + 1; *p != '\0'; ++p)
        if (*p == '/')
        {
          *p = '\0';
          mkdir(full, 0755);
          *p = '/';
        }
      FILE* f = fopen(full, "w");
      REQUIRE(f != nullptr);
      fputs(content, f);
      fclose(f);
    }

    void cpu(unsigned c, const char* rel, const char* content) const
    {
      char p[256];
      snprintf(p, sizeof(p), "sys/devices/system/cpu/cpu%u/%s", c, rel);
      write(p, content);
    }

    void cache(
        unsigned c, int index, int level, const char* type, const char* size,
        const char* shared) const
    {
      char p[256], v[32];
      snprintf(
          p, sizeof(p), "sys/devices/system/cpu/cpu%u/cache/index%d/level", c,
          index);
      snprintf(v, sizeof(v), "%d", level);
      write(p, v);
      snprintf(
          p, sizeof(p), "sys/devices/system/cpu/cpu%u/cache/index%d/type", c, index);
      write(p, type);
      snprintf(
          p, sizeof(p), "sys/devices/system/cpu/cpu%u/cache/index%d/size", c, index);
      write(p, size);
      snprintf(
          p, sizeof(p),
          "sys/devices/system/cpu/cpu%u/cache/index%d/coherency_line_size", c,
          index);
      write(p, "64");
      snprintf(
          p, sizeof(p), "sys/devices/system/cpu/cpu%u/cache/index%d/shared_cpu_list",
          c, index);
      write(p, shared);
    }
  };

  detail::cpu_bits all_cpus()
  {
    detail::cpu_bits b{};
    for (size_t c = 0; c < TOPOLOGY_MAX_CPUS; ++c)
      b.set(c);
    return b;
  }

  struct fixture_result
  {
    topology_summary t{};
    std::vector<cpu_desc> cpus;
  };

  fixture_result query(
      const fake_root& root, const detail::cpu_bits& allowed = all_cpus())
  {
    fixture_result r;
    size_t count = 0;
    REQUIRE(
        detail::linux_topology(root.path, allowed, &r.t, nullptr, 0, &count).ok());
    r.cpus.resize(count);
    REQUIRE(
        detail::linux_topology(
            root.path, allowed, &r.t, r.cpus.data(), count, &count)
            .ok());
    check_consistent(r.t, r.cpus.data(), count);
    return r;
  }

  char* num(char (&buf)[32], unsigned long long v)
  {
    snprintf(buf, sizeof(buf), "%llu", v);
    return buf;
  }
} // namespace

TEST_CASE("topology fixture: Intel hybrid, wrong cpu_capacity")
{
  // 8 P-cores with hyper-threading (CPUs 0-15), 8 E-cores (16-23). Every
  // cpu_capacity says 1024: the cpu_core/cpu_atom lists must decide.
  fake_root root;
  root.write("sys/devices/system/cpu/online", "0-23\n");
  root.write("sys/devices/cpu_core/cpus", "0-15\n");
  root.write("sys/devices/cpu_atom/cpus", "16-23\n");
  for (unsigned c = 0; c < 24; ++c)
  {
    char siblings[32];
    if (c < 16)
      snprintf(siblings, sizeof(siblings), "%u-%u\n", c & ~1u, c | 1u);
    else
      snprintf(siblings, sizeof(siblings), "%u\n", c);
    root.cpu(c, "topology/thread_siblings_list", siblings);
    root.cpu(c, "topology/physical_package_id", "0\n");
    root.cpu(c, "cpu_capacity", "1024\n");
    if (c < 16)
    {
      root.cache(c, 0, 1, "Data\n", "48K\n", siblings);
      root.cache(c, 1, 1, "Instruction\n", "32K\n", siblings);
      root.cache(c, 2, 2, "Unified\n", "1280K\n", siblings);
    }
    else
    {
      char cluster[32];
      snprintf(cluster, sizeof(cluster), "%u-%u\n", c & ~3u, c | 3u);
      root.cache(c, 0, 1, "Data\n", "32K\n", siblings);
      root.cache(c, 1, 1, "Instruction\n", "64K\n", siblings);
      root.cache(c, 2, 2, "Unified\n", "2048K\n", cluster);
    }
    root.cache(c, 3, 3, "Unified\n", "30720K\n", "0-23\n");
  }

  const fixture_result r = query(root);
  CHECK(r.t.class_source == core_class_source::CORE_CLASS_SOURCE_REPORTED);
  REQUIRE(r.t.class_count == 2);
  CHECK(r.t.logical_cpus == 24);
  CHECK(r.t.physical_cores == 16);
  CHECK(r.t.packages == 1);
  // Class 0: the P-cores
  CHECK(r.t.classes[0].logical_cpus == 16);
  CHECK(r.t.classes[0].physical_cores == 8);
  CHECK(r.t.classes[0].l1d.size == 48 * 1024);
  CHECK(r.t.classes[0].l1d.shared_by == 2);
  CHECK(r.t.classes[0].l2.size == 1280 * 1024);
  CHECK(r.t.classes[0].l3.size == 30720 * 1024);
  CHECK(r.t.classes[0].l3.shared_by == 24);
  // Class 1: the E-cores, an L2 per cluster of 4
  CHECK(r.t.classes[1].logical_cpus == 8);
  CHECK(r.t.classes[1].physical_cores == 8);
  CHECK(r.t.classes[1].l1d.size == 32 * 1024);
  CHECK(r.t.classes[1].l2.size == 2048 * 1024);
  CHECK(r.t.classes[1].l2.shared_by == 4);
  // Per CPU
  REQUIRE(r.cpus.size() == 24);
  CHECK(r.cpus[0].core == r.cpus[1].core); // hyper-threads
  CHECK(r.cpus[1].core != r.cpus[2].core);
  CHECK(r.cpus[0].core_class == 0);
  CHECK(r.cpus[16].core_class == 1);
  CHECK(r.cpus[0].last_level_cache == r.cpus[23].last_level_cache); // one L3
}

TEST_CASE("topology fixture: Arm phone with three tiers")
{
  // 4 little (capacity 400), 3 big (900), 1 prime (1024). core_id restarts
  // in every cluster, as on real Arm systems: it must not be used.
  fake_root root;
  root.write("sys/devices/system/cpu/online", "0-7\n");
  const unsigned capacity[8] = {400, 400, 400, 400, 900, 900, 900, 1024};
  const unsigned core_id[8]  = {0, 1, 2, 3, 0, 1, 2, 0};
  for (unsigned c = 0; c < 8; ++c)
  {
    char b[32];
    root.cpu(c, "cpu_capacity", num(b, capacity[c]));
    root.cpu(c, "topology/core_id", num(b, core_id[c]));
    root.cpu(c, "topology/physical_package_id", "-1\n");
    root.cpu(c, "topology/thread_siblings_list", num(b, c));
  }
  const fixture_result r = query(root);
  CHECK(r.t.class_source == core_class_source::CORE_CLASS_SOURCE_REPORTED);
  REQUIRE(r.t.class_count == 3);
  CHECK(r.t.classes[0].logical_cpus == 1); // prime
  CHECK(r.t.classes[1].logical_cpus == 3); // big
  CHECK(r.t.classes[2].logical_cpus == 4); // little
  CHECK(r.t.physical_cores == 8);          // not merged by core_id
  CHECK(r.t.packages == 1);
  CHECK(r.cpus[7].core_class == 0);
  CHECK(r.cpus[0].core_class == 2);
}

TEST_CASE("topology fixture: classes inferred from frequencies")
{
  fake_root root;
  root.write("sys/devices/system/cpu/online", "0-7\n");
  for (unsigned c = 0; c < 8; ++c)
    root.cpu(c, "cpufreq/cpuinfo_max_freq", c < 4 ? "3000000\n" : "2000000\n");
  const fixture_result r = query(root);
  CHECK(r.t.class_source == core_class_source::CORE_CLASS_SOURCE_INFERRED);
  REQUIRE(r.t.class_count == 2);
  CHECK(r.t.classes[0].logical_cpus == 4);
  CHECK(r.cpus[0].core_class == 0);
  CHECK(r.cpus[7].core_class == 1);
}

TEST_CASE("topology fixture: favored cores are not a class")
{
  // Turbo Boost Max: two cores reach 5.0 GHz, the others 4.8 (4% apart)
  fake_root root;
  root.write("sys/devices/system/cpu/online", "0-7\n");
  for (unsigned c = 0; c < 8; ++c)
    root.cpu(c, "cpufreq/cpuinfo_max_freq", c < 2 ? "5000000\n" : "4800000\n");
  const fixture_result r = query(root);
  CHECK(r.t.class_source == core_class_source::CORE_CLASS_SOURCE_UNIFORM);
  CHECK(r.t.class_count == 1);
  CHECK(r.t.classes[0].logical_cpus == 8);
}

TEST_CASE("topology fixture: more than four classes merge into the last")
{
  fake_root root;
  root.write("sys/devices/system/cpu/online", "0-5\n");
  for (unsigned c = 0; c < 6; ++c)
  {
    char b[32];
    root.cpu(c, "cpu_capacity", num(b, 1024 - c * 100));
  }
  const fixture_result r = query(root);
  REQUIRE(r.t.class_count == MAX_CORE_CLASSES);
  CHECK(r.t.classes[0].logical_cpus == 1);
  CHECK(r.t.classes[3].logical_cpus == 3); // the three slowest
  CHECK(r.cpus[5].core_class == 3);
}

TEST_CASE("topology fixture: offline CPUs, NUMA nodes and availability")
{
  fake_root root;
  root.write("sys/devices/system/cpu/online", "0-2,4\n");
  root.write("sys/devices/system/node/online", "0-1\n");
  root.write("sys/devices/system/node/node0/cpulist", "0-1\n");
  root.write("sys/devices/system/node/node1/cpulist", "2-4\n");
  detail::cpu_bits allowed{};
  allowed.set(1);
  allowed.set(4);
  const fixture_result r = query(root, allowed);
  REQUIRE(r.cpus.size() == 4);
  CHECK(r.cpus[3].id == 4); // CPU 3 is offline
  CHECK(r.cpus[0].numa_node == 0);
  CHECK(r.cpus[1].numa_node == 0);
  CHECK(r.cpus[2].numa_node == 1);
  CHECK(r.cpus[3].numa_node == 1);
  CHECK(!r.cpus[0].available);
  CHECK(r.cpus[1].available);
  CHECK(!r.cpus[2].available);
  CHECK(r.cpus[3].available);
}

TEST_CASE("topology fixture: cgroup quotas")
{
  uint32_t milli = 0;
  int64_t period = 0;

  SUBCASE("v2, nested: the tightest level wins")
  {
    fake_root root;
    root.write("proc/self/cgroup", "0::/a/b\n");
    root.write("sys/fs/cgroup/cpu.max", "max 100000\n");
    root.write("sys/fs/cgroup/a/cpu.max", "200000 100000\n");
    root.write("sys/fs/cgroup/a/b/cpu.max", "150000 100000\n");
    detail::linux_cgroup_quota(root.path, &milli, &period);
    CHECK(milli == 1500);
    CHECK(period == 100'000'000);
  }
  SUBCASE("v2, a parent tighter than the child")
  {
    fake_root root;
    root.write("proc/self/cgroup", "0::/a/b\n");
    root.write("sys/fs/cgroup/a/cpu.max", "50000 100000\n");
    root.write("sys/fs/cgroup/a/b/cpu.max", "400000 200000\n");
    detail::linux_cgroup_quota(root.path, &milli, &period);
    CHECK(milli == 500);
    CHECK(period == 100'000'000);
  }
  SUBCASE("v2, in a cgroup namespace (Docker)")
  {
    fake_root root;
    root.write("proc/self/cgroup", "0::/\n");
    root.write("sys/fs/cgroup/cpu.max", "150000 100000\n");
    detail::linux_cgroup_quota(root.path, &milli, &period);
    CHECK(milli == 1500);
  }
  SUBCASE("v2, a path that does not exist below the mount")
  {
    fake_root root;
    root.write("proc/self/cgroup", "0::/host/side/path\n");
    root.write("sys/fs/cgroup/cpu.max", "250000 100000\n");
    detail::linux_cgroup_quota(root.path, &milli, &period);
    CHECK(milli == 2500);
  }
  SUBCASE("v2, no limit")
  {
    fake_root root;
    root.write("proc/self/cgroup", "0::/user.slice\n");
    root.write("sys/fs/cgroup/cpu.max", "max 100000\n");
    root.write("sys/fs/cgroup/user.slice/cpu.max", "max 100000\n");
    detail::linux_cgroup_quota(root.path, &milli, &period);
    CHECK(milli == 0);
    CHECK(period == 0);
  }
  SUBCASE("v1")
  {
    fake_root root;
    root.write("proc/self/cgroup", "12:memory:/docker/x\n4:cpu,cpuacct:/docker/x\n");
    root.write("sys/fs/cgroup/cpu,cpuacct/cpu.cfs_quota_us", "-1\n");
    root.write("sys/fs/cgroup/cpu,cpuacct/cpu.cfs_period_us", "100000\n");
    root.write("sys/fs/cgroup/cpu,cpuacct/docker/x/cpu.cfs_quota_us", "150000\n");
    root.write("sys/fs/cgroup/cpu,cpuacct/docker/x/cpu.cfs_period_us", "100000\n");
    detail::linux_cgroup_quota(root.path, &milli, &period);
    CHECK(milli == 1500);
    CHECK(period == 100'000'000);
  }
  SUBCASE("no cgroup file")
  {
    fake_root root;
    detail::linux_cgroup_quota(root.path, &milli, &period);
    CHECK(milli == 0);
  }
}
#endif // Linux, Android

/*****************************/
/* WINDOWS FIXTURES          */
/*****************************/

#if OSPRIM_OS_WINDOWS
namespace
{
  /// @brief Appends one SYSTEM_CPU_SET_INFORMATION entry (32 bytes).
  void add_cpu_set(
      std::vector<unsigned char>& buf, uint32_t id, uint16_t group, uint8_t lp,
      uint8_t core, uint8_t llc, uint8_t efficiency, uint8_t flags,
      uint32_t type = 0)
  {
    unsigned char e[32] = {};
    const uint32_t size = 32;
    memcpy(e + 0, &size, 4);
    memcpy(e + 4, &type, 4);
    memcpy(e + 8, &id, 4);
    memcpy(e + 12, &group, 2);
    e[14] = lp;
    e[15] = core;
    e[16] = llc;
    e[17] = 0;
    e[18] = efficiency;
    e[19] = flags;
    buf.insert(buf.end(), e, e + 32);
  }
} // namespace

TEST_CASE("topology fixture: Windows CPU sets")
{
  std::vector<unsigned char> buf;
  // Group 0: 2 P-cores with hyper-threading (efficiency 1), 2 E-cores (0)
  add_cpu_set(buf, 0x100, 0, 0, 0, 0, 1, 0);
  add_cpu_set(buf, 0x101, 0, 1, 0, 0, 1, 0);
  add_cpu_set(buf, 0x102, 0, 2, 1, 0, 1, 0x2);       // reserved for another process
  add_cpu_set(buf, 0x103, 0, 3, 1, 0, 1, 0x2 | 0x4); // reserved for this process
  add_cpu_set(buf, 0x104, 0, 4, 2, 1, 0, 0);
  add_cpu_set(buf, 0x105, 0, 5, 3, 1, 0, 0);
  add_cpu_set(buf, 0x999, 0, 6, 4, 1, 0, 0, 1); // unknown type: skipped
  add_cpu_set(buf, 0x200, 1, 0, 0, 0, 0, 0);    // group 1: not allowed

  const uint16_t groups[1] = {0};
  detail::cpu_record r[16];
  // Primary mask without LP 5
  const size_t n = detail::windows_parse_cpu_sets(
      buf.data(), buf.size(), groups, 1, 0, 0x1F, r, 16);
  REQUIRE(n == 7);
  CHECK(r[0].id == 0x100);
  CHECK(r[0].rank == 1);
  CHECK(r[4].rank == 0);
  CHECK(r[0].core_key == r[1].core_key);
  CHECK(r[1].core_key != r[2].core_key);
  CHECK(r[0].available);
  CHECK(!r[2].available); // reserved for another process
  CHECK(r[3].available);  // reserved for this one
  CHECK(r[4].available);
  CHECK(!r[5].available); // outside the primary-group mask
  CHECK(!r[6].available); // group not allowed
  CHECK(r[6].location == ((uint64_t{1} << 8) | 0));

  // Through the shared code: classes and dense indices
  topology_summary t{};
  cpu_desc cpus[16];
  size_t first[MAX_CORE_CLASSES];
  detail::finish_topology(
      r, detail::g_topology_scratch.classes, n,
      core_class_source::CORE_CLASS_SOURCE_REPORTED, &t, cpus, 16, first);
  CHECK(t.class_count == 2);
  CHECK(t.classes[0].logical_cpus == 4);
  CHECK(t.classes[0].physical_cores == 2);
  CHECK(t.classes[1].logical_cpus == 3);
}

TEST_CASE("topology fixture: Windows CPU sets over several processor groups")
{
  std::vector<unsigned char> buf;
  for (uint32_t i = 0; i < 160; ++i)
    add_cpu_set(
        buf, 0x100 + i, static_cast<uint16_t>(i / 64), static_cast<uint8_t>(i % 64),
        static_cast<uint8_t>((i % 64) / 2), 0, 0, 0);
  const uint16_t groups[3] = {0, 1, 2};
  static detail::cpu_record r[TOPOLOGY_MAX_CPUS];
  // Spanning all groups: the primary mask is 0
  const size_t n = detail::windows_parse_cpu_sets(
      buf.data(), buf.size(), groups, 3, 0, 0, r, TOPOLOGY_MAX_CPUS);
  REQUIRE(n == 160);
  size_t available = 0;
  for (size_t i = 0; i < n; ++i)
    available += r[i].available ? 1 : 0;
  CHECK(available == 160);
  // Group-relative core indices do not collide across groups
  CHECK(r[0].core_key != r[64].core_key);
}
#endif // Windows

#if OSPRIM_OS_WINDOWS
TEST_CASE("topology: a job object CPU rate cap is a quota")
{
  session s;
  // Put this process in a job capped at 50% of the machine, then lift the cap
  const HANDLE job = CreateJobObjectW(nullptr, nullptr);
  REQUIRE(job != nullptr);
  if (!AssignProcessToJobObject(job, GetCurrentProcess()))
  {
    note("cannot join a job object here (error %lu): skipped", GetLastError());
    CloseHandle(job);
    return;
  }
  struct
  {
    DWORD flags;
    DWORD rate;
  } info{0x1 | 0x4, 5000}; // ENABLE | HARD_CAP, 50.00%
  if (!SetInformationJobObject(
          job, JobObjectCpuRateControlInformation, &info, sizeof(info)))
  {
    note("cannot cap the job's CPU rate here (error %lu): skipped", GetLastError());
    CloseHandle(job);
    return;
  }
  cpu_budget b{};
  const auto st = get_cpu_budget(&b);
  info          = {0, 0}; // lift the cap before checking
  SetInformationJobObject(
      job, JobObjectCpuRateControlInformation, &info, sizeof(info));
  CloseHandle(job);

  REQUIRE(st.ok());
  const uint32_t all = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
  note("job capped at 50%%: quota %u millicpus (%u CPUs)", b.quota_millicpus, all);
  CHECK(b.quota_millicpus == all * 500);
  CHECK(b.parallelism_full() == (all + 1) / 2);
}
#endif
