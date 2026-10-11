# The package installs module sources. Every consuming build produces its own
# GNU CMIs and archive using its compiler, global flags and configuration.
include(CheckCXXSourceCompiles)
set(VMHOOK_STD_MODULE_SOURCE "" CACHE FILEPATH "Optional override for the compiler's std module source")
set(VMHOOK_MODULE_COMPILE_OPTIONS "" CACHE STRING "Additional options shared by the VMHook module and its importers")

function(vmhook_enable_module source_dir)
    # GNU header units are orchestrated explicitly; CMake scanning cannot
    # represent their imports. Targets created after find_package inherit it.
    set(CMAKE_CXX_SCAN_FOR_MODULES OFF PARENT_SCOPE)
    if(TARGET vmhook::module)
        return()
    endif()
    if(CMAKE_VERSION VERSION_LESS 4.0)
        message(FATAL_ERROR "VMHOOK_BUILD_MODULE requires CMake 4.0 or newer for the GNU C++26 language mode.")
    endif()
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
        message(FATAL_ERROR "VMHOOK_BUILD_MODULE requires GNU G++ 16 or newer with -freflection support.")
    endif()
    find_package(Python3 3.10 QUIET COMPONENTS Interpreter)
    if(NOT Python3_Interpreter_FOUND)
        message(FATAL_ERROR "VMHOOK_BUILD_MODULE requires Python 3.10 or newer.")
    endif()
    set(_vmhook_saved_try_compile_target_type "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
    set(_vmhook_saved_required_flags "${CMAKE_REQUIRED_FLAGS}")
    set(_vmhook_saved_standard "${CMAKE_CXX_STANDARD}")
    set(CMAKE_CXX_STANDARD 26)
    string(JOIN " " _vmhook_probe_options ${VMHOOK_MODULE_COMPILE_OPTIONS})
    set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} ${_vmhook_probe_options} -freflection")
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

    set(_vmhook_module_dir "${CMAKE_CURRENT_BINARY_DIR}/vmhook-module/$<CONFIG>")
    set(_vmhook_module_archive "${_vmhook_module_dir}/libvmhook.a")
    set(_vmhook_module_mapper "${_vmhook_module_dir}/module.mapper")
    separate_arguments(_vmhook_flags NATIVE_COMMAND "${CMAKE_CXX_FLAGS}")
    set(_vmhook_module_flags)
    foreach(_vmhook_flag IN LISTS VMHOOK_MODULE_COMPILE_OPTIONS)
        list(APPEND _vmhook_module_flags "--flag=${_vmhook_flag}")
    endforeach()
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
            "${_vmhook_module_dir}/gcm/vmhook.gcm" "${_vmhook_module_dir}/sdk/vmhook/detail/windows.hpp" "${_vmhook_module_dir}/build-info.json"
        COMMAND "${Python3_EXECUTABLE}" "${source_dir}/tools/build_module.py"
            --compiler "${CMAKE_CXX_COMPILER}" --ar "${CMAKE_AR}"
            --source-dir "${source_dir}" --build-dir "${_vmhook_module_dir}"
            ${_vmhook_module_flags}
        DEPENDS "${source_dir}/modules/vmhook.ixx" "${source_dir}/modules/vmhook.windows.ixx"
            "${source_dir}/include/vmhook/detail/windows.hpp"
            "${source_dir}/tools/build_module.py" "${source_dir}/tools/make_header.py"
            "${CMAKE_CXX_COMPILER}" "${CMAKE_AR}"
        COMMAND_EXPAND_LISTS VERBATIM)
    add_custom_target(vmhook_module_build ALL DEPENDS "${_vmhook_module_archive}" "${_vmhook_module_mapper}")
    add_library(vmhook_module INTERFACE)
    add_library(vmhook::module ALIAS vmhook_module)
    add_dependencies(vmhook_module vmhook_module_build)
    target_compile_features(vmhook_module INTERFACE cxx_std_26)
    target_compile_options(vmhook_module INTERFACE ${VMHOOK_MODULE_COMPILE_OPTIONS} -std=c++26 -freflection -fmodules
        "-fmodule-mapper=${_vmhook_module_mapper}")
    target_include_directories(vmhook_module SYSTEM INTERFACE
        "${_vmhook_module_dir}/stdlib-overlay")
    target_link_libraries(vmhook_module INTERFACE "${_vmhook_module_archive}")

endfunction()
