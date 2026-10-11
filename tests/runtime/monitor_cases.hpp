// Included inside native_probe's fixture namespace.
auto check_monitor_locks(vmhook::native_thread_context& context, std::ostream& output) -> bool
{
    const auto object_field = native_fixture::static_members::get_field("monitor_object");
    const std::unique_ptr<native_fixture> object = object_field->get();
    if (!object || !object->get_instance()) { return false; }
    if (!vmhook::monitor_locks_supported())
    { return conversion_refused([&] { vmhook::monitor_lock lock{object}; }); }
    const auto field = [](const char* name) { return native_fixture::static_members::get_field(name); };
    const auto read = [&](const char* name) -> std::int32_t { return field(name)->get(); };
    const auto write = [&](const char* name, std::int32_t value) { field(name)->set(value); };
    const auto held = [&] { return static_cast<bool>(object->get_method("monitor_held", "()Z")->call()); };
    const auto await = [&](const char* name, std::int32_t expected)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (read(name) == expected) { return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return false;
    };
    output << "monitor_stage=start" << std::endl;
    if (held()) { return false; }
    {
        vmhook::monitor_lock lock{object};
        output << "monitor_stage=locked held " << held() << " depth " << vmhook::detail::native_backend::active_context_lifetime()->operation_depth << std::endl;
        if (!lock.owns_lock() || !held() || context.close()) { return false; }
        {
            vmhook::monitor_lock recursive{object};
            if (!held()) { return false; }
        }
        output << "monitor_stage=recursive_done" << std::endl;
        if (!held()) { return false; }
        object->get_field("value")->set(std::int32_t{8000});
        write("monitor_request", 1);
        if (!await("monitor_attempting", 1)) { return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds{15});
        if (read("monitor_acquired") != 0) { return false; }
        output << "monitor_stage=java_blocked" << std::endl;
        native_fixture::collect();
        if (!held() || object->value() != 8000 || read("monitor_acquired") != 0) { return false; }
    }
    output << "monitor_stage=released" << std::endl;
    if (held() || !await("monitor_acquired", 1) || read("monitor_seen") != 8000 || object->value() != 8001)
    { return false; }
    if (!await("monitor_request", 0)) { return false; }
    output << "monitor_stage=java_acquired" << std::endl;
    write("monitor_request", 2);
    if (!await("monitor_inside", 1)) { return false; }
    output << "monitor_stage=java_inside" << std::endl;
    std::atomic<bool> started{}, acquired{}, result{};
    std::thread competitor{[&]
    {
        try
        {
            vmhook::native_thread_context other{thread_name()};
            if (!other) { return; }
            const std::unique_ptr<native_fixture> same = native_fixture::static_members::get_field("monitor_object")->get();
            started.store(true, std::memory_order_release);
            {
                vmhook::monitor_lock lock{same};
                acquired.store(true, std::memory_order_release);
                result.store(same->value() == 9001
                    && static_cast<bool>(same->get_method("monitor_held", "()Z")->call())
                    && !other.close(), std::memory_order_release);
                same->get_field("value")->set(std::int32_t{9002});
            }
            result.store(result.load(std::memory_order_acquire) && other.close(), std::memory_order_release);
        }
        catch (...) { result.store(false, std::memory_order_release); }
    }};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (!started.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
    { std::this_thread::sleep_for(std::chrono::milliseconds{1}); }
    std::this_thread::sleep_for(std::chrono::milliseconds{15});
    bool blocked = started.load(std::memory_order_acquire) && !acquired.load(std::memory_order_acquire);
    // The blocked DLL thread must use the VM's real blocked-state transition,
    // otherwise this collection would stall waiting for that native contender.
    native_fixture::collect();
    write("monitor_release", 1);
    competitor.join();
    output << "monitor_stage=competitor_joined blocked " << blocked << " acquired " << acquired.load() << " result " << result.load() << std::endl;
    if (!blocked || !acquired.load(std::memory_order_acquire) || !result.load(std::memory_order_acquire)
        || object->value() != 9002 || held()) { return false; }
    try
    {
        vmhook::with_monitor(object, [] { throw std::bad_alloc{}; });
        return false;
    }
    catch (const std::bad_alloc&) {}
    if (held() || vmhook::with_monitor(object, [&] { return held() ? 42 : -1; }) != 42) { return false; }
    output << "monitor_stage=unwind_done" << std::endl;
    const auto java_null = std::make_unique<native_fixture>(nullptr);
    try { vmhook::monitor_lock lock{java_null}; return false; }
    catch (const vmhook::exception& error)
    { if (error.code() != vmhook::error_code::null_reference) { return false; } }
    struct heap_embedded final
    {
        vmhook::monitor_lock lock;
        explicit heap_embedded(const std::unique_ptr<native_fixture>& value) : lock{value} {}
    };
    return conversion_refused([&] { auto unsafe = std::make_unique<heap_embedded>(object); }) && !held();
}
