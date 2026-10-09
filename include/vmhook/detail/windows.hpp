// Windows SDK boundary, compiled as a GNU C++ header unit for the module.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>


namespace vmhook::windows
{
    using ::BOOL;
    using ::CONTEXT;
    using ::DWORD;
    using ::DWORD64;
    using ::EXCEPTION_POINTERS;
    using ::HANDLE;
    using ::HMODULE;
    using ::IMAGE_DEBUG_DIRECTORY;
    using ::IMAGE_DOS_HEADER;
    using ::IMAGE_NT_HEADERS64;
    using ::IMAGE_SECTION_HEADER;
    using ::LONG;
    using ::MEMORY_BASIC_INFORMATION;
    using ::PVOID;
    using ::RUNTIME_FUNCTION;
    using ::SIZE_T;
    using ::SYSTEM_INFO;
    using ::THREADENTRY32;
    using ::ULONG;

    using ::AddVectoredExceptionHandler;
    using ::CloseHandle;
    using ::CreateToolhelp32Snapshot;
    using ::DuplicateHandle;
    using ::FlushInstructionCache;
    using ::GetCurrentProcess;
    using ::GetCurrentProcessId;
    using ::GetCurrentThreadId;
    using ::GetModuleHandleA;
    using ::GetModuleHandleW;
    using ::GetModuleHandleExW;
    using ::GetModuleFileNameA;
    using ::GetModuleFileNameW;
    using ::GetEnvironmentVariableA;
    using ::GetEnvironmentVariableW;
    using ::GetProcAddress;
    using ::GetSystemInfo;
    using ::GetThreadContext;
    using ::GetThreadId;
    using ::OpenThread;
    using ::ReadProcessMemory;
    using ::RemoveVectoredExceptionHandler;
    using ::ResumeThread;
    using ::RtlAddFunctionTable;
    using ::RtlDeleteFunctionTable;
    using ::SetThreadContext;
    using ::SuspendThread;
    using ::TerminateProcess;
    using ::Thread32First;
    using ::Thread32Next;
    using ::VirtualAlloc;
    using ::VirtualFree;
    using ::VirtualProtect;
    using ::VirtualQuery;
    using ::WaitForSingleObject;
    using ::WriteProcessMemory;

    inline constexpr DWORD page_noaccess{ PAGE_NOACCESS };
    inline constexpr auto image_dos_signature{ IMAGE_DOS_SIGNATURE };
    inline constexpr auto image_nt_signature{ IMAGE_NT_SIGNATURE };
    inline constexpr auto image_file_machine_amd64{ IMAGE_FILE_MACHINE_AMD64 };
    inline constexpr auto image_nt_optional_hdr64_magic{ IMAGE_NT_OPTIONAL_HDR64_MAGIC };
    inline constexpr auto image_directory_entry_debug{ IMAGE_DIRECTORY_ENTRY_DEBUG };
    inline constexpr auto image_debug_type_codeview{ IMAGE_DEBUG_TYPE_CODEVIEW };
    inline constexpr DWORD page_readonly{ PAGE_READONLY };
    inline constexpr DWORD page_readwrite{ PAGE_READWRITE };
    inline constexpr DWORD page_writecopy{ PAGE_WRITECOPY };
    inline constexpr DWORD page_execute{ PAGE_EXECUTE };
    inline constexpr DWORD page_execute_read{ PAGE_EXECUTE_READ };
    inline constexpr DWORD page_execute_readwrite{ PAGE_EXECUTE_READWRITE };
    inline constexpr DWORD page_execute_writecopy{ PAGE_EXECUTE_WRITECOPY };
    inline constexpr DWORD page_guard{ PAGE_GUARD };
    inline constexpr DWORD mem_commit{ MEM_COMMIT };
    inline constexpr DWORD mem_free{ MEM_FREE };
    inline constexpr DWORD mem_release{ MEM_RELEASE };
    inline constexpr DWORD mem_reserve{ MEM_RESERVE };
    inline constexpr DWORD snapshot_threads{ TH32CS_SNAPTHREAD };
    inline constexpr DWORD thread_get_context{ THREAD_GET_CONTEXT };
    inline constexpr DWORD thread_set_context{ THREAD_SET_CONTEXT };
    inline constexpr DWORD thread_suspend_resume{ THREAD_SUSPEND_RESUME };
    inline constexpr DWORD thread_query_information{ THREAD_QUERY_INFORMATION };
    inline constexpr DWORD thread_query_limited_information{ THREAD_QUERY_LIMITED_INFORMATION };
    inline constexpr DWORD thread_all_access{ THREAD_ALL_ACCESS };
    inline constexpr DWORD synchronize{ SYNCHRONIZE };
    inline constexpr DWORD wait_object_0{ WAIT_OBJECT_0 };
    inline constexpr DWORD duplicate_same_access{ DUPLICATE_SAME_ACCESS };
    inline constexpr DWORD module_handle_from_address{ GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS };
    inline constexpr DWORD module_handle_pin{ GET_MODULE_HANDLE_EX_FLAG_PIN };
    inline constexpr DWORD error_write_fault{ ERROR_WRITE_FAULT };
    inline constexpr DWORD context_control{ CONTEXT_CONTROL };
    inline constexpr DWORD context_debug_registers{ CONTEXT_DEBUG_REGISTERS };
    inline constexpr DWORD exception_single_step{ EXCEPTION_SINGLE_STEP };
    inline constexpr LONG exception_continue_execution{ EXCEPTION_CONTINUE_EXECUTION };
    inline constexpr LONG exception_continue_search{ EXCEPTION_CONTINUE_SEARCH };
    inline constexpr auto thread_owner_process_id_offset{
        __builtin_offsetof(THREADENTRY32, th32OwnerProcessID) };
    inline const HANDLE invalid_handle_value{ INVALID_HANDLE_VALUE };
}
