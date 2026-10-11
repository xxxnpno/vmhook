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
    const auto check = [&](bool value, const char* name)
    { if (!value) { std::cerr << "FAIL: " << name << '\n'; ++failures; } };
    auto* const method = reinterpret_cast<vmhook::hotspot::method*>(0x1000);
    auto* const identity = reinterpret_cast<const vmhook::hotspot::const_method*>(0x2000);
    vmhook::hotspot::hook_dispatch_snapshot snapshot;
    std::vector<unsigned> order;
    const auto add = [&](unsigned id, auto callback, vmhook::hook_recursion recursion = vmhook::hook_recursion::suppress_same_observer)
    {
        vmhook::hotspot::hooked_method entry;
        entry.method = method; entry.const_method_identity = identity;
        entry.observer = std::make_shared<vmhook::hotspot::method_observer_control>();
        entry.observer->id = id; entry.observer->options.recursion = recursion;
        entry.detour = std::make_shared<vmhook::hotspot::hook_callback_t>(std::move(callback));
        snapshot.methods.push_back(std::move(entry));
    };
    add(1, [&](auto*, auto*, auto*) { order.push_back(1); });
    add(2, [&](auto*, auto*, auto* slot) { order.push_back(2); slot->cancel = true; slot->retval = 73; });
    add(3, [&](auto*, auto*, auto*) { order.push_back(3); });
    vmhook::hotspot::return_slot slot{};
    vmhook::hotspot::dispatch_method_observers(snapshot, method, identity, nullptr, nullptr, &slot);
    check(order == std::vector<unsigned>{1, 2} && slot.cancel && slot.retval == 73,
          "first forced return wins and skips later observers");
    order.clear(); slot = {};
    snapshot.methods[1].observer->enabled.store(false);
    vmhook::hotspot::dispatch_method_observers(snapshot, method, identity, nullptr, nullptr, &slot);
    check(order == std::vector<unsigned>{1, 3} && !slot.cancel,
          "disabled observers are skipped in an old immutable snapshot");
    order.clear(); slot = {};
    vmhook::hotspot::dispatch_method_observers(snapshot, method, nullptr, nullptr, nullptr, &slot);
    check(order.empty(), "metadata identity mismatch never invokes an observer");

    snapshot.methods.clear();
    unsigned recursive_calls{};
    add(4, [&](auto*, auto*, auto*)
    {
        ++recursive_calls;
        vmhook::hotspot::return_slot nested{};
        vmhook::hotspot::dispatch_method_observers(snapshot, method, identity, nullptr, nullptr, &nested);
    });
    vmhook::hotspot::dispatch_method_observers(snapshot, method, identity, nullptr, nullptr, &slot);
    check(recursive_calls == 1 && vmhook::hook_resources().suppressed_recursions == 1,
          "same observer recursion is suppressed without affecting other threads");
    snapshot.methods.front().observer->options.recursion = vmhook::hook_recursion::allow;
    recursive_calls = 0;
    vmhook::hotspot::dispatch_method_observers(snapshot, method, identity, nullptr, nullptr, &slot);
    check(recursive_calls == 64 && vmhook::hook_callbacks_idle(),
          "explicit recursion has a finite depth and restores callback state");
    vmhook::call_without_hooks([&]
    {
        vmhook::hotspot::dispatch_method_observers(snapshot, method, identity, nullptr, nullptr, &slot);
        check(recursive_calls == 64, "context suppression prevents observer recursion");
        std::thread other([&]
        {
            vmhook::hotspot::return_slot isolated{};
            vmhook::hotspot::dispatch_method_observers(snapshot, method, identity, nullptr, nullptr, &isolated);
        });
        other.join();
        check(recursive_calls == 128, "context suppression remains local to the calling OS thread");
    });
    try { vmhook::call_without_hooks([] { throw std::runtime_error{"test"}; }); }
    catch (const std::runtime_error&) {}
    check(vmhook::hotspot::suppress_method_observers == 0,
          "context suppression unwinds on C++ exceptions");

    snapshot.methods.clear(); order.clear(); slot = {};
    add(5, [&](auto*, auto*, auto* result) { result->cancel = true; result->retval = 19; throw 5; });
    add(6, [&](auto*, auto*, auto*) { order.push_back(6); });
    vmhook::hotspot::dispatch_method_observers(snapshot, method, identity, nullptr, nullptr, &slot);
    check(order == std::vector<unsigned>{6} && !slot.cancel && slot.retval == 0
          && vmhook::hook_resources().callback_failures == 1,
          "callback failures discard their forced return and permit subsequent observers");

    // An entered callback can finish after explicit removal, while the next
    // dispatch skips it. Its captures must never be destroyed under the lock.
    snapshot.methods.clear();
    std::atomic<unsigned> destroyed{};
    std::atomic<bool> destruction_under_lock{};
    std::latch entered{1}, leave{1};
    auto capture = std::shared_ptr<int>(new int{7}, [&](int* value)
    {
        if (vmhook::hotspot::g_hooked_methods_mutex.try_lock())
        { vmhook::hotspot::g_hooked_methods_mutex.unlock(); }
        else { destruction_under_lock.store(true); }
        delete value; ++destroyed;
    });
    add(7, [capture, &entered, &leave](auto*, auto*, auto*)
    { if (*capture == 7) { entered.count_down(); leave.wait(); } });
    capture.reset();
    {
        std::lock_guard lock{vmhook::hotspot::g_hooked_methods_mutex};
        vmhook::hotspot::g_hooked_methods = snapshot.methods;
        vmhook::hotspot::g_hook_dispatch_snapshots.reserve(4);
        auto published = std::make_unique<vmhook::hotspot::hook_dispatch_snapshot>(std::move(snapshot));
        vmhook::hotspot::publish_hook_dispatch_locked(std::move(published));
    }
    std::thread in_flight([&]
    {
        vmhook::hotspot::hook_dispatch_reader reader;
        vmhook::hotspot::return_slot own_slot{};
        vmhook::hotspot::dispatch_method_observers(*reader.snapshot, method, identity, nullptr, nullptr, &own_slot);
    });
    entered.wait();
    check(!vmhook::hook_callbacks_idle(), "entered readers are visible to quiescence diagnostics");
    check(vmhook::remove_hook(7) && !vmhook::remove_hook(7) && !vmhook::remove_hook(0),
          "persistent observer removal is explicit and idempotent");
    check(destroyed.load() == 0 && vmhook::hook_resources().active_observers == 0,
          "removal retains an active callback while disabling future entries");
    leave.count_down(); in_flight.join();
    std::vector<std::unique_ptr<vmhook::hotspot::hook_dispatch_snapshot>> retired;
    retired.reserve(4);
    {
        std::lock_guard lock{vmhook::hotspot::g_hooked_methods_mutex};
        vmhook::hotspot::collect_hook_dispatch_locked(retired);
    }
    retired.clear();
    check(destroyed.load() == 1 && !destruction_under_lock.load() && vmhook::hook_callbacks_idle(),
          "retired captures are destroyed outside registry locks after readers drain");
    check(vmhook::hook_resources().dll_must_remain_loaded,
          "callback quiescence never authorizes unloading published executable code");
    return failures ? 1 : 0;
}
