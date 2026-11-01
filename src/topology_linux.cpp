/*****************************************************************/ /**
 * @file   topology_linux.cpp
 * @brief  Linux and Android topology, budget and affinity.
 *
 * - Budget: the calling thread's affinity (new threads inherit it, and it
 *   already includes container and Android background cpusets), plus the
 *   tightest cgroup CPU quota along the hierarchy (v2 cpu.max, v1
 *   cpu.cfs_quota_us / cpu.cfs_period_us).
 * - Topology: sysfs (online CPUs, hyper-thread siblings, package ids,
 *   caches with their sharing lists, NUMA nodes). Classes, first that applies:
 *   1. cpu_capacity, if its values differ (Arm; some Intel kernels),
 *   2. Intel's hybrid lists /sys/devices/cpu_core/cpus and cpu_atom/cpus,
 *   3. cpuinfo_max_freq, grouping values less than 15% apart (INFERRED),
 *   4. otherwise one class.
 * - Affinity: sched_setaffinity on the calling thread (hard only).
 *
 * Everything reads below a root directory: empty for the real system, a
 * fake tree in the tests.
 *********************************************************************/
#include "internal.hpp"

#if !(OSPRIM_OS_LINUX || OSPRIM_OS_ANDROID)
  #error "topology_linux.cpp is for Linux and Android only"
