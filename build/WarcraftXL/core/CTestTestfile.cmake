# CMake generated Testfile for 
# Source directory: C:/WXL-WoW-Decomp/core
# Build directory: C:/WXL-WoW-Decomp/WXLMorfyClient/Build/WarcraftXL/core
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
if(CTEST_CONFIGURATION_TYPE MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
  add_test([=[seams]=] "C:/WXL-WoW-Decomp/WXLMorfyClient/Build/WarcraftXL/core/Debug/wxl-core-seam-harness.exe")
  set_tests_properties([=[seams]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/WXL-WoW-Decomp/core/CMakeLists.txt;183;add_test;C:/WXL-WoW-Decomp/core/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
  add_test([=[seams]=] "C:/WXL-WoW-Decomp/WXLMorfyClient/Build/WarcraftXL/core/Release/wxl-core-seam-harness.exe")
  set_tests_properties([=[seams]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/WXL-WoW-Decomp/core/CMakeLists.txt;183;add_test;C:/WXL-WoW-Decomp/core/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
  add_test([=[seams]=] "C:/WXL-WoW-Decomp/WXLMorfyClient/Build/WarcraftXL/core/MinSizeRel/wxl-core-seam-harness.exe")
  set_tests_properties([=[seams]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/WXL-WoW-Decomp/core/CMakeLists.txt;183;add_test;C:/WXL-WoW-Decomp/core/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
  add_test([=[seams]=] "C:/WXL-WoW-Decomp/WXLMorfyClient/Build/WarcraftXL/core/RelWithDebInfo/wxl-core-seam-harness.exe")
  set_tests_properties([=[seams]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/WXL-WoW-Decomp/core/CMakeLists.txt;183;add_test;C:/WXL-WoW-Decomp/core/CMakeLists.txt;0;")
else()
  add_test([=[seams]=] NOT_AVAILABLE)
endif()
subdirs("sdk")
