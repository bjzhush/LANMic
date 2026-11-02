cmake_minimum_required(VERSION 3.20)
project(NetMicDriver LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_ARCHITECTURES arm64)
set(CMAKE_OSX_DEPLOYMENT_TARGET 13.0)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(NETMIC_SHARED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../shared)

# Pure logic, no Core Audio: shared by the plugin (stage 2+) and the unit tests.
add_library(netmic_core STATIC src/JitterBuffer.cpp)
target_include_directories(netmic_core PUBLIC src ${NETMIC_SHARED_DIR})
target_compile_options(netmic_core PRIVATE -Wall -Wextra)

option(NETMIC_BUILD_TESTS "Build unit tests" ON)
if(NETMIC_BUILD_TESTS)
  enable_testing()
  add_executable(netmic_tests tests/test_main.cpp tests/test_protocol.cpp tests/test_jitter_buffer.cpp)
  target_link_libraries(netmic_tests PRIVATE netmic_core)
  target_compile_definitions(netmic_tests PRIVATE NETMIC_TEST_VECTORS="${NETMIC_SHARED_DIR}/test_vectors.txt")
  target_compile_options(netmic_tests PRIVATE -Wall -Wextra)
  add_test(NAME netmic_tests COMMAND netmic_tests)
endif()

# The NetMic.driver AudioServerPlugIn bundle (libASPL) is added in stage 2.