#endif

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace osprim
{
  inline namespace v0_1
  {
    namespace
    {
      using detail::cpu_bits;
      using detail::cpu_record;

      constexpr size_t PATH_SIZE = 512;
      constexpr size_t TEXT_SIZE = 8192;

      /// @brief Reads a whole (small) file, NUL-terminated.
      /// @return false if unreadable or empty
      bool read_text(const char* path, char* buf, size_t size) noexcept
      {
        const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
          return false;
        size_t total = 0;
        while (total + 1 < size)
        {
          const ssize_t r = ::read(fd, buf + total, size - 1 - total);
          if (r < 0 && errno == EINTR)
            continue;
          if (r <= 0)
            break;
          total += static_cast<size_t>(r);
        }
        ::close(fd);
        buf[total] = '\0';
        return total != 0;
      }

      bool read_u64(const char* path, uint64_t* out) noexcept
      {
        char b[64];
        if (!read_text(path, b, sizeof(b)))
          return false;
        char* end                  = nullptr;
        const unsigned long long v = ::strtoull(b, &end, 10);
        if (end == b)
          return false;
        *out = static_cast<uint64_t>(v);
        return true;
      }

      bool read_i64(const char* path, int64_t* out) noexcept
      {
        char b[64];
        if (!read_text(path, b, sizeof(b)))
          return false;
        char* end         = nullptr;
        const long long v = ::strtoll(b, &end, 10);
        if (end == b)
          return false;
        *out = static_cast<int64_t>(v);
        return true;
      }

      /// @brief Parses a CPU list ("0-3,8,10-11").
      bool parse_cpulist(const char* s, cpu_bits* out) noexcept
      {
        *out     = cpu_bits{};
        bool any = false;
        while (*s != '\0')
        {
          while (*s == ' ' || *s == ',' || *s == '\n')
            ++s;
          if (*s < '0' || *s > '9')
            break;
          char* end             = nullptr;
          const unsigned long a = ::strtoul(s, &end, 10);
          unsigned long b       = a;
          s                     = end;
          if (*s == '-')
          {
            b = ::strtoul(s + 1, &end, 10);
            s = end;
          }
          for (unsigned long c = a; c <= b && c < TOPOLOGY_MAX_CPUS; ++c)
          {
            out->set(c);
            any = true;
          }
        }
        return any;
      }

      /// @brief First CPU of a CPU list, or `fallback`.
      uint64_t first_cpu(const char* s, uint64_t fallback) noexcept
      {
        cpu_bits b;
        if (!parse_cpulist(s, &b))
          return fallback;
        for (size_t c = 0; c < TOPOLOGY_MAX_CPUS; ++c)
          if (b.test(c))
            return c;
        return fallback;
      }

      /// @brief "48K", "2048K", "36M" in bytes.
      uint64_t parse_size(const char* s) noexcept
      {
        char* end  = nullptr;
        uint64_t v = ::strtoull(s, &end, 10);
        if (*end == 'K')
          v *= 1024;
        else if (*end == 'M')
          v *= 1024 * 1024;
        else if (*end == 'G')
          v *= 1024ull * 1024 * 1024;
        return v;
      }

      /// @brief Key of the highest cache level of `cpu`: its first sharing CPU.
      uint64_t last_level_cache_key(
          const char* root, uint64_t cpu, uint64_t fallback) noexcept
      {
        char p[PATH_SIZE];
        char text[TEXT_SIZE];
        uint64_t best_level = 0;
        uint64_t key        = fallback;
        for (int index = 0; index < 16; ++index)
        {
          uint64_t level = 0;
          ::snprintf(
              p, sizeof(p), "%s/sys/devices/system/cpu/cpu%llu/cache/index%d/level",
              root, static_cast<unsigned long long>(cpu), index);
          if (!read_u64(p, &level))
            break;
          ::snprintf(
              p, sizeof(p), "%s/sys/devices/system/cpu/cpu%llu/cache/index%d/type",
              root, static_cast<unsigned long long>(cpu), index);
          if (!read_text(p, text, sizeof(text))
              || ::strncmp(text, "Instruction", 11) == 0)
            continue;
          if (level <= best_level)
            continue;
          ::snprintf(
              p, sizeof(p),
              "%s/sys/devices/system/cpu/cpu%llu/cache/index%d/shared_cpu_list",
              root, static_cast<unsigned long long>(cpu), index);
          if (!read_text(p, text, sizeof(text)))
            continue;
          best_level = level;
          key        = first_cpu(text, fallback);
        }
        return key;
      }

      /// @brief The caches of `cpu`, for its class.
      void read_caches(const char* root, uint64_t cpu, core_class_desc* out) noexcept
      {
        char p[PATH_SIZE];
        char text[TEXT_SIZE];
        for (int index = 0; index < 16; ++index)
        {
          uint64_t level = 0;
          ::snprintf(
              p, sizeof(p), "%s/sys/devices/system/cpu/cpu%llu/cache/index%d/level",
              root, static_cast<unsigned long long>(cpu), index);
          if (!read_u64(p, &level))
            break;
          ::snprintf(
              p, sizeof(p), "%s/sys/devices/system/cpu/cpu%llu/cache/index%d/type",
              root, static_cast<unsigned long long>(cpu), index);
          if (!read_text(p, text, sizeof(text))
              || ::strncmp(text, "Instruction", 11) == 0)
            continue;

          cache_desc c{};
          ::snprintf(
              p, sizeof(p), "%s/sys/devices/system/cpu/cpu%llu/cache/index%d/size",
              root, static_cast<unsigned long long>(cpu), index);
          if (read_text(p, text, sizeof(text)))
          {
            const uint64_t size = parse_size(text);
            c.size = size > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(size);
          }
          uint64_t line = 0;
          ::snprintf(
              p, sizeof(p),
              "%s/sys/devices/system/cpu/cpu%llu/cache/index%d/coherency_line_size",
              root, static_cast<unsigned long long>(cpu), index);
          if (read_u64(p, &line))
            c.line_size =
                static_cast<uint16_t>(line > UINT16_MAX ? UINT16_MAX : line);
          ::snprintf(
              p, sizeof(p),
              "%s/sys/devices/system/cpu/cpu%llu/cache/index%d/shared_cpu_list",
              root, static_cast<unsigned long long>(cpu), index);
          cpu_bits shared;
          if (read_text(p, text, sizeof(text)) && parse_cpulist(text, &shared))
          {
            const size_t n = shared.count();
            c.shared_by    = static_cast<uint16_t>(n > UINT16_MAX ? UINT16_MAX : n);
          }

          if (level == 1 && out->l1d.size == 0)
            out->l1d = c;
          else if (level == 2 && out->l2.size == 0)
            out->l2 = c;
          else if (level == 3 && out->l3.size == 0)
            out->l3 = c;
        }
      }

      /// @brief Number of distinct ranks among the records.
      size_t distinct_ranks(const cpu_record* r, size_t n) noexcept
      {
        size_t count = 0;
        for (size_t i = 0; i < n; ++i)
        {
          bool seen = false;
          for (size_t j = 0; j < i && !seen; ++j)
            seen = r[j].rank == r[i].rank;
          count += seen ? 0 : 1;
        }
        return count;
      }

      /// @brief Classes from cpu_capacity, Intel's hybrid lists, or maximum
      /// frequencies, in that order. Sets the records' ranks.
      core_class_source classify(const char* root, cpu_record* r, size_t n) noexcept
      {
        char p[PATH_SIZE];
        char text[TEXT_SIZE];

        // 1. cpu_capacity, when its values differ (all equal: maybe wrong,
        // as on some Intel hybrid kernels: look further)
        bool all = n != 0;
        for (size_t i = 0; i < n && all; ++i)
        {
          ::snprintf(
              p, sizeof(p), "%s/sys/devices/system/cpu/cpu%llu/cpu_capacity", root,
              static_cast<unsigned long long>(r[i].id));
          all = read_u64(p, &r[i].rank);
        }
        if (all && distinct_ranks(r, n) > 1)
          return core_class_source::CORE_CLASS_SOURCE_REPORTED;

        // 2. Intel hybrid: the two PMU devices list their CPUs
        for (size_t i = 0; i < n; ++i)
          r[i].rank = 0;
        cpu_bits core_cpus, atom_cpus;
        ::snprintf(p, sizeof(p), "%s/sys/devices/cpu_core/cpus", root);
        const bool has_core =
            read_text(p, text, sizeof(text)) && parse_cpulist(text, &core_cpus);
        ::snprintf(p, sizeof(p), "%s/sys/devices/cpu_atom/cpus", root);
        const bool has_atom =
            read_text(p, text, sizeof(text)) && parse_cpulist(text, &atom_cpus);
        if (has_core && has_atom)
        {
          for (size_t i = 0; i < n; ++i)
            r[i].rank =
                core_cpus.test(r[i].id) ? 2 : (atom_cpus.test(r[i].id) ? 1 : 0);
          return core_class_source::CORE_CLASS_SOURCE_REPORTED;
        }

        // 3. Maximum frequencies: a gap of more than 15% between consecutive
        // distinct values starts a new class (favored cores of a uniform CPU
        // differ by a few percent only)
        all = n != 0;
        for (size_t i = 0; i < n && all; ++i)
        {
          ::snprintf(
              p, sizeof(p),
              "%s/sys/devices/system/cpu/cpu%llu/cpufreq/cpuinfo_max_freq", root,
              static_cast<unsigned long long>(r[i].id));
          all = read_u64(p, &r[i].rank);
        }
        if (!all)
        {
          for (size_t i = 0; i < n; ++i)
            r[i].rank = 0;
          return core_class_source::CORE_CLASS_SOURCE_UNIFORM;
        }
        // Walk the distinct frequencies downwards; each one maps to the
        // highest frequency of its group
        constexpr size_t MAX_DISTINCT = 64;
        uint64_t freq[MAX_DISTINCT];
        uint64_t top[MAX_DISTINCT];
        size_t m         = 0;
        size_t groups    = 0;
        uint64_t ceiling = UINT64_MAX;
        uint64_t prev    = 0;
        uint64_t current = 0;
        for (;;)
        {
          bool found    = false;
          uint64_t best = 0;
          for (size_t i = 0; i < n; ++i)
            if (r[i].rank < ceiling && (!found || r[i].rank > best))
            {
              best  = r[i].rank;
              found = true;
            }
          if (!found)
            break;
          if (groups == 0 || best * 100 < prev * 85)
          {
            current = best;
            ++groups;
          }
          if (m < MAX_DISTINCT)
          {
            freq[m] = best;
            top[m]  = current;
            ++m;
          }
          prev    = best;
          ceiling = best;
        }
        if (groups < 2)
        {
          for (size_t i = 0; i < n; ++i)
            r[i].rank = 0;
          return core_class_source::CORE_CLASS_SOURCE_UNIFORM;
        }
        for (size_t i = 0; i < n; ++i)
        {
          uint64_t t = current; // beyond MAX_DISTINCT values: the slowest group
          for (size_t k = 0; k < m; ++k)
            if (freq[k] == r[i].rank)
            {
              t = top[k];
              break;
            }
          r[i].rank = t;
        }
        return core_class_source::CORE_CLASS_SOURCE_INFERRED;
      }

      /// @brief Restored by an empty set_current_thread_allowed_cpus().
      cpu_bits g_initial_affinity{};
      bool g_initial_affinity_valid = false;

      bool get_affinity(cpu_bits* out) noexcept
      {
        *out = cpu_bits{};
        return ::sched_getaffinity(
                   0, sizeof(out->words), reinterpret_cast<cpu_set_t*>(out->words))
               == 0;
      }
    } // namespace

    namespace detail
    {
      size_t cpu_bits::count() const noexcept
      {
        size_t n = 0;
        for (uint64_t w : words)
          n += static_cast<size_t>(__builtin_popcountll(w));
        return n;
      }

      status<topology_result> linux_topology(
          const char* root, const cpu_bits& allowed, topology_summary* out,
          cpu_desc* cpus, size_t capacity, size_t* count) noexcept
      {
        char p[PATH_SIZE];
        char text[TEXT_SIZE];

        cpu_bits online;
        ::snprintf(p, sizeof(p), "%s/sys/devices/system/cpu/online", root);
        if (!read_text(p, text, sizeof(text)) || !parse_cpulist(text, &online))
        {
          online       = cpu_bits{};
          const long n = ::sysconf(_SC_NPROCESSORS_ONLN);
          for (long c = 0;
               c < (n < 1 ? 1 : n) && c < static_cast<long>(TOPOLOGY_MAX_CPUS); ++c)
            online.set(static_cast<size_t>(c));
        }

        topology_lock lock;
        cpu_record* const r = g_topology_scratch.records;
        size_t n            = 0;
        for (size_t c = 0; c < TOPOLOGY_MAX_CPUS; ++c)
        {
          if (!online.test(c))
            continue;
          cpu_record& rec = r[n++];
          rec             = cpu_record{};
          rec.id          = c;
          // The core: its first hyper-thread. (core_id is not unique: on
          // Arm it restarts in every cluster.)
          uint64_t core = c;
          ::snprintf(
              p, sizeof(p),
              "%s/sys/devices/system/cpu/cpu%zu/topology/thread_siblings_list", root,
              c);
          if (read_text(p, text, sizeof(text)))
            core = first_cpu(text, c);
          else
          {
            ::snprintf(
                p, sizeof(p),
                "%s/sys/devices/system/cpu/cpu%zu/topology/core_cpus_list", root, c);
            if (read_text(p, text, sizeof(text)))
              core = first_cpu(text, c);
          }
          // -1 on some Arm systems: one package
          int64_t package = 0;
          ::snprintf(
              p, sizeof(p),
              "%s/sys/devices/system/cpu/cpu%zu/topology/physical_package_id", root,
              c);
          if (!read_i64(p, &package) || package < 0)
            package = 0;
          rec.core_key    = core;
          rec.package_key = static_cast<uint64_t>(package);
          rec.llc_key =
              last_level_cache_key(root, c, (uint64_t{1} << 63) | rec.package_key);
          rec.available = allowed.test(c);
        }

        // NUMA nodes (absent: one node)
        cpu_bits nodes;
        ::snprintf(p, sizeof(p), "%s/sys/devices/system/node/online", root);
        if (read_text(p, text, sizeof(text)) && parse_cpulist(text, &nodes))
        {
          for (size_t node = 0; node < TOPOLOGY_MAX_CPUS; ++node)
          {
            if (!nodes.test(node))
              continue;
            cpu_bits members;
            ::snprintf(
                p, sizeof(p), "%s/sys/devices/system/node/node%zu/cpulist", root,
                node);
            if (!read_text(p, text, sizeof(text)) || !parse_cpulist(text, &members))
              continue;
            for (size_t i = 0; i < n; ++i)
              if (members.test(r[i].id))
                r[i].node_key = node;
          }
        }

        const core_class_source source = classify(root, r, n);
        size_t first[MAX_CORE_CLASSES] = {};
        finish_topology(
            r, g_topology_scratch.classes, n, source, out, cpus, capacity, first);
        for (size_t k = 0; k < out->class_count; ++k)
          read_caches(root, r[first[k]].id, &out->classes[k]);
        *count = n;
        return {};
      }

      void linux_cgroup_quota(
          const char* root, uint32_t* millicpus, int64_t* period_ns) noexcept
      {
        *millicpus = 0;
        *period_ns = 0;
        char p[PATH_SIZE];
        char text[TEXT_SIZE];
        ::snprintf(p, sizeof(p), "%s/proc/self/cgroup", root);
        if (!read_text(p, text, sizeof(text)))
          return;

        auto consider = [&](int64_t quota_us, int64_t period_us)
        {
          if (quota_us <= 0 || period_us <= 0)
            return;
          int64_t milli = quota_us * 1000 / period_us;
          if (milli < 1)
            milli = 1;
          if (milli > static_cast<int64_t>(UINT32_MAX))
            return;
          if (*millicpus == 0 || static_cast<uint32_t>(milli) < *millicpus)
          {
            *millicpus = static_cast<uint32_t>(milli);
            *period_ns = period_us * 1000;
          }
        };

        // From the process's cgroup up to the root of the hierarchy
        auto walk = [&](const char* base, const char* path, auto read_level)
        {
          char dir[PATH_SIZE];
          ::snprintf(dir, sizeof(dir), "%s%s", base, path);
          const size_t base_len = ::strlen(base);
          for (;;)
          {
            size_t len = ::strlen(dir);
            while (len > base_len && dir[len - 1] == '/')
              dir[--len] = '\0';
            read_level(dir);
            if (len <= base_len)
              break;
            char* slash = ::strrchr(dir, '/');
            if (slash == nullptr || static_cast<size_t>(slash - dir) < base_len)
              dir[base_len] = '\0';
            else
              *slash = '\0';
          }
        };

        // Lines: "hierarchy:controllers:path"
        for (char* line = text; line != nullptr && *line != '\0';)
        {
          char* next = ::strchr(line, '\n');
          if (next != nullptr)
            *next++ = '\0';
          char* c1 = ::strchr(line, ':');
          char* c2 = c1 != nullptr ? ::strchr(c1 + 1, ':') : nullptr;
          if (c2 != nullptr)
          {
            *c1               = '\0';
            *c2               = '\0';
            const char* ctrls = c1 + 1;
            const char* path  = c2 + 1;
            if (*ctrls == '\0' && ::strcmp(line, "0") == 0)
            {
              // cgroup v2: "max 100000" or "150000 100000"
              char base[PATH_SIZE];
              ::snprintf(base, sizeof(base), "%s/sys/fs/cgroup", root);
              walk(
                  base, path,
                  [&](const char* dir)
                  {
                    char f[PATH_SIZE + 16];
                    char v[128];
                    ::snprintf(f, sizeof(f), "%s/cpu.max", dir);
                    if (!read_text(f, v, sizeof(v)) || ::strncmp(v, "max", 3) == 0)
                      return;
                    char* end           = nullptr;
                    const long long q   = ::strtoll(v, &end, 10);
                    const long long per = ::strtoll(end, nullptr, 10);
                    consider(q, per);
                  });
            }
            else
            {
              // cgroup v1: the hierarchy holding the "cpu" controller
              bool cpu = false;
              for (const char* t = ctrls; *t != '\0';)
              {
                const char* comma = ::strchr(t, ',');
                const size_t len =
                    comma != nullptr ? static_cast<size_t>(comma - t) : ::strlen(t);
                cpu |= len == 3 && ::strncmp(t, "cpu", 3) == 0;
                t += len + (comma != nullptr ? 1 : 0);
              }
              if (cpu)
              {
                const char* mounts[2] = {
                    "/sys/fs/cgroup/cpu,cpuacct", "/sys/fs/cgroup/cpu"};
                for (const char* m : mounts)
                {
                  char base[PATH_SIZE];
                  ::snprintf(base, sizeof(base), "%s%s", root, m);
                  walk(
                      base, path,
                      [&](const char* dir)
                      {
                        char f[PATH_SIZE + 32];
                        int64_t q = 0, per = 0;
                        ::snprintf(f, sizeof(f), "%s/cpu.cfs_quota_us", dir);
                        if (!read_i64(f, &q))
                          return;
                        ::snprintf(f, sizeof(f), "%s/cpu.cfs_period_us", dir);
                        if (!read_i64(f, &per))
                          return;
                        consider(q, per);
                      });
                }
              }
            }
          }
          line = next;
        }
      }

      void platform_topology_initialize() noexcept
      {
        g_initial_affinity_valid = get_affinity(&g_initial_affinity);
      }

      status<cpu_budget_result> platform_cpu_budget(cpu_budget* out) noexcept
      {
        cpu_bits allowed;
        if (!get_affinity(&allowed))
          return {
              cpu_budget_result::CPU_BUDGET_SYSTEM_ERROR,
              native_error_kind::NATIVE_ERROR_POSIX_ERRNO,
              static_cast<int32_t>(errno)};
        out->available_cpus = static_cast<uint32_t>(allowed.count());
        linux_cgroup_quota("", &out->quota_millicpus, &out->quota_period_ns);
        return {};
      }

      status<topology_result> platform_topology_query(
          topology_summary* out, cpu_desc* cpus, size_t capacity,
          size_t* count) noexcept
      {
        cpu_bits allowed;
        if (!get_affinity(&allowed))
          return {
              topology_result::TOPOLOGY_SYSTEM_ERROR,
              native_error_kind::NATIVE_ERROR_POSIX_ERRNO,
              static_cast<int32_t>(errno)};
        return linux_topology("", allowed, out, cpus, capacity, count);
      }

      status<thread_affinity_result> platform_preferred_cpus(
          const uint64_t*, size_t) noexcept
      {
        return {
            thread_affinity_result::THREAD_AFFINITY_UNSUPPORTED,
            native_error_kind::NATIVE_ERROR_NONE, 0};
      }

      status<thread_affinity_result> platform_allowed_cpus(
          const uint64_t* ids, size_t count) noexcept
      {
        cpu_bits mask{};
        if (count == 0)
        {
          if (g_initial_affinity_valid)
            mask = g_initial_affinity;
          else
            for (size_t c = 0; c < TOPOLOGY_MAX_CPUS; ++c)
              mask.set(c);
        }
        for (size_t i = 0; i < count; ++i)
        {
          if (ids[i] >= TOPOLOGY_MAX_CPUS)
            return {
                thread_affinity_result::THREAD_AFFINITY_INVALID,
                native_error_kind::NATIVE_ERROR_NONE, 0};
          mask.set(static_cast<size_t>(ids[i]));
        }
        if (::sched_setaffinity(
                0, sizeof(mask.words), reinterpret_cast<cpu_set_t*>(mask.words))
            == 0)
          return {};
        const int err = errno;
        // EINVAL: none of these CPUs exists or is allowed (cpusets)
        return {
            err == EINVAL ? thread_affinity_result::THREAD_AFFINITY_INVALID
                          : thread_affinity_result::THREAD_AFFINITY_SYSTEM_ERROR,
            native_error_kind::NATIVE_ERROR_POSIX_ERRNO, static_cast<int32_t>(err)};
      }
    } // namespace detail
  } // namespace v0_1
} // namespace osprim
