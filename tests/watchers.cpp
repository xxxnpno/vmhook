#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
import vmhook.windows;
#else
#include <vmhook/vmhook.hpp>
#endif

namespace
{
    alignas(8) volatile vmhook::windows::LONG watched{};
    alignas(8) volatile vmhook::windows::LONG foreign{};
}

int main()
{
    int failures{};
    const auto check = [&](bool condition, const char* message)
    {
        if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
    };
    std::atomic<unsigned> hits{};
    std::atomic<bool> released{};
    std::thread writer([&]
    {
        while (!released.load(std::memory_order_acquire)) { std::this_thread::yield(); }
        watched = 123;
    });

    const auto address = reinterpret_cast<std::uint64_t>(const_cast<vmhook::windows::LONG*>(&watched));
    const auto foreign_address = reinterpret_cast<std::uint64_t>(const_cast<vmhook::windows::LONG*>(&foreign));
    const auto bits = vmhook::os::detail_dr::build_dr7(0,
        vmhook::os::data_breakpoint_kind::write, vmhook::os::data_breakpoint_length::four_bytes);
    const auto foreign_bits = vmhook::os::detail_dr::build_dr7(1,
        vmhook::os::data_breakpoint_kind::write, vmhook::os::data_breakpoint_length::four_bytes);
    auto configuration = std::make_unique<vmhook::detail::dr_configuration>();
    configuration->address = address;
    configuration->bits = bits;
    {
        std::lock_guard lock{vmhook::detail::dr_mutex};
        check(vmhook::detail::ensure_dr_handler_installed(), "install a real Windows VEH");
        vmhook::detail::dr_slots[0].configuration.store(configuration.get(), std::memory_order_release);
        vmhook::detail::dr_slots[0].callback = std::make_shared<vmhook::detail::dr_callback_t>(
            [&](const void* pointer) { if (pointer == const_cast<vmhook::windows::LONG*>(&watched)) { ++hits; } });
        vmhook::detail::dr_slots[0].in_use.store(true, std::memory_order_release);
        vmhook::detail::dr_slots[0].notifying.store(true, std::memory_order_release);
        const auto applied = vmhook::detail::refresh_thread_drs(0, address, bits);
        check(applied.complete() && applied.applied >= 1, "arm the suspended writer thread");
    }

    // Install an unrelated breakpoint in another hardware slot, then attempt
    // to replace it through VMHook. Its address and control bits must survive.
    const auto foreign_installed = vmhook::os::detail_dr::for_each_thread([&](vmhook::windows::HANDLE thread)
    {
        vmhook::windows::CONTEXT context{};
        context.ContextFlags = vmhook::windows::context_debug_registers;
        if (!vmhook::windows::GetThreadContext(thread, &context)) { return false; }
        context.Dr1 = foreign_address;
        context.Dr7 |= foreign_bits;
        return vmhook::windows::SetThreadContext(thread, &context) != false;
    });
    check(foreign_installed.complete(), "install the foreign-slot fixture");
    const auto rejected = vmhook::detail::refresh_thread_drs(1, address, foreign_bits);
    check(rejected.failed >= 1 && !rejected.complete(), "refuse an occupied foreign slot");
    const auto preserved = vmhook::os::detail_dr::for_each_thread([&](vmhook::windows::HANDLE thread)
    {
        vmhook::windows::CONTEXT context{};
        context.ContextFlags = vmhook::windows::context_debug_registers;
        return vmhook::windows::GetThreadContext(thread, &context)
            && context.Dr1 == foreign_address
            && (context.Dr7 & vmhook::detail::dr_mask(1)) == foreign_bits;
    });
    check(preserved.complete(), "preserve the foreign address and DR7 control bits");

    // Exercise the handler's ownership decision directly: foreign DR6 causes
    // and CPU single-step status must continue to their own handler.
    vmhook::windows::EXCEPTION_POINTERS exception{};
    std::remove_pointer_t<decltype(exception.ExceptionRecord)> record{};
    vmhook::windows::CONTEXT context{};
    record.ExceptionCode = vmhook::windows::exception_single_step;
    exception.ExceptionRecord = &record;
    exception.ContextRecord = &context;
    context.Dr0 = foreign_address;
    context.Dr7 = bits;
    context.Dr6 = 1;
    check(vmhook::detail::dr_exception_handler(&exception) == vmhook::windows::exception_continue_search
          && context.Dr6 == 1 && context.Dr0 == foreign_address,
          "ignore a foreign breakpoint trap without consuming its status");
    context.Dr0 = address;
    context.Dr6 = 1 | (std::uint64_t{1} << 14);
    check(vmhook::detail::dr_exception_handler(&exception) == vmhook::windows::exception_continue_search
          && context.Dr6 == (std::uint64_t{1} << 14), "preserve single-step status beside an owned trap");
    const auto before_write = hits.load();
    released.store(true, std::memory_order_release);
    writer.join();
    check(watched == 123 && hits.load() == before_write + 1,
          "an actual hardware write delivers one owned notification");
    vmhook::detail::dr_slots[0].notifying.store(false, std::memory_order_release);
    vmhook::detail::clear_thread_drs(0);
    vmhook::detail::dr_slots[0].configuration.store(nullptr, std::memory_order_release);
    vmhook::detail::dr_slots[0].in_use.store(false, std::memory_order_release);
    return failures ? 1 : 0;
}
