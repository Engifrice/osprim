/*****************************************************************/ /**
 * @file   platform.hpp
 * @brief  The target operating system and architecture, as seen by osprim.
 *
 * Exactly one OSPRIM_OS_* and one OSPRIM_ARCH_* macro is 1, the others 0.
 * Unsupported targets stop with an error.
 *********************************************************************/
#ifndef OSPRIM_PLATFORM_HPP
#define OSPRIM_PLATFORM_HPP

#define OSPRIM_OS_LINUX   0
#define OSPRIM_OS_ANDROID 0
#define OSPRIM_OS_WINDOWS 0
#define OSPRIM_OS_APPLE   0

#if defined(_M_ARM64EC)
  #error "osprim: ARM64EC is not supported"
#elif defined(_WIN32)
  #undef OSPRIM_OS_WINDOWS
  #define OSPRIM_OS_WINDOWS 1
#elif defined(__APPLE__)
  #undef OSPRIM_OS_APPLE
  #define OSPRIM_OS_APPLE 1
#elif defined(__ANDROID__)
  #undef OSPRIM_OS_ANDROID
  #define OSPRIM_OS_ANDROID 1
#elif defined(__linux__)
  #undef OSPRIM_OS_LINUX
  #define OSPRIM_OS_LINUX 1
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

#endif // !OSPRIM_PLATFORM_HPP
