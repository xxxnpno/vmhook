cmake_minimum_required(VERSION 3.28)

project(vmhook VERSION 8.0.0
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

add_library(vmhook INTERFACE)
add_library(vmhook::vmhook ALIAS vmhook)
target_compile_features(vmhook INTERFACE cxx_std_23)
target_include_directories(vmhook INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>")

option(VMHOOK_BUILD_MODULE "Build the C++26 reflection module (requires G++ 16+, Python, and libstdc++ module sources)" OFF)
set(VMHOOK_STD_MODULE_SOURCE "" CACHE FILEPATH "Optional override for the compiler's std module source location")
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

if(VMHOOK_BUILD_MODULE)
    if(CMAKE_VERSION VERSION_LESS 4.0)
        message(FATAL_ERROR "VMHOOK_BUILD_MODULE requires CMake 4.0 or newer for the GNU C++26 language mode.")
    endif()
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
        message(FATAL_ERROR "VMHOOK_BUILD_MODULE requires GNU G++ 16 or newer with -freflection support.")
    endif()
    if(NOT Python3_Interpreter_FOUND)
        message(FATAL_ERROR "VMHOOK_BUILD_MODULE requires Python 3.10 or newer.")
    endif()
    set(_vmhook_saved_required_flags "${CMAKE_REQUIRED_FLAGS}")
    set(_vmhook_saved_standard "${CMAKE_CXX_STANDARD}")
    set(CMAKE_CXX_STANDARD 26)
    set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -freflection")
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    check_cxx_source_compiles("\
#include <meta>
#include <type_traits>
static_assert(std::meta::is_type(^^int));
enum class kind { first, second };
consteval auto count() -> int {
    int result = 0;
    template for (constexpr auto e : std::define_static_array(std::meta::enumerators_of(^^kind))) { ++result; }
    return result;
}
static_assert(count() == 2);
struct annotation_name {
    const char* text;
    consteval explicit annotation_name(std::string_view value) : text{std::define_static_string(value)} {}
};
struct [[= annotation_name(\"probe\")]] annotated {};
static_assert(std::meta::annotations_of(^^annotated).size() == 1);
" VMHOOK_REFLECTION_AVAILABLE)
    set(CMAKE_TRY_COMPILE_TARGET_TYPE "${_vmhook_saved_try_compile_target_type}")
    set(CMAKE_CXX_STANDARD "${_vmhook_saved_standard}")
    set(CMAKE_REQUIRED_FLAGS "${_vmhook_saved_required_flags}")
    if(NOT VMHOOK_REFLECTION_AVAILABLE)
        message(FATAL_ERROR "The toolchain lacks the reflection and expansion statements needed by the C++26 module.")
    endif()

    set(_vmhook_module_dir "${CMAKE_CURRENT_BINARY_DIR}/module/$<CONFIG>")
    set(_vmhook_module_archive "${_vmhook_module_dir}/libvmhook.a")
    set(_vmhook_module_mapper "${_vmhook_module_dir}/module.mapper")
    separate_arguments(_vmhook_flags NATIVE_COMMAND "${CMAKE_CXX_FLAGS}")
    set(_vmhook_module_flags)
    if(VMHOOK_STD_MODULE_SOURCE)
        list(APPEND _vmhook_module_flags --std-source "${VMHOOK_STD_MODULE_SOURCE}")
    endif()
    foreach(_vmhook_flag IN LISTS _vmhook_flags)
        list(APPEND _vmhook_module_flags "--flag=${_vmhook_flag}")
    endforeach()
    foreach(_vmhook_config IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
        separate_arguments(_vmhook_config_flags NATIVE_COMMAND "${CMAKE_CXX_FLAGS_${_vmhook_config}}")
        foreach(_vmhook_flag IN LISTS _vmhook_config_flags)
            list(APPEND _vmhook_module_flags "$<$<CONFIG:${_vmhook_config}>:--flag=${_vmhook_flag}>")
        endforeach()
    endforeach()
    add_custom_command(OUTPUT "${_vmhook_module_archive}" "${_vmhook_module_mapper}"
        BYPRODUCTS "${_vmhook_module_dir}/std.o" "${_vmhook_module_dir}/vmhook.windows.o"
            "${_vmhook_module_dir}/vmhook.o" "${_vmhook_module_dir}/gcm/std.gcm"
            "${_vmhook_module_dir}/gcm/windows-sdk.gcm" "${_vmhook_module_dir}/gcm/vmhook.windows.gcm"
            "${_vmhook_module_dir}/gcm/vmhook.gcm" "${_vmhook_module_dir}/sdk/vmhook/detail/windows.hpp"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_module.py"
            --compiler "${CMAKE_CXX_COMPILER}" --ar "${CMAKE_AR}"
            --source-dir "${CMAKE_CURRENT_SOURCE_DIR}" --build-dir "${_vmhook_module_dir}"
            ${_vmhook_module_flags}
        DEPENDS modules/vmhook.ixx modules/vmhook.windows.ixx
            include/vmhook/detail/windows.hpp tools/build_module.py tools/make_header.py
            "${CMAKE_CXX_COMPILER}" "${CMAKE_AR}"
        COMMAND_EXPAND_LISTS VERBATIM)
    add_custom_target(vmhook_module_build ALL DEPENDS "${_vmhook_module_archive}" "${_vmhook_module_mapper}")
    add_library(vmhook_module INTERFACE)
    add_library(vmhook::module ALIAS vmhook_module)
    add_dependencies(vmhook_module vmhook_module_build)
    target_compile_features(vmhook_module INTERFACE cxx_std_26)
    target_compile_options(vmhook_module INTERFACE -freflection -fmodules
        "-fmodule-mapper=${_vmhook_module_mapper}")
    target_include_directories(vmhook_module SYSTEM INTERFACE
        "${_vmhook_module_dir}/stdlib-overlay")
    target_link_libraries(vmhook_module INTERFACE "${_vmhook_module_archive}")
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
    if(Python3_Interpreter_FOUND)
        add_test(NAME vmhook.source_contract
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_source.py")
    endif()
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(FILES include/vmhook/vmhook.hpp DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/vmhook")
# Module CMIs are compiler/flag dependent. Install source, never cached CMIs.
install(FILES modules/vmhook.ixx modules/vmhook.windows.ixx DESTINATION "${CMAKE_INSTALL_DATADIR}/vmhook/modules")
install(FILES include/vmhook/detail/windows.hpp DESTINATION "${CMAKE_INSTALL_DATADIR}/vmhook/include/vmhook/detail")
install(FILES tools/build_module.py tools/make_header.py DESTINATION "${CMAKE_INSTALL_DATADIR}/vmhook/tools")
install(TARGETS vmhook EXPORT vmhookTargets)
install(EXPORT vmhookTargets NAMESPACE vmhook:: FILE vmhookTargets.cmake
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vmhook")
configure_package_config_file(cmake/vmhookConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/vmhookConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vmhook")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/vmhookConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/vmhookConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/vmhookConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vmhook")
