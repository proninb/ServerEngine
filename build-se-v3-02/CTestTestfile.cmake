# CMake generated Testfile for 
# Source directory: C:/Boris/ServerEngine
# Build directory: C:/Boris/ServerEngine/build-se-v3-02
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
if(CTEST_CONFIGURATION_TYPE MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
  add_test("server_engine_foundation" "C:/Boris/ServerEngine/build-se-v3-02/Debug/server_engine_tests.exe")
  set_tests_properties("server_engine_foundation" PROPERTIES  _BACKTRACE_TRIPLES "C:/Boris/ServerEngine/CMakeLists.txt;33;add_test;C:/Boris/ServerEngine/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
  add_test("server_engine_foundation" "C:/Boris/ServerEngine/build-se-v3-02/Release/server_engine_tests.exe")
  set_tests_properties("server_engine_foundation" PROPERTIES  _BACKTRACE_TRIPLES "C:/Boris/ServerEngine/CMakeLists.txt;33;add_test;C:/Boris/ServerEngine/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
  add_test("server_engine_foundation" "C:/Boris/ServerEngine/build-se-v3-02/MinSizeRel/server_engine_tests.exe")
  set_tests_properties("server_engine_foundation" PROPERTIES  _BACKTRACE_TRIPLES "C:/Boris/ServerEngine/CMakeLists.txt;33;add_test;C:/Boris/ServerEngine/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
  add_test("server_engine_foundation" "C:/Boris/ServerEngine/build-se-v3-02/RelWithDebInfo/server_engine_tests.exe")
  set_tests_properties("server_engine_foundation" PROPERTIES  _BACKTRACE_TRIPLES "C:/Boris/ServerEngine/CMakeLists.txt;33;add_test;C:/Boris/ServerEngine/CMakeLists.txt;0;")
else()
  add_test("server_engine_foundation" NOT_AVAILABLE)
endif()
