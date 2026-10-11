#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
import vmhook.windows;
#else
#include <vmhook/vmhook.hpp>
#endif

namespace
{
    int failures{};

    void check(bool condition, const char* message)
    {
        if (!condition)
        {
            std::cerr << "FAIL: " << message << '\n';
            ++failures;
        }
    }
}

int main()
{
    // A malformed VM codec must refuse instead of shifting by an invalid
    // amount, wrapping an address, or truncating a different pointer.
    constexpr std::uint64_t base{0x1000};
    check(reinterpret_cast<std::uintptr_t>(vmhook::hotspot::narrow_decode(base, 3, 2)) == base + 16,
          "decode a valid narrow pointer");
    check(vmhook::hotspot::narrow_encode(base, 3, base + 16) == 2,
          "encode a valid narrow pointer");
    check(vmhook::hotspot::narrow_decode(base, 3, 0) == nullptr, "preserve a null narrow pointer");
    check(vmhook::hotspot::narrow_decode(base, 64, 1) == nullptr, "reject an invalid decode shift");
    check(vmhook::hotspot::narrow_decode((std::numeric_limits<std::uint64_t>::max)() - 3, 3, 1) == nullptr,
          "reject a decoded address overflow");
    check(vmhook::hotspot::narrow_encode(base, 64, base + 16) == 0, "reject an invalid encode shift");
    check(vmhook::hotspot::narrow_encode(base, 3, base - 8) == 0, "reject a pointer below the base");
    check(vmhook::hotspot::narrow_encode(base, 3, base + 9) == 0, "reject an unaligned pointer");
    check(vmhook::hotspot::narrow_encode(0, 0, std::uint64_t{1} << 32) == 0,
          "reject a truncated narrow pointer");

    // Verify the real Windows memory primitives used to inspect foreign VM
    // data: boundary crossing into a protected page must refuse safely.
    const std::size_t page = vmhook::os::page_size();
    auto* memory = static_cast<unsigned char*>(
        vmhook::windows::VirtualAlloc(nullptr, page * 2, vmhook::windows::mem_commit | vmhook::windows::mem_reserve, vmhook::windows::page_readwrite));
    if (!memory)
    {
        std::cerr << "VirtualAlloc failed\n";
        return 1;
    }

    std::uint32_t expected{0x12345678};
    std::uint32_t actual{};
    check(vmhook::os::safe_write(memory, &expected, sizeof expected), "write a valid region");
    check(vmhook::os::safe_read(&actual, memory, sizeof actual) && actual == expected,
          "read back the written bytes");
    check(!vmhook::os::safe_read(nullptr, memory, sizeof actual), "reject null destination");
    check(!vmhook::os::safe_read(&actual, nullptr, sizeof actual), "reject null source");
    check(!vmhook::os::safe_read(&actual, memory, 0), "reject an empty read");
    check(!vmhook::os::safe_read(&actual, memory, std::numeric_limits<std::size_t>::max()),
          "reject an overflowing range");

    vmhook::windows::DWORD old_protection{};
    check(vmhook::windows::VirtualProtect(memory + page, page, vmhook::windows::page_noaccess, &old_protection) != 0,
          "protect the second page");
    check(!vmhook::os::safe_read(&actual, memory + page, sizeof actual),
          "refuse a read of inaccessible memory");
    check(!vmhook::os::safe_read(&actual, memory + page - 2, sizeof actual),
          "refuse a read across a protected boundary");
    check(!vmhook::os::safe_write(memory + page, &expected, sizeof expected),
          "refuse a write to inaccessible memory");
    check(vmhook::os::query_region(memory).readable, "recognize readable memory");
    check(!vmhook::os::query_region(memory + page).readable, "recognize inaccessible memory");
    vmhook::windows::VirtualFree(memory, 0, vmhook::windows::mem_release);
    check(!vmhook::os::safe_read(&actual, memory, sizeof actual), "refuse freed memory");

    // This executable has no JVM. The library must refuse metadata lookup and
    // native-thread adoption without dereferencing missing VM exports.
    check(vmhook::os::find_jvm_module() == nullptr, "standalone fixture has no JVM");
    check(vmhook::find_class("java/lang/Object") == nullptr, "refuse lookup without a JVM");
    check(!vmhook::hotspot::ensure_current_java_thread(), "refuse unattached native thread");
    return failures ? 1 : 0;
}
