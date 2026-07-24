cmake_minimum_required(VERSION 3.20)

# This project deliberately does not include the root Sunshine CMake graph.
# It is the fast, hermetic layer for pure SteamOS request and Gamescope-command
# behavior; lifecycle tests remain part of the root test_sunshine target.
project(steamshine_steamos_core_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()

set(GTEST_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../third-party/lizardbyte-common/third-party/googletest")
set(INSTALL_GTEST OFF)
set(INSTALL_GMOCK OFF)
add_subdirectory("${GTEST_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/googletest")

add_library(steamos_virtual_session_core STATIC
    "${CMAKE_CURRENT_LIST_DIR}/../../src/steamos_virtual_session_core.cpp"
)
target_include_directories(steamos_virtual_session_core PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../..")
target_compile_options(steamos_virtual_session_core PRIVATE -Wall -Wno-sign-compare)

add_executable(test_steamos_virtual_session_core
    "${CMAKE_CURRENT_LIST_DIR}/../unit/test_steamos_virtual_session_core.cpp"
)
target_link_libraries(test_steamos_virtual_session_core PRIVATE steamos_virtual_session_core gtest_main)
add_test(NAME SteamOSVirtualSessionCore COMMAND test_steamos_virtual_session_core)
