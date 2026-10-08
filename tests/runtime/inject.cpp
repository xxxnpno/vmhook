#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv)
{
    if (argc != 3) { return 1; }
    char* end{};
    const auto pid = std::strtoul(argv[1], &end, 10);
    if (!pid || !end || *end) { return 2; }
    HANDLE process = ::OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                  PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                  FALSE, static_cast<DWORD>(pid));
    if (!process) { return 3; }
    const std::size_t bytes = std::strlen(argv[2]) + 1;
    void* path = ::VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!path) { ::CloseHandle(process); return 4; }
    SIZE_T written{};
    if (!::WriteProcessMemory(process, path, argv[2], bytes, &written) || written != bytes)
    {
        ::VirtualFreeEx(process, path, 0, MEM_RELEASE);
        ::CloseHandle(process);
        return 5;
    }
    const auto loader = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        ::GetProcAddress(::GetModuleHandleA("kernel32.dll"), "LoadLibraryA"));
    HANDLE thread = loader ? ::CreateRemoteThread(process, nullptr, 0, loader, path, 0, nullptr) : nullptr;
    if (!thread)
    {
        ::VirtualFreeEx(process, path, 0, MEM_RELEASE);
        ::CloseHandle(process);
        return 6;
    }
    const DWORD waited = ::WaitForSingleObject(thread, 10000);
    DWORD loaded{};
    const bool completed = waited == WAIT_OBJECT_0 && ::GetExitCodeThread(thread, &loaded);
    // LoadLibrary may still be reading the remote buffer after a timeout.
    if (completed) { ::VirtualFreeEx(process, path, 0, MEM_RELEASE); }
    ::CloseHandle(thread);
    ::CloseHandle(process);
    return completed && loaded ? 0 : 7;
}
