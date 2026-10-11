#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
import vmhook.windows;
#else
#include <vmhook/vmhook.hpp>
#include <fstream>
#endif

namespace
{
    class fixture : public vmhook::object<fixture>
    {
    public:
        using vmhook::object<fixture>::object;
        static auto first() -> std::unique_ptr<vmhook::any_object>
        { return static_members::get_method("first")->call(); }
        static auto second() -> std::unique_ptr<vmhook::any_object>
        { return static_members::get_method("second")->call(); }
        static auto first_loader() -> std::unique_ptr<vmhook::any_object>
        { return static_members::get_method("first_loader")->call(); }
        static auto first_alive() -> bool
        { return static_members::get_method("first_alive")->call(); }
        static auto current_loader_matches(const std::unique_ptr<vmhook::any_object>& loader) -> bool
        { return static_members::get_method("current_loader_matches")->call(loader); }
        static auto drop_anchors() -> void
        { const auto result = static_members::get_method("drop_anchors")->call();
          if (!result.was_invoked() || result.threw()) { throw vmhook::exception{"Drop failed"}; } }
        static auto collect() -> void
        { const auto result = static_members::get_method("collect")->call();
          if (!result.was_invoked() || result.threw()) { throw vmhook::exception{"Collection failed"}; } }
    };
    class copy_one : public vmhook::object<copy_one>
    {
    public:
        using vmhook::object<copy_one>::object;
        auto value() const -> std::int32_t { return get_field("value")->get(); }
        auto add(std::int32_t amount) const -> std::int32_t { return get_method("add")->call(amount); }
        static auto total() -> std::int32_t { return static_members::get_field("total")->get(); }
    };
    class copy_two : public vmhook::object<copy_two>
    {
    public:
        using vmhook::object<copy_two>::object;
        static auto total() -> std::int32_t { return static_members::get_field("total")->get(); }
    };
    template<typename function_type>
    auto refused(function_type&& function) -> bool
    {
        try { function(); }
        catch (const vmhook::exception&) { return true; }
        return false;
    }
    auto check(std::ostream& output, std::string_view name, bool correct) -> bool
    { output << name << '=' << (correct ? "pass" : "fail") << '\n'; output.flush(); return correct; }

