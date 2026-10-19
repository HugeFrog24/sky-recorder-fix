cmake_minimum_required(VERSION 3.22)

if(NOT CMAKE_TOOLCHAIN_FILE)
    set(ndk_candidates "${CMAKE_ANDROID_NDK}" "$ENV{ANDROID_NDK_HOME}" "$ENV{ANDROID_NDK_ROOT}")
    set(sdk_candidates "$ENV{ANDROID_HOME}" "$ENV{ANDROID_SDK_ROOT}")
    if(CMAKE_HOST_WIN32)
        list(APPEND sdk_candidates "$ENV{LOCALAPPDATA}/Android/Sdk")
    elseif(CMAKE_HOST_APPLE)
        list(APPEND sdk_candidates "$ENV{HOME}/Library/Android/sdk")
    else()
        list(APPEND sdk_candidates "$ENV{HOME}/Android/Sdk")
    endif()
    foreach(sdk IN LISTS sdk_candidates)
        if(sdk)
            file(GLOB installed_ndks LIST_DIRECTORIES true "${sdk}/ndk/*")
            list(SORT installed_ndks COMPARE NATURAL ORDER DESCENDING)
            list(APPEND ndk_candidates ${installed_ndks} "${sdk}/ndk-bundle")
        endif()
    endforeach()
    foreach(ndk IN LISTS ndk_candidates)
        if(ndk AND EXISTS "${ndk}/build/cmake/android.toolchain.cmake")
            set(CMAKE_TOOLCHAIN_FILE "${ndk}/build/cmake/android.toolchain.cmake" CACHE FILEPATH "Android toolchain")
            break()
        endif()
    endforeach()
    if(NOT CMAKE_TOOLCHAIN_FILE)
        message(FATAL_ERROR "Set ANDROID_NDK_HOME or CMAKE_TOOLCHAIN_FILE to an Android NDK.")
    endif()
endif()

if(CMAKE_GENERATOR MATCHES "Ninja" AND NOT CMAKE_MAKE_PROGRAM)
    foreach(sdk IN LISTS sdk_candidates)
        if(sdk)
            file(GLOB cmake_bins LIST_DIRECTORIES true "${sdk}/cmake/*/bin")
            list(SORT cmake_bins COMPARE NATURAL ORDER DESCENDING)
            list(APPEND ninja_hints ${cmake_bins})
        endif()
    endforeach()
    find_program(sdk_ninja NAMES ninja ninja-build HINTS ${ninja_hints})
    if(sdk_ninja)
        set(CMAKE_MAKE_PROGRAM "${sdk_ninja}" CACHE FILEPATH "Ninja executable")
    endif()
endif()

set(ANDROID_ABI arm64-v8a CACHE STRING "Android ABI")
set(ANDROID_PLATFORM android-26 CACHE STRING "Minimum Android API")
set(ANDROID_STL c++_static CACHE STRING "C++ runtime")
project(sky_recorder_fix LANGUAGES CXX)
if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "This module requires Android arm64-v8a.")
endif()
option(SKY_RECORDER_TRACE "Enable recorder diagnostics" OFF)
option(SKY_RECORDER_LINT "Enable strict clang-tidy checks" OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
add_library(sky_recorder_fix SHARED src/fix.cpp src/locator.cpp)
set_target_properties(sky_recorder_fix PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
target_compile_definitions(sky_recorder_fix PRIVATE SKY_RECORDER_TRACE=$<BOOL:${SKY_RECORDER_TRACE}>)
target_compile_options(sky_recorder_fix PRIVATE -O2 -fno-exceptions -fno-rtti -fvisibility=hidden
    -Wall -Wextra -Werror "-ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=."
    "-ffile-prefix-map=${CMAKE_BINARY_DIR}=build")
target_link_options(sky_recorder_fix PRIVATE -Wl,-z,max-page-size=16384
    -Wl,-z,relro,-z,now -Wl,--build-id=sha1 -Wl,--exclude-libs,ALL)
target_link_libraries(sky_recorder_fix PRIVATE log dl)
set_property(TARGET sky_recorder_fix APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/config.json")
add_custom_command(TARGET sky_recorder_fix POST_BUILD
    COMMAND "${CMAKE_OBJCOPY}" --strip-debug --remove-section=.config
        --add-section ".config=${CMAKE_CURRENT_SOURCE_DIR}/config.json"
        --set-section-flags .config=readonly "$<TARGET_FILE:sky_recorder_fix>"
    VERBATIM)

if(SKY_RECORDER_LINT)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    get_filename_component(llvm_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(CLANG_TIDY NAMES clang-tidy HINTS "${llvm_bin}" REQUIRED)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/fix.cpp" adapter_source)
    string(FIND "${adapter_source}" "using Callback" adapter_start)
    string(FIND "${adapter_source}" "static int memory_protection(" adapter_end)
    if(adapter_start LESS 0 OR adapter_end LESS adapter_start)
        message(FATAL_ERROR "Cannot extract the production adapter for linting.")
    endif()
    math(EXPR adapter_length "${adapter_end}-${adapter_start}")
    string(SUBSTRING "${adapter_source}" ${adapter_start} ${adapter_length} adapter_source)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/adapter-under-test.inc" "${adapter_source}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS src/fix.cpp)
    add_library(lint_tests OBJECT EXCLUDE_FROM_ALL tests/test_adapter.cpp tests/test_locator.cpp)
    set_target_properties(lint_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    target_include_directories(lint_tests PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}")
    target_compile_options(lint_tests PRIVATE -UNDEBUG)
    add_custom_target(lint
        COMMAND "${CLANG_TIDY}" --verify-config
        COMMAND "${CLANG_TIDY}" -p "${CMAKE_BINARY_DIR}" src/locator.cpp tests/test_locator.cpp
        COMMAND "${CLANG_TIDY}" -p "${CMAKE_BINARY_DIR}" src/fix.cpp
            --extra-arg=-USKY_RECORDER_TRACE --extra-arg=-DSKY_RECORDER_TRACE=0
        COMMAND "${CLANG_TIDY}" -p "${CMAKE_BINARY_DIR}" src/fix.cpp
            --extra-arg=-USKY_RECORDER_TRACE --extra-arg=-DSKY_RECORDER_TRACE=1
        COMMAND "${CLANG_TIDY}" -p "${CMAKE_BINARY_DIR}" tests/test_adapter.cpp --extra-arg=-DTEST_TRACE=0
        COMMAND "${CLANG_TIDY}" -p "${CMAKE_BINARY_DIR}" tests/test_adapter.cpp --extra-arg=-DTEST_TRACE=1
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        VERBATIM)
endif()
