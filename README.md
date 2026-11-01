# osprim
Low-level, unopinionated operating system primitives for game engines, in C++20.

osprim provides **mechanism, not policy**: the lowest-level building blocks, with the same API on every target, on top of which an engine builds its own safe and convenient layer. It never allocates, and never throws.

## Status:
[![CI](https://github.com/Engifrice/osprim/actions/workflows/CI.yml/badge.svg?branch=main&event=push)](https://github.com/Engifrice/osprim/actions/workflows/CI.yml)

| Module | Header             | Contents                                                                        |
| ------ | ------------------ | ------------------------------------------------------------------------------- |
| Base   | `osprim/base.hpp`  | Library lifetime, results, native errors, diagnostics (leak and misuse reports) |
| Clock  | `osprim/clock.hpp` | Awake, continuous and wall clocks; raw tick counter                             |

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

## Supported targets

| Platform | Architectures     | Toolchains     | CI                                  |
| -------- | ----------------- | -------------- | ----------------------------------- |
| Linux    | x86-64, arm64     | GCC, Clang     | tests, ASan+UBSan, TSan             |
| Windows  | x86-64, arm64     | MSVC, clang-cl | tests                               |
| macOS    | arm64, x86-64     | Apple Clang    | tests, ASan+UBSan, TSan (arm64)     |
| Android  | arm64-v8a, x86_64 | NDK Clang      | build, tests on the x86_64 emulator |
| iOS      | arm64             | Apple Clang    | build                               |

64-bit only.

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
