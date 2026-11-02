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

set(NETMIC_VERSION 0.2.0)

# Pure logic (no Core Audio / libASPL): shared by the plugin and the unit tests.
add_library(netmic_core STATIC src/JitterBuffer.cpp src/ToneGenerator.cpp src/Config.cpp)
target_include_directories(netmic_core PUBLIC src ${NETMIC_SHARED_DIR})
target_compile_options(netmic_core PRIVATE -Wall -Wextra)
target_link_libraries(netmic_core PUBLIC "-framework CoreFoundation")

option(NETMIC_BUILD_TESTS "Build unit tests" ON)
if(NETMIC_BUILD_TESTS)
  enable_testing()
  add_executable(netmic_tests tests/test_main.cpp tests/test_protocol.cpp tests/test_jitter_buffer.cpp
                              tests/test_config.cpp tests/test_tone.cpp)
  target_link_libraries(netmic_tests PRIVATE netmic_core)
  target_compile_definitions(netmic_tests PRIVATE NETMIC_TEST_VECTORS="${NETMIC_SHARED_DIR}/test_vectors.txt")
  target_compile_options(netmic_tests PRIVATE -Wall -Wextra)
  add_test(NAME netmic_tests COMMAND netmic_tests)
endif()

# libASPL (git submodule), built and installed into the build tree.
include(ExternalProject)
set(LIBASPL_PREFIX ${CMAKE_CURRENT_BINARY_DIR}/libASPL-prefix)
ExternalProject_Add(libASPL
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libASPL
  BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/libASPL-build
  INSTALL_DIR ${LIBASPL_PREFIX}
  CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
             -DCMAKE_BUILD_TYPE=Release
             -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
             -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
             -DBUILD_TESTING=OFF
  BUILD_BYPRODUCTS ${LIBASPL_PREFIX}/lib/libASPL.a
  LOG_CONFIGURE YES
  LOG_BUILD YES
  LOG_INSTALL YES
)

# NetMic.driver: Contents/Info.plist + Contents/MacOS/NetMic (a Mach-O bundle).
add_library(NetMic MODULE src/Plugin.cpp src/NetMicDevice.cpp)
add_dependencies(NetMic libASPL)
target_include_directories(NetMic SYSTEM PRIVATE ${LIBASPL_PREFIX}/include)
target_link_libraries(NetMic PRIVATE netmic_core ${LIBASPL_PREFIX}/lib/libASPL.a
                      "-framework CoreFoundation" "-framework CoreAudio")
target_compile_options(NetMic PRIVATE -Wall -Wextra -Wno-unused-parameter)
# Not build/Info.plist: CoreFoundation would treat build/ as the main bundle of
# the test tools there, and then fail to open NetMic.driver inside it.
configure_file(Info.plist.in ${CMAKE_CURRENT_BINARY_DIR}/plist/Info.plist @ONLY)
set_target_properties(NetMic PROPERTIES
  BUNDLE TRUE
  BUNDLE_EXTENSION driver
  PREFIX ""
  SUFFIX ""
  MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_BINARY_DIR}/plist/Info.plist
)

# Ad-hoc signature by default; CODESIGN_IDENTITY="Apple Development: ..." to use a certificate.
set(CODESIGN_IDENTITY "$ENV{CODESIGN_IDENTITY}" CACHE STRING "codesign identity (empty: ad-hoc)")
if(CODESIGN_IDENTITY STREQUAL "")
  set(NETMIC_SIGN_ID "-")
else()
  set(NETMIC_SIGN_ID "${CODESIGN_IDENTITY}")
endif()
add_custom_command(TARGET NetMic POST_BUILD
  COMMAND codesign --force --sign "${NETMIC_SIGN_ID}" --timestamp=none
          "$<TARGET_BUNDLE_DIR:NetMic>"
  COMMENT "Signing NetMic.driver (${NETMIC_SIGN_ID})"
  VERBATIM)

if(NETMIC_BUILD_TESTS)
  # Loads the built NetMic.driver in-process and drives it like the HAL does.
  add_executable(netmic_plugin_smoke tests/plugin_smoke.cpp)
  target_link_libraries(netmic_plugin_smoke PRIVATE "-framework CoreFoundation" "-framework CoreAudio")
  target_compile_options(netmic_plugin_smoke PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
  add_dependencies(netmic_plugin_smoke NetMic)
  add_test(NAME netmic_plugin_smoke COMMAND netmic_plugin_smoke $<TARGET_BUNDLE_DIR:NetMic>)
endif()
