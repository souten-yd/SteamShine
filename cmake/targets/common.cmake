# common target definitions
# this file will also load platform specific macros

if(APPLE AND NOT SUNSHINE_BUILD_HOMEBREW)
    add_executable(sunshine MACOSX_BUNDLE ${SUNSHINE_TARGET_FILES})
else()
    add_executable(sunshine ${SUNSHINE_TARGET_FILES})
endif()
foreach(dep ${SUNSHINE_TARGET_DEPENDENCIES})
    add_dependencies(sunshine ${dep})  # compile these before sunshine
endforeach()

# platform specific target definitions
if(WIN32)
    include(${CMAKE_MODULE_PATH}/targets/windows.cmake)
elseif(UNIX)
    include(${CMAKE_MODULE_PATH}/targets/unix.cmake)

    if(APPLE)
        include(${CMAKE_MODULE_PATH}/targets/macos.cmake)
    else()
        include(${CMAKE_MODULE_PATH}/targets/linux.cmake)
    endif()
endif()

target_link_libraries(sunshine ${SUNSHINE_EXTERNAL_LIBRARIES} ${EXTRA_LIBS})
target_compile_definitions(sunshine PUBLIC ${SUNSHINE_DEFINITIONS})

# CLion complains about unknown flags after running cmake, and cannot add symbols to the index for cuda files
if(CUDA_INHERIT_COMPILE_OPTIONS)
    foreach(flag IN LISTS SUNSHINE_COMPILE_OPTIONS)
        list(APPEND SUNSHINE_COMPILE_OPTIONS_CUDA "$<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=${flag}>")
    endforeach()
endif()

target_compile_options(sunshine PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>;$<$<COMPILE_LANGUAGE:CUDA>:${SUNSHINE_COMPILE_OPTIONS_CUDA};-std=c++17>)  # cmake-lint: disable=C0301
target_link_options(sunshine PRIVATE ${SUNSHINE_LINK_OPTIONS})

# Homebrew build fails the vite build if we set these environment variables
if(${SUNSHINE_BUILD_HOMEBREW})
    set(NPM_SOURCE_ASSETS_DIR "")
    set(NPM_ASSETS_DIR "")
    set(NPM_BUILD_HOMEBREW "true")
else()
    set(NPM_SOURCE_ASSETS_DIR ${SUNSHINE_SOURCE_ASSETS_DIR})
    set(NPM_ASSETS_DIR ${CMAKE_BINARY_DIR})
    set(NPM_BUILD_HOMEBREW "")
endif()

# Both Web UIs are built by default during the migration. These options provide
# a deterministic packaging boundary without changing the shared backend.
option(SUNSHINE_BUILD_UPSTREAM_WEB_UI "Build the upstream Sunshine Web UI" ON)
option(STEAMSHINE_BUILD_WEB_UI "Build the SteamShine Web UI" ON)

if(SUNSHINE_BUILD_UPSTREAM_WEB_UI)
    find_program(NPM npm REQUIRED)
    set(NPM_INSTALL_FLAGS --ignore-scripts)
    if (NPM_OFFLINE)
        list(APPEND NPM_INSTALL_FLAGS --offline)
    endif()

    # Dependency installation is keyed only to the npm manifests. This avoids
    # repeating `npm ci` for C++ builds and ordinary Web source changes.
    set(SUNSHINE_WEB_NPM_STAMP "${CMAKE_BINARY_DIR}/web-ui/npm-ci.stamp")
    add_custom_command(
            OUTPUT "${SUNSHINE_WEB_NPM_STAMP}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            COMMENT "Installing upstream Web UI NPM dependencies"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/web-ui"
            COMMAND "$<$<BOOL:${WIN32}>:cmd;/C>" "${NPM}" ci ${NPM_INSTALL_FLAGS}
            COMMAND "${CMAKE_COMMAND}" -E touch "${SUNSHINE_WEB_NPM_STAMP}"
            DEPENDS "${CMAKE_SOURCE_DIR}/package.json" "${CMAKE_SOURCE_DIR}/package-lock.json"
            COMMAND_EXPAND_LISTS
            VERBATIM)
    add_custom_target(web-ui-dependencies DEPENDS "${SUNSHINE_WEB_NPM_STAMP}")

    file(GLOB_RECURSE SUNSHINE_WEB_UI_SOURCES CONFIGURE_DEPENDS
            "${SUNSHINE_SOURCE_ASSETS_DIR}/common/assets/web/*")
    set(SUNSHINE_WEB_MANIFEST "${CMAKE_BINARY_DIR}/assets/web/.vite/manifest.json")
    add_custom_command(
            OUTPUT "${SUNSHINE_WEB_MANIFEST}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            COMMENT "Building the upstream Sunshine Web UI artifact"
            COMMAND "${CMAKE_COMMAND}" -E env "SUNSHINE_BUILD_HOMEBREW=${NPM_BUILD_HOMEBREW}" "SUNSHINE_SOURCE_ASSETS_DIR=${NPM_SOURCE_ASSETS_DIR}" "SUNSHINE_ASSETS_DIR=${NPM_ASSETS_DIR}" "$<$<BOOL:${WIN32}>:cmd;/C>" "${NPM}" run build-clean  # cmake-lint: disable=C0301
            DEPENDS "${SUNSHINE_WEB_NPM_STAMP}" "${CMAKE_SOURCE_DIR}/vite.config.js" ${SUNSHINE_WEB_UI_SOURCES}
            COMMAND_EXPAND_LISTS
            VERBATIM)
    add_custom_target(web-ui ALL DEPENDS "${SUNSHINE_WEB_MANIFEST}")
