/*****************************************************************/ /**
 * @file   user_config.hpp
 * @brief  Test configuration for the OSPRIM_USER_CONFIG CI job: contract
 * violations are counted instead of aborting.
 *
 * Configure with -DOSPRIM_USER_CONFIG=<absolute path to this file>.
 *********************************************************************/
#ifndef OSPRIM_TESTS_USER_CONFIG_HPP
#define OSPRIM_TESTS_USER_CONFIG_HPP

#include <atomic>

#define OSPRIM_TEST_USER_CONFIG 1

/// @brief Number of failed OSPRIM_ASSERT checks (library and tests share it).
inline std::atomic<int> osprim_test_assert_failures{0};

#define OSPRIM_ASSERT(expr, msg) \
  ((expr) ? (void)0 : ((void)(msg), (void)osprim_test_assert_failures.fetch_add(1)))

#endif // !OSPRIM_TESTS_USER_CONFIG_HPP
