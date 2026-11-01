/*****************************************************************/ /**
 * @file   platform.hpp
 * @brief  Internal: identifies the target operating system and architecture.
 *
 * Exactly one OSPRIM_OS_* and one OSPRIM_ARCH_* macro are 1. The numeric
 * OSPRIM_PLATFORM_ID must match the OSPRIM_CMAKE_PLATFORM definition passed
 * by CMake, which guarantees that the sources selected by the build system
 * match the target the compiler actually generates code for.
 *********************************************************************/
#ifndef OSPRIM_SRC_PLATFORM_HPP
#define OSPRIM_SRC_PLATFORM_HPP

#define OSPRIM_OS_LINUX   0
#define OSPRIM_OS_ANDROID 0
#define OSPRIM_OS_WINDOWS 0
#define OSPRIM_OS_APPLE   0

#if defined(_M_ARM64EC)
  #error "osprim: ARM64EC is not supported"
#elif defined(_WIN32)
  #undef OSPRIM_OS_WINDOWS
  #define OSPRIM_OS_WINDOWS  1
  #define OSPRIM_PLATFORM_ID 3
#elif defined(__APPLE__)
  #undef OSPRIM_OS_APPLE
  #define OSPRIM_OS_APPLE    1
  #define OSPRIM_PLATFORM_ID 4
#elif defined(__ANDROID__)
  #undef OSPRIM_OS_ANDROID
  #define OSPRIM_OS_ANDROID  1
  #define OSPRIM_PLATFORM_ID 2
#elif defined(__linux__)
  #undef OSPRIM_OS_LINUX
  #define OSPRIM_OS_LINUX    1
  #define OSPRIM_PLATFORM_ID 1
#else
  #error "osprim: unsupported operating system"
#endif

#if defined(_M_X64) || defined(__x86_64__)
  #define OSPRIM_ARCH_X86_64 1
  #define OSPRIM_ARCH_ARM64  0
#elif defined(_M_ARM64) || defined(__aarch64__) || defined(__arm64__)
  #define OSPRIM_ARCH_X86_64 0
  #define OSPRIM_ARCH_ARM64  1
#else
  #error "osprim: unsupported architecture (x86-64 and arm64 only)"
#endif

#if defined(OSPRIM_CMAKE_PLATFORM) && OSPRIM_CMAKE_PLATFORM != OSPRIM_PLATFORM_ID
  #error \
      "osprim: the build system selected sources for a different platform than the compiler targets"
#endif

#endif // !OSPRIM_SRC_PLATFORM_HPP
