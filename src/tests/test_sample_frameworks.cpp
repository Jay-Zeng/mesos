#include <gtest/gtest.h>

#include "external_test.hpp"

// Run each of the sample frameworks in local mode
TEST_EXTERNAL(SampleFrameworks, CFramework)
TEST_EXTERNAL(SampleFrameworks, CppFramework)
#ifdef MESOS_HAS_JAVA
  TEST_EXTERNAL(SampleFrameworks, JavaFramework)
#endif 
#ifdef MESOS_HAS_PYTHON
  TEST_EXTERNAL(SampleFrameworks, PythonFramework)
#endif
