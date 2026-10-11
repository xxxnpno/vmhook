cmake_minimum_required(VERSION 3.28)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/modules/vmhook.ixx" _vmhook_version_line
    REGEX "version_string")
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" _vmhook_version "${_vmhook_version_line}")
if(NOT _vmhook_version)
    message(FATAL_ERROR "Cannot read VMHook's version from the canonical module.")
endif()

project(vmhook VERSION ${_vmhook_version}
    DESCRIPTION "Windows x64 GNU C++23 header and C++26 module for HotSpot internals"
    LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "VMHook supports Windows targets only. Use a Windows G++ toolchain.")
endif()
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "VMHook supports GNU G++ only.")
endif()
if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
    message(FATAL_ERROR "The C++23 header requires GNU G++ 14 or newer.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "VMHook requires Windows x64.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Header units are built explicitly by tools/build_module.py. Do not let CMake
# scan their importers: CMake's module scanner does not support header units.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

include(CheckCXXSourceCompiles)
set(_vmhook_saved_try_compile_target_type "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
check_cxx_source_compiles("\
#if !defined(_WIN64) || !defined(__x86_64__)
#error VMHook requires Windows x64
#endif
#include <windows.h>
#include <expected>
#include <format>
struct explicit_this { auto value(this const explicit_this&) -> int { return 1; } };
auto probe() -> int { return explicit_this{}.value(); }
" VMHOOK_CXX23_AVAILABLE)
set(CMAKE_TRY_COMPILE_TARGET_TYPE "${_vmhook_saved_try_compile_target_type}")
if(NOT VMHOOK_CXX23_AVAILABLE)
    message(FATAL_ERROR "G++ must provide Windows x64 SDK headers, C++23 explicit-object parameters, <expected>, and <format>.")
endif()

include(GNUInstallDirs)

add_library(vmhook INTERFACE)
add_library(vmhook::vmhook ALIAS vmhook)
target_compile_features(vmhook INTERFACE cxx_std_23)
target_include_directories(vmhook INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

option(VMHOOK_BUILD_MODULE "Build the C++26 reflection module (requires G++ 16+, Python, and libstdc++ module sources)" OFF)
option(VMHOOK_WARNINGS_AS_ERRORS "Treat warnings as errors in first-party tests" OFF)

function(vmhook_apply_warnings target)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic
        -Wno-unused-function -Wno-unused-local-typedefs
        -Wno-unused-but-set-variable -Wno-cast-function-type)
    if(VMHOOK_WARNINGS_AS_ERRORS)
        target_compile_options(${target} PRIVATE -Werror)
    endif()
endfunction()

# An injected DLL can opt in to avoid runtime DLL search-path dependencies.
function(vmhook_static_runtime target)
    target_link_options(${target} PRIVATE -static-libgcc -static-libstdc++ -static)
endfunction()

find_package(Python3 3.10 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_custom_target(regenerate_header
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_header.py"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" VERBATIM)
    add_custom_target(check_generated_header
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_header.py" --check
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" VERBATIM)
endif()

include(cmake/vmhookModule.cmake)
if(VMHOOK_BUILD_MODULE)
    vmhook_enable_module("${CMAKE_CURRENT_SOURCE_DIR}")
endif()

option(VMHOOK_BUILD_EXAMPLES "Build the standalone example DLLs" OFF)
option(VMHOOK_BUILD_BENCHMARKS "Build the injected runtime benchmark DLL" OFF)
if(VMHOOK_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
if(VMHOOK_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
    if(Python3_Interpreter_FOUND)
        add_test(NAME vmhook.wrapper_generator
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/wrapper_generator.py")
        add_test(NAME vmhook.source_contract
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_source.py")
    endif()
endif()

include(CMakePackageConfigHelpers)
install(FILES include/vmhook/vmhook.hpp DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/vmhook")
# Module CMIs are compiler/flag dependent. Install source, never cached CMIs.
install(FILES modules/vmhook.ixx modules/vmhook.windows.ixx DESTINATION "${CMAKE_INSTALL_DATADIR}/vmhook/modules")
install(FILES include/vmhook/detail/windows.hpp DESTINATION "${CMAKE_INSTALL_DATADIR}/vmhook/include/vmhook/detail")
install(FILES tools/build_module.py tools/make_header.py DESTINATION "${CMAKE_INSTALL_DATADIR}/vmhook/tools")
install(FILES tools/private_profiles.py tools/build_private_profiles.py
    DESTINATION "${CMAKE_INSTALL_DATADIR}/vmhook/tools")
foreach(_vmhook_optional_tool IN ITEMS inspect_runtime.py generate_wrappers.py)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/${_vmhook_optional_tool}")
        install(FILES "tools/${_vmhook_optional_tool}" DESTINATION "${CMAKE_INSTALL_DATADIR}/vmhook/tools")
    endif()
endforeach()
install(FILES data/native_profiles.json data/native_audits.json
    DESTINATION "${CMAKE_INSTALL_DATADIR}/vmhook/data")
install(TARGETS vmhook EXPORT vmhookTargets)
install(EXPORT vmhookTargets NAMESPACE vmhook:: FILE vmhookTargets.cmake
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vmhook")
configure_package_config_file(cmake/vmhookConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/vmhookConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vmhook"
    PATH_VARS CMAKE_INSTALL_DATADIR)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/vmhookConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMajorVersion)
install(FILES cmake/vmhookModule.cmake DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vmhook")
install(FILES LICENSE.txt DESTINATION "${CMAKE_INSTALL_DATADIR}/vmhook")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/vmhookConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/vmhookConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vmhook")
