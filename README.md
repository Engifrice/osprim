# osprim
Low-level, unopinionated operating system primitives for game engines, in C++20.

osprim provides **mechanism, not policy**: the lowest-level building blocks, with the same API on every target, on top of which an engine builds its own safe and convenient layer. It never allocates, and never throws.

## Status:
[![CI](https://github.com/Engifrice/osprim/actions/workflows/CI.yml/badge.svg?branch=main&event=push)](https://github.com/Engifrice/osprim/actions/workflows/CI.yml)

| Module            | Header                | Contents                                                                                                                                                       |
| ----------------- | --------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Base              | `osprim/base.hpp`     | Library lifetime, results, native errors, diagnostics (leak and misuse reports)                                                                                |
| Clock             | `osprim/clock.hpp`    | Awake, continuous and wall clocks; raw tick counter                                                                                                            |
| Wait              | `osprim/wait.hpp`     | Wait on an address until woken or an awake deadline; wake one or all                                                                                           |
| Thread            | `osprim/thread.hpp`   | Create, join with a deadline, detach; ids, names, scheduling classes; yield, spin hint, precise sleep                                                          |
| Power             | `osprim/power.hpp`    | Timer resolution, process activity, performance sessions, thermal state                                                                                        |
| Topology          | `osprim/topology.hpp` | CPU budget, core classes and caches, thread affinity, cache line constants                                                                                     |
| File              | `osprim/file.hpp`     | Open, positioned read and write, flush, size and identity, adoption of native handles, unbuffered I/O                                                          |
| Region            | `osprim/region.hpp`   | Mapped regions of files                                                                                                                                        |
| Path              | `osprim/path.hpp`     | Directory handles and operations relative to them: entry information, create, remove, rename, symlink targets, listing, canonical paths, the current directory |
| Dynamic libraries | `osprim/dynlib.hpp`   | Load by path or from the system, symbols, release                                                                                                              |
| Platform          | `osprim/platform.hpp` | `OSPRIM_OS_*` and `OSPRIM_ARCH_*` macros                                                                                                                       |

`osprim/osprim.hpp` includes everything.

## Rules

- `osprim::initialize()` is called exactly once before any other osprim function, and `osprim::deinitialize()` exactly once after the last one. Initialization may be repeated after deinitialization. Neither is thread-safe.
- Every function is `noexcept`. The library is built without exceptions and RTTI.
- The library never allocates, except for handle tracking (debug), which goes through the allocator hooks given to `initialize()`.
- Fallible operations return `status<Result>`: the operation's own `enum class` (value 0 is success) plus the native error behind it (errno, Win32, NTSTATUS, `kern_return_t`).
- Preconditions are checked with `OSPRIM_ASSERT(expr, msg)`, `assert()` by default. Misuse that can only be detected at runtime (leaked or double-closed handles, a writable region over a hole, a file shrunk under a region) is reported through the diagnostic callback.
- Every symbol lives in a versioned inline namespace (`osprim::v0_1`): headers and a library of different versions do not link together.

## Clocks

| Clock      | Meaning                                                         | Linux, Android     | Windows                             | Apple                  |
| ---------- | --------------------------------------------------------------- | ------------------ | ----------------------------------- | ---------------------- |
| awake      | Monotonic, stops during suspend. Every osprim deadline uses it. | `CLOCK_MONOTONIC`  | `QueryUnbiasedInterruptTimePrecise` | `mach_absolute_time`   |
| continuous | Monotonic, counts suspend                                       | `CLOCK_BOOTTIME`   | `QueryInterruptTimePrecise`         | `mach_continuous_time` |
| wall       | UTC since 1970, can jump                                        | `CLOCK_REALTIME`   | `GetSystemTimePreciseAsFileTime`    | `CLOCK_REALTIME`       |
| ticks      | Cheapest counter, for measurement only                          | TSC / `CNTVCT_EL0` | TSC / `CNTVCT_EL0`                  | TSC / `CNTVCT_EL0`     |

Times and durations are `int64_t` nanoseconds (`FOREVER_NS` = `INT64_MAX` means no deadline). Each clock has its own instant type, so they cannot be mixed up.

Ticks come from the invariant TSC on x86-64 (frequency calibrated against the awake clock during `initialize()`, which takes ~10-20 ms) and from `CNTVCT_EL0` on arm64 (frequency reported by the hardware, or derived exactly from the mach timebase on Apple). Without either, ticks fall back to awake nanoseconds. `ticks_to_ns()` is a single 64x64 multiplication and shift, and is exact for decades of ticks at every common frequency. `sample_ticks()` pairs a tick reading with an awake reading, so that a profiler can map ticks onto the awake clock.

```cpp
#include <osprim/osprim.hpp>

int main()
{
    if (!osprim::initialize({}).ok())
        return 1;

    const osprim::awake_instant deadline = osprim::deadline_after(16'000'000); // 16 ms

    const uint64_t t0 = osprim::ticks_now();
    // ... work ...
    const int64_t spent_ns = osprim::ticks_to_ns(osprim::ticks_now() - t0);
    const int64_t left_ns  = deadline.remaining(osprim::awake_now());

    osprim::deinitialize();
}
```

## Wait

`wait(word, expected, deadline)` blocks while a `std::atomic<wait_word_t>` holds `expected`, until `wake_one()`/`wake_all()`, the deadline, or a spurious return. It is the futex-style primitive that locks, thread pools and run loops are built on.

- It does not loop: callers re-check their own condition after every return, which they must do anyway.
- `WAIT_TIMED_OUT` guarantees that the awake deadline has passed; it is never reported early on any platform. Timeouts can be late (Linux timer slack, Apple timer coalescing, the Windows timer resolution): reducing that is a thread or power setting.
- No memory ordering: the caller's loads and stores carry it. Words are private to the process.

| Platform       | Wait                                                                    | Wake                                       |
| -------------- | ----------------------------------------------------------------------- | ------------------------------------------ |
| Linux, Android | `FUTEX_WAIT_BITSET_PRIVATE` (absolute `CLOCK_MONOTONIC`)                | `FUTEX_WAKE_PRIVATE`                       |
| Windows        | `WaitOnAddress` (relative milliseconds, rounded up)                     | `WakeByAddressSingle` / `WakeByAddressAll` |
| Apple          | `os_sync_wait_on_address_with_deadline` (`OS_CLOCK_MACH_ABSOLUTE_TIME`) | `os_sync_wake_by_address_any` / `_all`     |

```cpp
std::atomic<osprim::wait_word_t> ready{0};

// waiter
const osprim::awake_instant deadline = osprim::deadline_after(5'000'000'000);
while (ready.load(std::memory_order_acquire) == 0)
    if (osprim::wait(ready, 0, deadline) == osprim::wait_result::WAIT_TIMED_OUT)
        break;

// waker
ready.store(1, std::memory_order_release);
osprim::wake_one(ready);
```

## Threads

A thread is described by a caller-owned `thread_context`: the library never allocates for it. The context holds the entry function, its argument, the thread's state (a wait word, so it can be waited on), its OS id and its native handle. It must stay valid until `thread_join()` returns `THREAD_JOIN_OK`, or, for a detached thread, until its state reads `THREAD_STATE_EXITED`.

```cpp
struct worker
{
    osprim::thread_context ctx;
    int index;
};

static void run(void* user) noexcept
{
    auto* w = static_cast<worker*>(user);
    osprim::set_current_thread_name("worker");
    osprim::set_current_thread_class(osprim::thread_class::THREAD_CLASS_UTILITY);
    // ...
}

worker w{};
w.ctx.entry = &run;
w.ctx.user  = &w;
if (osprim::thread_create(w.ctx, osprim::thread_desc{.stack_size = 1 << 20}).ok())
    osprim::thread_join(w.ctx, osprim::awake_instant::forever());
```

- `thread_join()` takes an awake deadline on every platform (it waits on the state word, then releases the thread), and returns `THREAD_JOIN_TIMED_OUT` with the thread still owned if the deadline passes first.
- Settings apply to the **calling thread only**: a thread configures itself, usually first thing in its entry function. New threads inherit their creator's nice value (Linux, Android) or QoS class (Apple).
- Names are at most 15 bytes of UTF-8.
- `sleep_until()` never returns early: absolute `clock_nanosleep` on `CLOCK_MONOTONIC`, `mach_wait_until`, or a high-resolution waitable timer on Windows (precise without raising the system timer resolution).
- `cpu_relax()` is the spin-loop hint.

| `thread_class` | Windows          | Apple QoS          | Linux, Android nice |
| -------------- | ---------------- | ------------------ | ------------------- |
| `BACKGROUND`   | `LOWEST`, EcoQoS | `BACKGROUND`       | 10                  |
| `UTILITY`      | `BELOW_NORMAL`   | `UTILITY`          | 5                   |
| `NORMAL`       | `NORMAL`         | `USER_INITIATED`   | 0                   |
| `INTERACTIVE`  | `ABOVE_NORMAL`   | `USER_INTERACTIVE` | -6                  |

Desktop Linux usually refuses unprivileged users any decrease of the nice value: `INTERACTIVE`, and `NORMAL` after a lower class, return `SET_THREAD_CLASS_DENIED` (Android allows them).

## Power and timing

The operating system constantly trades speed for battery and heat, and guesses what each program needs from window focus, user input and CPU use. These features tell it when its guess is wrong, or tell the program what the system is doing.

**Which feature do I need?**

| Your goal                                             | Use                                            | Scope   | Cost               | Does something on                         |
| ----------------------------------------------------- | ---------------------------------------------- | ------- | ------------------ | ----------------------------------------- |
| Wake up on time for the next frame                    | `sleep_until`                                  | thread  | none               | all                                       |
| Precise timeouts on waits (about 1 ms, not 15 ms)     | `process_timer_resolution_begin` / `_end`      | process | battery while held | Windows (`OK` elsewhere: already precise) |
| Keep unimportant work off the fast cores              | `set_current_thread_class(BACKGROUND)`         | thread  | none               | all                                       |
| The user is here, though the OS sees no input         | `process_activity_begin` / `_end`              | process | battery while held | Windows, macOS                            |
| Meet a frame deadline without wasting power on mobile | `perf_session_create`, `_report`               | threads | none               | Android 13+                               |
| Lower quality before the device overheats             | `current_thermal_level`, `thermal_watch_begin` | process | none               | Android 11+, Apple                        |

- **One feature per goal:** osprim picks each platform's mechanism.
- **Results tell whether the goal is met:** `OK` means it is in effect (whether osprim acted or the platform already behaves that way); `UNSUPPORTED` means this platform cannot provide it and nothing changed.
- **The scope is in the name:** `process_` affects the whole program, `current_thread_` only the caller.
- **Nothing happens unless asked:** `initialize()` changes no power setting. Requests are handles, tracked like every other handle. `get_power_features()` tells once which goals the device supports.

| Feature                                | Windows                                                                              | Apple                                                                  | Linux                         | Android                                |
| -------------------------------------- | ------------------------------------------------------------------------------------ | ---------------------------------------------------------------------- | ----------------------------- | -------------------------------------- |
| Timer resolution                       | `timeBeginPeriod` (ignored while minimized on Windows 11, unless `latency_critical`) | already precise                                                        | already precise (50 us slack) | already precise                        |
| `latency_critical`                     | process power throttling off (high QoS, timer resolution honored)                    | `NSProcessInfo` activity (no App Nap, no timer coalescing), macOS only | -                             | -                                      |
| `keep_system_awake`, `keep_display_on` | power request                                                                        | `NSProcessInfo` activity, macOS only                                   | -                             | - (window flag, engine UI layer)       |
| Performance session                    | -                                                                                    | -                                                                      | -                             | `APerformanceHint` (API 33)            |
| Thermal level, watch                   | -                                                                                    | `NSProcessInfo` thermal state                                          | -                             | `AThermal` (API 30)                    |
| Thermal headroom                       | -                                                                                    | -                                                                      | -                             | `AThermal_getThermalHeadroom` (API 31) |

osprim only uses native C interfaces (including libandroid, and Foundation's process APIs through the Objective-C runtime).

## Topology

| Your goal                                 | Use                                                                          |
| ----------------------------------------- | ---------------------------------------------------------------------------- |
| How many worker threads to create         | `get_cpu_budget()`, then `parallelism_full()` or `parallelism_unthrottled()` |
| Which cores are fast, which are efficient | `topology_query()`                                                           |
| Cache sizes, and which CPUs share them    | `topology_query()`                                                           |
| Padding against false sharing             | `FALSE_SHARING_SIZE`                                                         |
| Keep a thread on fast or efficient cores  | `set_current_thread_class()` first; then preferred, then allowed CPUs        |

**The budget** says what a thread created now by the caller may use, which is often less than the machine has:
- *Where it may run:* affinity (`taskset`, `start /affinity`), container cpusets, Android moving background apps to a few efficient cores, Windows CPU sets reserved for other processes.
- *How much CPU time:* container quotas (`--cpus=1.5`: 150 ms per 100 ms) and Windows job rate caps. Exceeding a quota freezes the whole process until the next period, so a pool sized to the machine stalls.

New threads inherit their creator's CPUs on Linux and Android, and the process's on Windows: the budget follows that rule, so call it from the thread that creates the pool. It is cheap, and it changes at run time with no OS notification: query it every few seconds and on lifecycle events (Android resume).

| `cpu_budget`      | Linux, Android                                                     | Windows                                              | Apple          |
| ----------------- | ------------------------------------------------------------------ | ---------------------------------------------------- | -------------- |
| `available_cpus`  | calling thread's affinity                                          | process affinity, minus CPU sets reserved for others | `hw.activecpu` |
| `quota_millicpus` | cgroup v2 `cpu.max` / v1 `cfs_quota`, tightest along the hierarchy | job object CPU rate cap                              | none           |

`parallelism_full()` rounds the quota up (uses all of it, may be throttled at full load, as Go and .NET do); `parallelism_unthrottled()` rounds it down (at least 1).

**The topology** is a snapshot: costly, never taken by `initialize()`, cached by the caller. No allocation: the caller passes the array (a first call with none returns the count). Core classes are ordered fastest first, and `class_source` says how far to trust them:

|                | Classes from                                                                                                                                        | CPU entries                           |
| -------------- | --------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------- |
| Windows        | CPU sets `EfficiencyClass` (`REPORTED`)                                                                                                             | yes (CPU set ids)                     |
| Linux, Android | `cpu_capacity` if its values differ, else Intel's `cpu_core`/`cpu_atom` lists (`REPORTED`), else maximum frequencies 15% apart or more (`INFERRED`) | yes (CPU numbers)                     |
| Apple          | performance levels, `hw.perflevelN` (`REPORTED`)                                                                                                    | none: Apple exposes no CPU identities |

**Affinity** overrides the OS scheduler and its hardware hints (Intel Thread Director, Android's placement). Intel and Google both advise against it: prefer thread classes, then soft preferences, and restrict hard only when measured to help, once, at thread start.

|                | `set_current_thread_preferred_cpus` (soft) | `set_current_thread_allowed_cpus` (hard) |
| -------------- | ------------------------------------------ | ---------------------------------------- |
| Windows        | CPU sets                                   | group affinity (one processor group)     |
| Linux, Android | `UNSUPPORTED`                              | `sched_setaffinity`                      |
| Apple          | `UNSUPPORTED`                              | `UNSUPPORTED`                            |

|               | `CACHE_LINE_SIZE` | `FALSE_SHARING_SIZE`                           |
| ------------- | ----------------- | ---------------------------------------------- |
| x86-64        | 64                | 128 (the L2 prefetcher fetches pairs of lines) |
| arm64, Apple  | 128               | 128                                            |
| arm64, others | 64                | 64                                             |

## Files and regions

| Your goal                                                            | Use                                                          |
| -------------------------------------------------------------------- | ------------------------------------------------------------ |
| Open or create a file                                                | `file_open()` with a `file_disposition`                      |
| Read or write at an offset                                           | `file_read_at()`, `file_write_at()`                          |
| Make written data survive a power loss                               | `file_flush()`                                               |
| Size, last write time, "is it the same file?"                        | `file_info_of()`                                             |
| Grow or shrink a file                                                | `file_set_size()`: `FAST`, or `RESERVE` to reserve the space |
| Use a descriptor from another API (Android assets, document pickers) | `file_adopt()`                                               |
| Give a handle to an API that will close it                           | `file_release()`                                             |
| Stream large data without the OS cache                               | `file_open_desc::unbuffered`, `file_unbuffered_alignment()`  |
| Random access into large read-only data                              | `region_map()`                                               |

A `file_handle` is the native handle: no state outside handle tracking, free to copy. There is no file position: every read and write names its offset, so any number of threads may use one handle at once (on Windows too: handles are overlapped, and the library waits internally). One call is one system call: a read is short only at the end of the file, a write only when space runs out; "read exactly" and "write everything" loops belong to the layer above.

**Paths** are UTF-8 (validated everywhere), absolute, given as pointer and size, and at most `MAX_PATH_BYTES` (4095; 1023 on Apple). Content meant to ship everywhere stays under `PORTABLE_MAX_PATH_BYTES` (1023). On Windows they are normalized (`GetFullPathNameW`) and prefixed with `\\?\`, so the 260-character limit never applies; device paths (`\\.\`, reserved names such as `NUL`) are refused. Names reserved by Windows, the characters `< > : " | ? *` and case differences are not checked: names that work on one platform can fail or collide on another. Only regular files open.

|                          | Linux, Android                            | Windows                                 | Apple                               |
| ------------------------ | ----------------------------------------- | --------------------------------------- | ----------------------------------- |
| `FILE_IO_MAX` (one call) | 2,147,479,552                             | 4 GiB - 1                               | 2 GiB - 1                           |
| `file_flush()`           | `fdatasync`                               | `FlushFileBuffers`                      | `F_FULLFSYNC` (else `fsync`)        |
| `RESERVE` growth         | `fallocate`                               | allocation info (NTFS always allocates) | `F_PREALLOCATE`                     |
| Unbuffered               | `O_DIRECT`, alignment from `statx` (6.1+) | `FILE_FLAG_NO_BUFFERING`, sector size   | `F_NOCACHE`, held to the block size |

**Write failures.** When background write-back fails, Linux marks the lost pages clean and drops them; other systems are no better. So: keep the same handle open from the first write to `file_flush()` (a handle opened later may never see the error); after a failed flush, do not retry it and do not read the data back to check it (the cache may hold what the disk does not), write a complete new copy instead; expect errors at the write, at the flush, or both.


**Regions** map a range of a file as memory. Any offset works, the range must lie inside the file, and the file handle may be closed right after mapping. Every failure becomes a crash, because a memory access has no error return: a device error, a file truncated by another program, or a full disk while writing into unreserved space (grow with `RESERVE` first). Map large read-only data on local storage; read everything else, and write saves with `file_write_at()`. Durable writes through a region take two steps: `region_flush()`, then `file_flush()` on a handle to the file.

## Paths and directories

| Your goal                                      | Use                                                  |
| ---------------------------------------------- | ---------------------------------------------------- |
| Work inside a directory without races          | `dir_open_at()`, then the `*_at` calls               |
| Open a file in a directory                     | `file_open_at()`                                     |
| What is this entry (type, size, identity)?     | `path_info_at()`                                     |
| Create a directory                             | `dir_create_at()`                                    |
| Remove a file, a symlink or an empty directory | `path_remove_at()`                                   |
| Replace a file atomically (saves)              | `path_rename_at()` with `REPLACE` and `durable`      |
| Rename without overwriting                     | `path_rename_at()` with `NO_REPLACE`                 |
| List a directory                               | `dir_list_begin()`, `dir_list_next()`                |
| Where a symlink points                         | `path_read_link_at()`                                |
| The real path of an open file or directory     | `path_canonical()`                                   |
| The current directory                          | `current_directory_get()`, `current_directory_set()` |

**Every operation is relative to a directory handle** (or takes an absolute path with `NO_DIRECTORY`). A path is a name resolved again on every call, and what it names can change in between (editors, version control, sync clients, users, attackers: time-of-check to time-of-use races). So:
- **Do not check, then act**: act and read the result. Creating, opening and renaming without replacing are atomic, and `ALREADY_EXISTS` or `NOT_FOUND` are results, not prior checks.
- **Do not resolve a path twice**: open the directory once and name its entries relative to the handle. The handle stays on that directory even when it or its parents are renamed.
- **Walking a tree** (removal, copy): open each directory with `PATH_RESOLVE_NO_SYMLINKS` and never recurse into a `SYMLINK` entry. This is the bug behind Rust's CVE-2022-21658 in `remove_dir_all`.
- **Untrusted names** (mods, archives, user content): open the root normally, then use `PATH_RESOLVE_NO_SYMLINKS` for every name inside it. A relative name cannot climb above its handle (`..` is refused), so symlinks are the only way out; mount points are crossed in every mode. Ordinary absolute paths contain symlinks (`/var` on Apple, `/sdcard` on Android), so `NO_SYMLINKS` is meant for names relative to a trusted handle.

|                          | Linux, Android                                                                                                     | Apple                        | Windows                                                                                                                                                                 |
| ------------------------ | ------------------------------------------------------------------------------------------------------------------ | ---------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Relative operations      | `openat` family                                                                                                    | `openat` family              | `NtCreateFile` with a root directory; rename and delete by handle                                                                                                       |
| `NO_SYMLINKS`            | `openat2` (5.6+), else one component at a time (always on Android, whose app seccomp filter may kill on `openat2`) | `O_NOFOLLOW_ANY`             | One component at a time; name surrogates refused, other reparse points (OneDrive placeholders, deduplicated, compressed files) reopened and checked to be the same file |
| Rename without replacing | `renameat2` (Android 11+), else link then unlink (files)                                                           | `renameatx_np` `RENAME_EXCL` | Native                                                                                                                                                                  |
| Durable rename           | `fsync` of both directories                                                                                        | `F_FULLFSYNC`                | Flush of the target directory                                                                                                                                           |
| Removal                  | `unlinkat`                                                                                                         | `unlinkat`                   | POSIX semantics (the name disappears at once, even while open), read-only attribute ignored                                                                             |
| Listing                  | `getdents64`                                                                                                       | `getattrlistbulk`            | `GetFileInformationByHandleEx`, at most 64 KiB per call                                                                                                                 |

**Names relative to a handle**: components separated by `/` (and `\` on Windows), `.` skipped, `..` refused. On Windows, names Win32 cannot handle are refused (`NUL`, `COM1.txt`, a trailing dot or space, `< > : " | ? *`): a relative NT open would otherwise create entries most tools cannot open or delete.

**Symlinks** are followed (`NORMAL`), detected (`SYMLINK` entries) or refused (`NO_SYMLINKS`), and their targets can be read; the library does not create them. On Windows, `SYMLINK` means a name surrogate: symbolic links, WSL symlinks and junctions; volume mount points are directories, and cloud placeholders are ordinary files (no download is ever triggered). Removing or renaming a link acts on the link; a directory link is removed with `PATH_KIND_FILE` on every platform.

**Listing** uses a buffer the caller owns (`dir_reader`), at least `DIR_LIST_MIN_BUFFER` (one entry with the longest name: 792 bytes on Linux, 808 on Apple, 616 on Windows). Locally any size from 4 KiB lists at the same speed; on network shares and FUSE each call can be a round trip, so `DIR_LIST_RECOMMENDED_BUFFER` is 32 KiB (64 KiB on Windows, the most Windows 8.1 and earlier accept on network shares). Listings are unordered and not snapshots; on Linux, names are raw bytes and may not be UTF-8.

## Dynamic libraries

| Your goal                                       | Use                                                       |
| ----------------------------------------------- | --------------------------------------------------------- |
| Load a plugin from a known file                 | `dynlib_open()` with `DYNLIB_SEARCH_PATH` (absolute path) |
| Load a system library (Vulkan, `libandroid.so`) | `dynlib_open()` with `DYNLIB_SEARCH_SYSTEM` (bare name)   |
| Find a function or variable                     | `dynlib_symbol()` (terminated name, or pointer and size)  |
| Build a platform file name                      | `DYNLIB_PREFIX` + name + `DYNLIB_SUFFIX`                  |
| Know why a load failed                          | `dynlib_open_desc::message`, a buffer you own             |

A `dynlib_handle` is the native handle. **Loading a library again returns the same handle** with one more reference; every `dynlib_open()` needs its `dynlib_close()` (tracked builds count the references and report the leftovers as one leak). Symbols are bound at load time and stay local (`RTLD_NOW | RTLD_LOCAL`, the only behavior Windows has): a missing symbol fails the load instead of the first call, and one library never satisfies another's undeclared dependencies. Initializers run under the loader lock: never load or release a library from inside one.

**Releasing does not guarantee unloading.**

|                              | Linux, Android, Apple                                                            | Windows                                                                                                                                       |
| ---------------------------- | -------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------- |
| `PATH`                       | `dlopen` of the absolute path; dependencies through its `RUNPATH` / `@rpath`     | `LoadLibraryExW` with `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR` and `_DEFAULT_DIRS`: dependencies from the library's directory, then the system      |
| `SYSTEM`                     | `dlopen` of the bare name (the loader's search)                                  | `LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`: the application directory, System32, `AddDllDirectory` directories; never the current directory or `PATH` |
| Message                      | `dlerror()`, names a missing dependency                                          | The system message for the error code                                                                                                         |
| `NOT_FOUND`, `ACCESS_DENIED` | The file itself is missing or unreadable (a missing dependency is `LOAD_FAILED`) | The same                                                                                                                                      |

Platform rules: iOS loads only signed libraries inside the app bundle (`Frameworks/`) or the OS, never downloaded code. Android loads only the app's own libraries and the public NDK ones; uncompressed, page-aligned libraries load straight from the APK (`base.apk!/lib/arm64-v8a/libx.so`).

## Supported targets

| Platform              | Architectures     | Toolchains     | CI                                  |
| --------------------- | ----------------- | -------------- | ----------------------------------- |
| Linux                 | x86-64, arm64     | GCC, Clang     | tests, ASan+UBSan, TSan             |
| Windows               | x86-64, arm64     | MSVC, clang-cl | tests                               |
| macOS 14.4+           | arm64, x86-64     | Apple Clang    | tests, ASan+UBSan, TSan (arm64)     |
| Android 8.0+ (API 26) | arm64-v8a, x86_64 | NDK Clang      | build, tests on the x86_64 emulator |
| iOS 17.4+             | arm64             | Apple Clang    | build, tests in the simulator       |

64-bit only. The Apple minimums come from `os_sync_wait_on_address` (a build for an older deployment target stops with an error). On Windows, osprim links `synchronization.lib` and `winmm.lib`; on Apple, Foundation and the Objective-C runtime; on Android, libandroid is loaded at run time.

## Building

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/tests/osprim_tests
```

| Option                      | Default        | Effect                                                                                          |
| --------------------------- | -------------- | ----------------------------------------------------------------------------------------------- |
| `OSPRIM_BUILD_TESTS`        | top-level only | Build the tests (fetches doctest)                                                               |
| `OSPRIM_INSTALL`            | top-level only | Generate the install target and the `osprim` CMake package                                      |
| `OSPRIM_SHARED`             | `OFF`          | Build a shared library (static otherwise); see [One copy per process](#one-copy-per-process)    |
| `OSPRIM_TRACKING`           | `AUTO`         | Compile handle tracking in: `AUTO` (Debug only), `ON`, `OFF`                                    |
| `OSPRIM_USER_CONFIG`        | empty          | Path of a header included first by every osprim header, for example to override `OSPRIM_ASSERT` |
| `OSPRIM_WARNINGS_AS_ERRORS` | `OFF`          | `-Werror` / `/WX` (on in CI)                                                                    |

As a subproject:

```cmake
add_subdirectory(osprim)
target_link_libraries(engine PRIVATE osprim::osprim)
```

### One copy per process

osprim's state (initialization, handle tracking, the diagnostic callback and allocator) belongs to one copy of the library, and a process must contain exactly one: a static library linked into two modules (an executable and a shared library, or two shared libraries) is two copies, each needing its own `initialize()`, and a handle opened through one copy and closed through the other is reported as leaked and misused. Three ways to have one copy:

| Layout                                                 | How                                                                                                                        |
| ------------------------------------------------------ | -------------------------------------------------------------------------------------------------------------------------- |
| Everything static                                      | Link osprim into the executable only                                                                                       |
| osprim as its own shared library                       | `-DOSPRIM_SHARED=ON`: exports the `OSPRIM_API` functions (everything the headers declare), nothing else                    |
| osprim inside another shared library, exported from it | Link osprim statically into that library only, as a whole archive, and define `OSPRIM_API` in `OSPRIM_USER_CONFIG` (below) |

```cmake
set(OSPRIM_USER_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/osprim_config.hpp")
add_subdirectory(osprim)
target_compile_definitions(example PRIVATE FRICE_OS_BUILDING=1)
target_link_libraries(example
  # Whole archive: the linker would otherwise drop the osprim objects
  # 'example' itself does not call, and their exports with them
  PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,osprim::osprim>"
  # Users of frice_os get osprim's headers and definitions, never a copy of
  # its code (CMake 3.27)
  PUBLIC "$<COMPILE_ONLY:osprim::osprim>")
```

```cpp
// osprim_config.hpp. OSPRIM_BUILDING is set by osprim's CMake for its own sources.
#if defined(_WIN32)
  #if defined(OSPRIM_BUILDING) || defined(FRICE_OS_BUILDING)
    #define OSPRIM_API __declspec(dllexport)
  #else
    #define OSPRIM_API __declspec(dllimport)
  #endif
#else
  #define OSPRIM_API __attribute__((visibility("default")))
#endif
```

Other modules then link `example` only, never osprim. In a shared build, the test suite is `tests/test_shared.cpp` (handles crossing modules, one clock state): the main suite drives internal functions that the shared library does not export.

## License

MIT, see [LICENSE](LICENSE).
