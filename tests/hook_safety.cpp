#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
import vmhook.windows;
#else
#include <latch>
#include <vmhook/vmhook.hpp>
#endif

int main()
{
    int failures{};
    const auto check = [&](bool condition, const char* description)
    {
        if (!condition)
        {
            std::cerr << "FAIL: " << description << '\n';
            ++failures;
        }
    };

    // An in-flight callback must finish while an administrator holds the
    // installation mutex and removes its registration. Its captured objects
    // remain alive until that reader leaves and are destroyed outside the lock.
    std::atomic<unsigned> callback_calls{};
    std::atomic<unsigned> capture_destructions{};
    std::atomic<bool> destruction_under_lock{};
    auto capture = std::shared_ptr<int>(new int{7}, [&](int* value)
    {
        if (vmhook::hotspot::g_hooked_methods_mutex.try_lock())
        { vmhook::hotspot::g_hooked_methods_mutex.unlock(); }
        else { destruction_under_lock.store(true); }
        delete value;
        ++capture_destructions;
    });
    auto snapshot = std::make_unique<vmhook::hotspot::hook_dispatch_snapshot>();
    snapshot->methods.emplace_back();
    snapshot->methods.back().detour = std::make_shared<vmhook::hotspot::hook_callback_t>(
        [capture, &callback_calls](vmhook::hotspot::frame*, vmhook::hotspot::java_thread*,
                                  vmhook::hotspot::return_slot*)
        { if (*capture == 7) { ++callback_calls; } });
    capture.reset();
    {
        std::lock_guard lock{vmhook::hotspot::g_hooked_methods_mutex};
        vmhook::hotspot::g_hook_dispatch_snapshots.reserve(2);
        vmhook::hotspot::publish_hook_dispatch_locked(std::move(snapshot));
    }
    std::latch acquired{1}, removed{1}, invoked{1};
    std::thread dispatching([&]
    {
        const vmhook::hotspot::hook_dispatch_reader reader{};
        acquired.count_down();
        removed.wait();
        if (reader.snapshot && !reader.snapshot->methods.empty())
        { (*reader.snapshot->methods.front().detour)(nullptr, nullptr, nullptr); }
        invoked.count_down();
    });
    acquired.wait();
    std::vector<std::unique_ptr<vmhook::hotspot::hook_dispatch_snapshot>> retired_snapshots;
    retired_snapshots.reserve(2);
    {
        std::lock_guard lock{vmhook::hotspot::g_hooked_methods_mutex};
        vmhook::hotspot::g_hook_dispatch.store(nullptr, std::memory_order_seq_cst);
        vmhook::hotspot::collect_hook_dispatch_locked(retired_snapshots);
        check(retired_snapshots.empty() && capture_destructions.load() == 0,
              "removal retains the capture of an active dispatch");
        removed.count_down();
        invoked.wait();
        check(callback_calls.load() == 1,
              "an active dispatch invokes its callback while the installation mutex is held");
    }
    dispatching.join();
    {
        std::lock_guard lock{vmhook::hotspot::g_hooked_methods_mutex};
        vmhook::hotspot::collect_hook_dispatch_locked(retired_snapshots);
    }
    retired_snapshots.clear();
    check(capture_destructions.load() == 1 && !destruction_under_lock.load(),
          "retired callback captures are released once, outside the installation mutex");

    const std::array<std::uint8_t, 4> vex{0xc5, 0xf8, 0x58, 0xc0};
    const std::array<std::uint8_t, 3> vzeroupper{0xc5, 0xf8, 0x77};
    const std::array<std::uint8_t, 1> unknown{0x62};
    const std::array<std::uint8_t, 2> truncated{0x48, 0xb8};
    const std::string_view mixed{"(IJIDI[Ljava/lang/Object;)I"};
    const auto first = vmhook::detail::interpreter_argument_layout(mixed, false, 0);
    const auto wide = vmhook::detail::interpreter_argument_layout(mixed, false, 1);
    const auto following = vmhook::detail::interpreter_argument_layout(mixed, false, 2);
    const auto array = vmhook::detail::interpreter_argument_layout(mixed, false, 5);
    check(first && first->slot == 1 && first->descriptor == "I",
          "instance parameter zero excludes the receiver");
    check(wide && wide->slot == 2 && wide->descriptor == "J"
          && following && following->slot == 4 && following->descriptor == "I",
          "long parameters occupy two JVM slots before the next declared parameter");
    check(array && array->slot == 8 && array->descriptor == "[Ljava/lang/Object;",
          "array parameters remain single full-oop slots after mixed wide primitives");
    check(!vmhook::detail::interpreter_argument_layout(mixed, false, -1)
          && !vmhook::detail::interpreter_argument_layout(mixed, false, 6)
          && !vmhook::detail::interpreter_argument_layout("([V)V", true, 0)
          && !vmhook::detail::interpreter_argument_layout("(I)Vgarbage", true, 0),
          "negative, past-parameter and malformed descriptors are refused");
    check(vmhook::detail::interpreter_argument_type_matches<std::int64_t>("J")
          && !vmhook::detail::interpreter_argument_type_matches<double>("J")
          && !vmhook::detail::interpreter_argument_type_matches<std::int32_t>("[I")
          && vmhook::detail::interpreter_argument_type_matches<void*>("[I"),
          "descriptor checks distinguish wide primitive and reference arguments");
    check(vmhook::hotspot::interpreter_instruction_size(vex.data(), vex.size()) == 0,
          "VEX instructions require a validated decoder");
    check(vmhook::hotspot::interpreter_instruction_size(vzeroupper.data(), vzeroupper.size()) == 3,
          "the exact HotSpot vzeroupper helper instruction is recognized");
    check(vmhook::hotspot::interpreter_instruction_size(unknown.data(), unknown.size()) == 0,
          "unknown encodings are refused");
    check(vmhook::hotspot::interpreter_instruction_size(truncated.data(), truncated.size()) == 0,
          "truncated immediate encodings are refused");

    auto* code = static_cast<std::uint8_t*>(
        vmhook::windows::VirtualAlloc(nullptr, 8192, vmhook::windows::mem_commit | vmhook::windows::mem_reserve, vmhook::windows::page_execute_readwrite));
    if (!code) { return 1; }
    check(vmhook::os::allocate_rwx(code, 4096, false) == nullptr,
          "an occupied strict allocation hint cannot fall back to another address");
    auto* const nearby = vmhook::hotspot::allocate_nearby_memory(code, 4096);
    const auto displacement = nearby ? static_cast<std::int64_t>(
        reinterpret_cast<std::uintptr_t>(nearby)) - static_cast<std::int64_t>(
        reinterpret_cast<std::uintptr_t>(code) + 5) : std::numeric_limits<std::int64_t>::max();
    check(nearby && displacement >= std::numeric_limits<std::int32_t>::min()
          && displacement <= std::numeric_limits<std::int32_t>::max(),
          "a trampoline reservation remains reachable by its five-byte relative jump");
    if (nearby) { vmhook::os::release(nearby, 4096); }
    const std::array<std::uint8_t, 24> normal_entry{
        0x50, 0x55, 0x48, 0x89, 0xe5, 0x41, 0x55,
        0x41, 0xc6, 0x87, 0x34, 0x12, 0, 0, 1,
        0x41, 0xc6, 0x87, 0x34, 0x12, 0, 0, 0, 0xc3};
    std::memset(code, 0xcc, 8192);
    std::memcpy(code, normal_entry.data(), normal_entry.size());
    check(vmhook::hotspot::find_hook_location(code) == code + 7,
          "a fixed frame and paired flag stores identify an instruction boundary");

    const std::array<std::uint8_t, 29> normal_entry_with_helper{
        0x50, 0x55, 0x48, 0x89, 0xe5, 0x41, 0x55,
        0x41, 0xc6, 0x87, 0x34, 0x12, 0, 0, 1,
        0xc5, 0xf8, 0x77, 0xc3, 0x90,
        0x41, 0xc6, 0x87, 0x34, 0x12, 0, 0, 0, 0xc3};
    std::memset(code, 0xcc, 8192);
    std::memcpy(code, normal_entry_with_helper.data(), normal_entry_with_helper.size());
    check(vmhook::hotspot::find_hook_location(code) == code + 7,
          "HotSpot's vzeroupper and embedded helper return preserve the paired-store proof");

    const std::array<std::uint8_t, 26> immediate_flag{
        0x50, 0x55, 0x48, 0x89, 0xe5, 0x41, 0x55,
        0x48, 0xb8, 0x41, 0xc6, 0x87, 0x34, 0x12, 0, 0, 1,
        0x41, 0xc6, 0x87, 0x34, 0x12, 0, 0, 0, 0xc3};
    std::memset(code, 0xcc, 8192);
    std::memcpy(code, immediate_flag.data(), immediate_flag.size());
    check(vmhook::hotspot::find_hook_location(code) == nullptr,
          "flag-store bytes inside a movabs immediate are not instructions");

    const std::array<std::uint8_t, 27> immediate_frame{
        0x48, 0xb8, 0x50, 0x55, 0x48, 0x89, 0xe5, 0x41, 0x55, 0x90,
        0x41, 0xc6, 0x87, 0x34, 0x12, 0, 0, 1,
        0x41, 0xc6, 0x87, 0x34, 0x12, 0, 0, 0, 0xc3};
    std::memset(code, 0xcc, 8192);
    std::memcpy(code, immediate_frame.data(), immediate_frame.size());
    check(vmhook::hotspot::find_hook_location(code) == nullptr,
          "frame-prologue bytes inside an immediate cannot authorize a hook");

    const std::array<std::uint8_t, 8> one{0xb8, 1, 0, 0, 0, 0xc3, 0x90, 0x90};
    const std::array<std::uint8_t, 8> two{0xb8, 2, 0, 0, 0, 0xc3, 0x90, 0x90};
    std::memcpy(code, one.data(), one.size());
    check(!vmhook::hotspot::replace_interpreter_code(code, two, one),
          "an expected-byte mismatch refuses a patch");
    check(std::memcmp(code, one.data(), one.size()) == 0,
          "a refused patch preserves code bytes");

    // A completed worker can remain enumerable while its caller owns a
    // handle. Its signalled kernel object must not prevent a code transaction.
    using create_thread_t = vmhook::windows::HANDLE(*)(void*, std::size_t,
        vmhook::windows::DWORD(*)(void*), void*, vmhook::windows::DWORD, vmhook::windows::DWORD*);
    const auto create_thread = reinterpret_cast<create_thread_t>(vmhook::os::get_proc_address(
        vmhook::os::find_loaded_module("kernel32.dll"), "CreateThread"));
    const auto retired = create_thread ? create_thread(nullptr, 0,
        +[](void*) -> vmhook::windows::DWORD { return 30115; }, nullptr, 0, nullptr) : nullptr;
    check(retired && vmhook::windows::WaitForSingleObject(retired, 5000)
          == vmhook::windows::wait_object_0, "retain a completed worker's kernel handle");
    if (retired)
    {
        check(vmhook::hotspot::replace_interpreter_code(code, one, two),
              "a retained terminated thread does not block code patching");
        check(vmhook::hotspot::replace_interpreter_code(code, two, one),
              "the patch can be restored with the terminated handle open");
        vmhook::windows::CloseHandle(retired);
    }

    std::atomic<bool> running{true};
    std::atomic<unsigned> bad_results{};
    std::thread executing([&]
    {
        using generated_function = int(*)();
        const auto function = reinterpret_cast<generated_function>(code);
        while (running.load(std::memory_order_relaxed))
        {
            const auto result = function();
            if (result != 1 && result != 2) { ++bad_results; }
        }
    });
    bool changed{};
    unsigned completed{};
    for (unsigned attempt = 0; attempt < 100; ++attempt)
    {
        if (vmhook::hotspot::replace_interpreter_code(code, changed ? two : one,
                                                     changed ? one : two))
        {
            changed = !changed;
            ++completed;
        }
    }
    running.store(false, std::memory_order_relaxed);
    executing.join();
    check(completed > 0, "patching can complete while another thread executes the code");
    check(bad_results.load() == 0, "the executing thread never observes a torn immediate");
    vmhook::windows::MEMORY_BASIC_INFORMATION region{};
    check(vmhook::windows::VirtualQuery(code, &region, sizeof region) != 0
          && region.Protect == vmhook::windows::page_execute_readwrite, "patching preserves original page protection");
    vmhook::windows::VirtualFree(code, 0, vmhook::windows::mem_release);
    return failures ? 1 : 0;
}
