#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
import vmhook.windows;
#else
#include <vmhook/vmhook.hpp>
#include <format>
#include <fstream>
#endif

namespace
{
    class fixture : public vmhook::object<fixture>
    {
    public:
        using vmhook::object<fixture>::object;
        static auto first_loader() -> std::unique_ptr<vmhook::any_object>
        { return static_members::get_method("first_loader")->call(); }
        static auto collect() -> void
        { const auto result = static_members::get_method("collect")->call();
          if (!result.was_invoked() || result.threw()) { throw vmhook::exception{"Collection failed"}; } }
        static auto drop_anchors() -> void
        { const auto result = static_members::get_method("drop_anchors")->call();
          if (!result.was_invoked() || result.threw()) { throw vmhook::exception{"Drop failed"}; } }
    };
    auto check(std::ostream& output, std::string_view name, bool correct) -> bool
    { output << std::format("{}={}\n", name, (correct ? "pass" : "fail")); output.flush(); return correct; }
    template<typename function_type> auto refused(function_type&& function) -> bool
    {
        try { function(); }
        catch (const vmhook::exception&) { return true; }
        return false;
    }
    auto worker(void*) -> DWORD
    {
        char destination[4096]{};
        if (!::GetEnvironmentVariableA("VMHOOK_TEST_RESULT", destination, sizeof(destination))) { return 1; }
        std::ofstream output{destination};
        try
        {
            if (!check(output, "metadata", vmhook::register_class<fixture>("LifetimeFixture"))) { return 2; }
            std::unique_ptr<vmhook::class_object> expired{};
            vmhook::ref<vmhook::any_object> expired_ref{};
            vmhook::borrowed<vmhook::any_object> expired_borrow{};
            std::string copied_name{};
            {
                vmhook::native_thread_context source{"VMHook-class-inventory"};
                const auto capabilities{vmhook::runtime_capabilities()};
                if (!check(output, "native_attach", source.valid()
                    && capabilities.support(vmhook::runtime_feature::loaded_class_inventory).state
                        == vmhook::feature_state::conditional)) { return 3; }
                if (!check(output, "inventory_bounds", refused([]{(void)vmhook::get_loaded_classes(1);})
                    && refused([]{(void)vmhook::get_loaded_classes(0);})
                    && refused([]{(void)vmhook::get_loaded_classes(1048577);}))) { return 4; }
                const auto loader{fixture::first_loader()};
                const auto expected{vmhook::retain(loader)};
                auto classes{vmhook::get_loaded_classes()};
                std::size_t copies{}, selected{}, other{};
                bool allocated{!classes.empty()}, saw_string{};
                for (auto& record : classes)
                {
                    allocated = allocated && record.type && record.type->get_instance();
                    if (record.name == "java.lang.String")
                    {
                        saw_string = record.type->loader() && !record.type->loader()->get_instance();
                    }
                    if (record.name != "LifetimeFixture$Duplicate") { continue; }
                    ++copies;
                    if (expected.is_same_object(record.type->loader())) { ++selected; }
                    else { ++other; }
                }
                if (!check(output, "duplicate_loaders", allocated && saw_string
                    && copies == 2 && selected == 1 && other == 1)) { return 5; }
                fixture::drop_anchors();
                for (std::size_t repetition{}; repetition < 3; ++repetition)
                {
                    fixture::collect();
                    for (const auto& record : classes)
                    { if (record.type->name() != record.name) { return 6; } }
                }
                copied_name = classes.front().name;
                expired = std::move(classes.front().type);
                expired_ref = vmhook::ref<vmhook::any_object>::from_native_root(expired->get_native_root());
                expired_borrow = vmhook::borrowed<vmhook::any_object>{expired->get_instance(), expired->get_native_root()};
                if (!check(output, "rooted_mirrors", expired && expired->name() == copied_name
                    && expired->is<vmhook::class_object>() && !expired->is<vmhook::java_string>()
                    && expired_ref.is<vmhook::class_object>() && expired_ref.instance_of("java/lang/Object")
                    && expired_borrow.is<vmhook::class_object>() && expired_borrow.instance_of("java/lang/Object"))) { return 6; }
            }
            bool context_refused{refused([&]{(void)vmhook::get_loaded_classes();})
                && refused([&]{(void)expired->get_instance();})
                && refused([&]{(void)expired_ref.instance_of("java/lang/Object");})
                && refused([&]{(void)expired_borrow.instance_of("java/lang/Object");}) && !copied_name.empty()};
            {
                vmhook::native_thread_context target{"VMHook-class-inventory-receiver"};
                context_refused = context_refused && target.valid()
                    && refused([&]{(void)expired->get_instance();});
            }
            if (!check(output, "inventory_context", context_refused)) { return 7; }
            check(output, "inventory_complete", true);
            return 0;
        }
        catch (const std::exception& error)
        { output << std::format("exception={}\n", error.what()); output.flush(); return 8; }
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