    auto worker(void*) -> DWORD
    {
        char destination[4096]{};
        if (!::GetEnvironmentVariableA("VMHOOK_TEST_RESULT", destination, sizeof(destination))) { return 1; }
        std::ofstream output{destination};
        try
        {
            if (!check(output, "metadata", vmhook::register_class<fixture>("LifetimeFixture"))) { return 2; }
            vmhook::retained_object<copy_one> retained{};
            vmhook::retained_object<copy_one> retained_null{};
            std::unique_ptr<copy_one> expired{};
            {
                vmhook::native_thread_context source{"VMHook-lifetime-source"};
                if (!check(output, "native_attach", source.valid())) { return 3; }
                auto first = fixture::first();
                auto second = fixture::second();
                if (!check(output, "ambiguous_name", vmhook::find_class("LifetimeFixture$Duplicate") == nullptr)) { return 4; }
                if (!check(output, "loader_binding", vmhook::register_class<copy_one>("LifetimeFixture$Duplicate", first)
                    && vmhook::register_class<copy_two>("LifetimeFixture$Duplicate", second)
                    && copy_one::total() == 141 && copy_two::total() == 173
                    && first->is<copy_one>() && !first->is<copy_two>()
                    && vmhook::borrowed<vmhook::any_object>{first->get_instance(), first->get_native_root()}.is<copy_one>()
                    && !vmhook::borrowed<vmhook::any_object>{first->get_instance(), first->get_native_root()}.is<copy_two>())) { return 5; }
                auto created = vmhook::make_unique<copy_one>(std::int32_t{9});
                if (!check(output, "constructor_identity", created->value() == 9
                    && copy_one::total() == 150 && copy_two::total() == 173)) { return 6; }
                auto first_snapshot = vmhook::get_instances<copy_one>(64);
                fixture::collect();
                bool saw_first{}, saw_created{}, exact_first{!first_snapshot.empty()};
                for (const auto& object : first_snapshot)
                {
                    const auto value = object->value();
                    saw_first = saw_first || value == 41;
                    saw_created = saw_created || value == 9;
                    exact_first = exact_first && (value == 41 || value == 9);
                }
                if (!check(output, "loader_heap_snapshot", exact_first && saw_first && saw_created)) { return 6; }
                expired = std::make_unique<copy_one>(first->get_instance());
                expired->set_native_root(first->get_native_root());
                retained = vmhook::retain(expired);
                const auto resource_before = vmhook::root_resources();
                auto transient = vmhook::retain(expired);
                const auto resource_retained = vmhook::root_resources();
                transient.reset();
                const auto resource_released = vmhook::root_resources();
                vmhook::detail::flush_hook_roots();
                const auto resource_drained = vmhook::root_resources();
                for (std::size_t iteration{}; iteration < 64; ++iteration)
                { auto reused = vmhook::retain(expired); reused.reset(); }
                vmhook::detail::flush_hook_roots();
                const auto resource_reused = vmhook::root_resources();
                if (!check(output, "root_accounting", resource_before.context_active && resource_before.durable_ready
                    && resource_retained.durable_leases == resource_before.durable_leases + 1
                    && resource_retained.durable_occupied_slots == resource_before.durable_occupied_slots + 1
                    && resource_released.durable_leases == resource_before.durable_leases
                    && resource_released.durable_pending_releases == resource_before.durable_pending_releases + 1
                    && resource_drained.durable_occupied_slots == resource_before.durable_occupied_slots
                    && resource_drained.durable_pending_releases == 0
                    && resource_drained.durable_unreclaimed_slots == 0
                    && resource_reused.durable_allocated_slots == resource_drained.durable_allocated_slots
                    && resource_reused.durable_occupied_slots == resource_before.durable_occupied_slots
                    && resource_reused.durable_leases == resource_before.durable_leases
                    && resource_reused.durable_pending_releases == 0
                    && resource_drained.context_allocated_slots >= resource_drained.context_live_ticket_slots
                    && resource_drained.durable_internal_slots == 1)) { return 7; }
                auto java_null = std::make_unique<copy_one>(nullptr);
                retained_null = vmhook::retain(java_null);
                const std::unique_ptr<copy_one> empty{};
                if (!check(output, "null_contract", retained.is_same_object(expired) && !retained.is_same_object(java_null)
                    && retained_null.is_same_object(java_null) && retained_null.valid() && retained_null.acquire()
                    && !retained_null.acquire()->get_instance() && refused([&] { (void)vmhook::retain(empty); }))) { return 7; }
                auto loader = fixture::first_loader();
                source.set_context_class_loader(loader);
                if (!check(output, "context_loader", fixture::current_loader_matches(loader))) { return 8; }
                fixture::drop_anchors();
            }
            if (!check(output, "source_closed", refused([&] { (void)expired->get_instance(); })
                && refused([&] { (void)retained.acquire(); }) && !vmhook::root_resources().context_active)) { return 9; }
            ::Sleep(200);
            std::atomic<bool> correct{false};
            std::thread receiving([&]
            {
                try
                {
                    vmhook::native_thread_context target{"VMHook-lifetime-target"};
                    if (!target.valid()) { return; }
                    fixture::collect();
                    auto acquired = retained.acquire();
                    bool old_refused = refused([&] { (void)expired->get_instance(); });
                    bool alive = acquired && acquired->value() == 41 && acquired->add(1) == 42 && fixture::first_alive();
                    retained.reset();
                    fixture::collect();
                    alive = alive && acquired->value() == 41 && refused([&] { (void)retained.acquire(); });
                    correct.store(old_refused && alive);
                }
                catch (...) {}
            });
            receiving.join();
            if (!check(output, "cross_context_gc", correct.load())) { return 10; }
            vmhook::retained_object<copy_one> moved{std::move(retained_null)};
            if (!check(output, "released_resource", !retained_null.valid() && moved.acquire()
                && !moved.acquire()->get_instance())) { return 11; }
            // Created outside a local native context: these proxies own the
            // exact binding mirror rather than an expiring context root.
            auto old_field = copy_one::static_members::get_field("total");
            auto old_method = copy_one::static_members::get_method("total_value");
            {
                vmhook::native_thread_context next{"VMHook-lifetime-rebind"};
                auto other = vmhook::make_unique<copy_two>(std::int32_t{5});
                if (!check(output, "proxy_binding_lifetime", next.valid()
                    && vmhook::register_class<copy_one>("LifetimeFixture$Duplicate", other)
                    && copy_one::total() == 178 && static_cast<std::int32_t>(old_field->get()) == 150
                    && static_cast<std::int32_t>(old_method->call()) == 150)) { return 12; }
                auto rebound_snapshot = vmhook::get_instances<copy_one>(64);
                fixture::collect();
                bool saw_other{}, exact_other{!rebound_snapshot.empty()};
                for (const auto& object : rebound_snapshot)
                {
                    const auto value = object->value();
                    saw_other = saw_other || value == 5;
                    exact_other = exact_other && (value == 5 || value == 73);
                }
                if (!check(output, "rebound_heap_snapshot", exact_other && saw_other)) { return 12; }
            }
            if (!check(output, "unregister", vmhook::unregister_class<copy_one>()
                && vmhook::unregister_class<copy_two>())) { return 12; }
            check(output, "lifetime_complete", true);
            return 0;
        }
        catch (const std::exception& error)
        { output << "exception=" << error.what() << '\n'; output.flush(); return 13; }
    }
}
extern "C" BOOL __attribute__((ms_abi)) DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == 1)
    {
        ::DisableThreadLibraryCalls(instance);
        HANDLE thread{::CreateThread(nullptr, 0, worker, nullptr, 0, nullptr)};
        if (thread) { ::CloseHandle(thread); }
    }
    return true;
}
