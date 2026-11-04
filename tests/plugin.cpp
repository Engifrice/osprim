/*****************************************************************/ /**
 * @file   plugin.cpp
 * @brief  The shared library the dynamic library tests load: a function,
 * a variable and some state (to tell whether two loads share one image).
 * Uses nothing from the C++ runtime, so it loads without its libraries.
 *********************************************************************/
#if defined(_WIN32)
  #define OSPRIM_TEST_PLUGIN_EXPORT __declspec(dllexport)
#else
  #define OSPRIM_TEST_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

namespace
{
  int g_calls = 0;
}

extern "C"
{
  OSPRIM_TEST_PLUGIN_EXPORT int osprim_test_plugin_value = 42;

  OSPRIM_TEST_PLUGIN_EXPORT int osprim_test_plugin_add(int a, int b)
  {
    return a + b;
  }

  /// @brief Counts its calls: the state of the one loaded image.
  OSPRIM_TEST_PLUGIN_EXPORT int osprim_test_plugin_count()
  {
    return ++g_calls;
  }
}
