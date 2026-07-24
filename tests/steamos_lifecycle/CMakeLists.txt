cmake_minimum_required(VERSION 3.20)

project(steamshine_steamos_lifecycle_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()

set(GTEST_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../third-party/lizardbyte-common/third-party/googletest")
set(INSTALL_GTEST OFF)
set(INSTALL_GMOCK OFF)
add_subdirectory("${GTEST_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/googletest")

find_package(Boost REQUIRED COMPONENTS log log_setup thread filesystem)
find_package(OpenSSL REQUIRED)

add_library(steamos_virtual_session_lifecycle STATIC
    "${CMAKE_CURRENT_LIST_DIR}/../../src/steamos_virtual_session.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../src/steamos_virtual_session_core.cpp"
    "stubs.cpp"
)
target_include_directories(steamos_virtual_session_lifecycle PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../..")
target_link_libraries(steamos_virtual_session_lifecycle PRIVATE Boost::log Boost::log_setup Boost::thread Boost::filesystem)
target_compile_definitions(steamos_virtual_session_lifecycle PRIVATE SUNSHINE_TESTS)
target_compile_options(steamos_virtual_session_lifecycle PRIVATE -Wall -Wno-sign-compare)

add_executable(test_steamos_virtual_session_lifecycle
    "${CMAKE_CURRENT_LIST_DIR}/../unit/test_steamos_virtual_session.cpp"
)
target_link_libraries(test_steamos_virtual_session_lifecycle PRIVATE steamos_virtual_session_lifecycle gtest_main Boost::log Boost::log_setup Boost::thread Boost::filesystem OpenSSL::Crypto)
target_compile_definitions(test_steamos_virtual_session_lifecycle PRIVATE SUNSHINE_TESTS)
add_test(NAME SteamOSVirtualSessionLifecycle COMMAND test_steamos_virtual_session_lifecycle)
