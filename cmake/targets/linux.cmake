# linux specific target definitions

if(NOT FREEBSD)
    # Using newer c++ compilers / features on older distros causes runtime dyn link errors
    list(APPEND SUNSHINE_EXTERNAL_LIBRARIES -static-libgcc)

    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 15)
        list(APPEND SUNSHINE_EXTERNAL_LIBRARIES stdc++)
    else()
        list(APPEND SUNSHINE_EXTERNAL_LIBRARIES -static-libstdc++)
    endif()
endif()

option(STEAMSHINE_BUILD_INPUT_VISUALIZER "Build the SteamShine fullscreen input-latency probe" ON)
if(STEAMSHINE_BUILD_INPUT_VISUALIZER AND X11_FOUND AND X11_Xi_LIB)
    add_executable(steamshine_input_visualizer
        "${CMAKE_SOURCE_DIR}/tools/steamshine_input_visualizer.cpp")
    target_include_directories(steamshine_input_visualizer PRIVATE
        "${X11_INCLUDE_DIR}"
        "${X11_Xi_INCLUDE_PATH}")
    target_link_libraries(steamshine_input_visualizer PRIVATE
        "${X11_LIBRARIES}"
        "${X11_Xi_LIB}")
    target_compile_options(steamshine_input_visualizer PRIVATE ${SUNSHINE_COMPILE_OPTIONS})
    set_target_properties(steamshine_input_visualizer PROPERTIES
        OUTPUT_NAME "steamshine-input-visualizer")
    install(TARGETS steamshine_input_visualizer
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
