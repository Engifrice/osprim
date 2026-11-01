# osprim
Low-level, unopinionated operating system primitives for game engines, in C++20.

osprim provides **mechanism, not policy**: the lowest-level building blocks, with the same API on every target, on top of which an engine builds its own safe and convenient layer. It never allocates, and never throws.

## Status:
[![CI](https://github.com/Engifrice/osprim/actions/workflows/CI.yml/badge.svg?branch=main&event=push)](https://github.com/Engifrice/osprim/actions/workflows/CI.yml)

| Module | Header              | Contents                                                                                              |
| ------ | ------------------- | ----------------------------------------------------------------------------------------------------- |
| Base   | `osprim/base.hpp`   | Library lifetime, results, native errors, diagnostics (leak and misuse reports)                       |
| Clock  | `osprim/clock.hpp`  | Awake, continuous and wall clocks; raw tick counter                                                   |
| Wait   | `osprim/wait.hpp`   | Wait on an address until woken or an awake deadline; wake one or all                                  |
| Thread | `osprim/thread.hpp` | Create, join with a deadline, detach; ids, names, scheduling classes; yield, spin hint, precise sleep |

`osprim/osprim.hpp` includes everything.

## Rules

- `osprim::initialize()` is called exactly once before any other osprim function, and `osprim::deinitialize()` exactly once after the last one. Initialization may be repeated after deinitialization. Neither is thread-safe.
- Every function is `noexcept`. The library is built without exceptions and RTTI.
- The library never allocates, except for handle tracking (debug), which goes through the allocator hooks given to `initialize()`.
- Fallible operations return `status<Result>`: the operation's own `enum class` (value 0 is success) plus the native error behind it (errno, Win32, NTSTATUS, `kern_return_t`).
- Preconditions are checked with `OSPRIM_ASSERT(expr, msg)`, `assert()` by default. Misuse that can only be detected at runtime (leaked or double-closed handles) is reported through the diagnostic callback.
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

| `thread_class` | Windows        | Apple QoS          | Linux, Android nice |
| -------------- | -------------- | ------------------ | ------------------- |
| `BACKGROUND`   | `LOWEST`       | `BACKGROUND`       | 10                  |
| `UTILITY`      | `BELOW_NORMAL` | `UTILITY`          | 5                   |
| `NORMAL`       | `NORMAL`       | `USER_INITIATED`   | 0                   |
| `INTERACTIVE`  | `ABOVE_NORMAL` | `USER_INTERACTIVE` | -6                  |

Desktop Linux usually refuses unprivileged users any decrease of the nice value: `INTERACTIVE`, and `NORMAL` after a lower class, return `SET_THREAD_CLASS_DENIED` (Android allows them).

## Supported targets

| Platform              | Architectures     | Toolchains     | CI                                  |
| --------------------- | ----------------- | -------------- | ----------------------------------- |
| Linux                 | x86-64, arm64     | GCC, Clang     | tests, ASan+UBSan, TSan             |
| Windows               | x86-64, arm64     | MSVC, clang-cl | tests                               |
| macOS 14.4+           | arm64, x86-64     | Apple Clang    | tests, ASan+UBSan, TSan (arm64)     |
| Android 8.0+ (API 26) | arm64-v8a, x86_64 | NDK Clang      | build, tests on the x86_64 emulator |
| iOS 17.4+             | arm64             | Apple Clang    | build                               |

64-bit only. The Apple minimums come from `os_sync_wait_on_address` (a build for an older deployment target stops with an error). On Windows, osprim links `synchronization.lib`. Consoles will follow when their SDKs are available.

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
| `OSPRIM_TRACKING`           | `AUTO`         | Compile handle tracking in: `AUTO` (Debug only), `ON`, `OFF`                                    |
| `OSPRIM_USER_CONFIG`        | empty          | Path of a header included first by every osprim header, for example to override `OSPRIM_ASSERT` |
| `OSPRIM_WARNINGS_AS_ERRORS` | `OFF`          | `-Werror` / `/WX` (on in CI)                                                                    |

As a subproject:

```cmake
add_subdirectory(osprim)
target_link_libraries(engine PRIVATE osprim::osprim)
```

## License

MIT, see [LICENSE](LICENSE).