endif()

if(STEAMSHINE_BUILD_WEB_UI)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    file(GLOB_RECURSE STEAMSHINE_WEB_UI_SOURCES CONFIGURE_DEPENDS
            "${SUNSHINE_SOURCE_ASSETS_DIR}/common/assets/steamshine/*")
    set(STEAMSHINE_WEB_MANIFEST "${CMAKE_BINARY_DIR}/assets/steamshine/manifest.json")
    add_custom_command(
            OUTPUT "${STEAMSHINE_WEB_MANIFEST}"
            COMMENT "Building the SteamShine Web UI"
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/build-steamshine-web-assets.py" "${SUNSHINE_SOURCE_ASSETS_DIR}/common/assets/steamshine" "${CMAKE_BINARY_DIR}/assets/steamshine"
            DEPENDS "${CMAKE_SOURCE_DIR}/scripts/build-steamshine-web-assets.py" ${STEAMSHINE_WEB_UI_SOURCES}
            VERBATIM)
    add_custom_target(steamshine-web-ui ALL DEPENDS "${STEAMSHINE_WEB_MANIFEST}")
endif()

# Keep native and browser artifacts independently addressable. An explicit
# `sunshine` build now compiles only the native executable, while callers that
# need every browser bundle can request `web-artifacts`.
add_custom_target(sunshine-artifact DEPENDS sunshine)
add_custom_target(web-artifacts)
if(TARGET web-ui)
    add_dependencies(web-artifacts web-ui)
endif()
if(TARGET steamshine-web-ui)
    add_dependencies(web-artifacts steamshine-web-ui)
endif()

# docs
if(BUILD_DOCS)
    add_subdirectory(third-party/doxyconfig docs)
endif()

# tests
if(BUILD_TESTS)
    add_subdirectory(tests)
endif()

# custom compile flags, must be after adding tests

if (NOT BUILD_TESTS)
    set(TEST_DIR "")
else()
    set(TEST_DIR "${CMAKE_SOURCE_DIR}/tests")
endif()

# src/upnp
set_source_files_properties("${CMAKE_SOURCE_DIR}/src/upnp.cpp"
        DIRECTORY "${CMAKE_SOURCE_DIR}" "${TEST_DIR}"
        PROPERTIES COMPILE_FLAGS -Wno-pedantic)

# third-party/ViGEmClient
set(VIGEM_COMPILE_FLAGS "")
string(APPEND VIGEM_COMPILE_FLAGS "-Wno-unknown-pragmas ")
string(APPEND VIGEM_COMPILE_FLAGS "-Wno-misleading-indentation ")
string(APPEND VIGEM_COMPILE_FLAGS "-Wno-class-memaccess ")
string(APPEND VIGEM_COMPILE_FLAGS "-Wno-unused-function ")
string(APPEND VIGEM_COMPILE_FLAGS "-Wno-unused-variable ")
set_source_files_properties("${CMAKE_SOURCE_DIR}/third-party/ViGEmClient/src/ViGEmClient.cpp"
        DIRECTORY "${CMAKE_SOURCE_DIR}" "${TEST_DIR}"
        PROPERTIES
        COMPILE_DEFINITIONS "UNICODE=1;ERROR_INVALID_DEVICE_OBJECT_PARAMETER=650"
        COMPILE_FLAGS ${VIGEM_COMPILE_FLAGS})

# src/nvhttp
string(TOUPPER "x${CMAKE_BUILD_TYPE}" BUILD_TYPE)
if("${BUILD_TYPE}" STREQUAL "XDEBUG")
    if(WIN32)
        if (NOT BUILD_TESTS)
            set_source_files_properties("${CMAKE_SOURCE_DIR}/src/nvhttp.cpp"
                    DIRECTORY "${CMAKE_SOURCE_DIR}"
                    PROPERTIES COMPILE_FLAGS -O2)
        else()
            set_source_files_properties("${CMAKE_SOURCE_DIR}/src/nvhttp.cpp"
                    DIRECTORY "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/tests"
                    PROPERTIES COMPILE_FLAGS -O2)
        endif()
    endif()
else()
    add_definitions(-DNDEBUG)
endif()
