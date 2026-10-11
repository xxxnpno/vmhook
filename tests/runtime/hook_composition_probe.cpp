#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
import vmhook.windows;
#else
#include <vmhook/vmhook.hpp>
#include <format>
#endif

namespace
{
    class fixture final : public vmhook::object<fixture>
    {
    public:
        using vmhook::object<fixture>::object;
        static auto target(std::int32_t value) -> std::int32_t
        { return static_members::get_method("target")->call(value); }
        static auto target_object() -> std::unique_ptr<vmhook::any_object>
        { return static_members::get_method("target_object")->call(); }
    };
    auto worker(void*) -> DWORD
    {
        const char* path = std::getenv("VMHOOK_HOOK_COMPOSITION_REPORT");
        std::ofstream output{path ? path : "vmhook-hook-composition-report.txt"};
        output.setf(std::ios::unitbuf);
        bool valid = true;
        const auto check = [&](std::string_view name, bool value)
        { output << std::format("{}={}\n", name, (value ? "pass" : "fail")); valid = valid && value; };
        try
        {
            vmhook::native_thread_context context{"VMHook-hook-composition"};
            check("context", context.valid());
            if (!context.valid() || !vmhook::register_class<fixture>("HookCompositionFixture"))
            { check("registration", false); return 2; }
            unsigned order{};
            const auto second = vmhook::add_hook<fixture>("target", "(I)I",
                [&](vmhook::return_value& ret, std::int32_t) { order = order * 10 + 2; ret.set(std::int32_t{42}); },
                {.priority = 20});
            const auto first = vmhook::add_hook<fixture>("target", "(I)I",
                [&](vmhook::return_value& ret, std::int32_t value) { order = order * 10 + 1; ret.set_arg(0, value + 2); },
                {.priority = 10});
            const auto third = vmhook::add_hook<fixture>("target", "(I)I",
                [&](vmhook::return_value&, std::int32_t) { order = order * 10 + 3; }, {.priority = 30});
            check("three_observers", first && second && third);
            const auto active = vmhook::hook_resources();
            check("shared_policy", active.active_observers == 3 && active.retained_method_policies == 1
                && active.physical_stubs == 1 && vmhook::verify_hooks() == 0);
            check("priority_and_first_return", fixture::target(5) == 42 && order == 12);
            order = 0;
            check("remove_one", vmhook::remove_hook(second) && !vmhook::remove_hook(second));
            check("remaining_original_body", fixture::target(5) == 8 && order == 13);
            check("remove_remaining", vmhook::remove_hook(first) && vmhook::remove_hook(third));
            check("original_body", fixture::target(5) == 6);

            unsigned recursive_calls{};
            const auto recursive = vmhook::add_hook<fixture>("target", "(I)I",
                [&](vmhook::return_value& ret, std::int32_t value)
                { ++recursive_calls; ret.set(fixture::target(value) + 10); });
            check("recursive_observer", recursive != 0);
            check("suppressed_reentry", fixture::target(5) == 16 && recursive_calls == 1);
            check("same_thread_suppression", vmhook::call_without_hooks([] { return fixture::target(5); }) == 6
                && recursive_calls == 1);
            check("remove_recursive", vmhook::remove_hook(recursive));

            vmhook::method_hook_id self_removing{};
            unsigned removals{};
            self_removing = vmhook::add_hook<fixture>("target", "(I)I",
                [&](vmhook::return_value&, std::int32_t) { if (vmhook::remove_hook(self_removing)) { ++removals; } });
            check("callback_side_removal", self_removing && fixture::target(7) == 8
                && fixture::target(7) == 8 && removals == 1);
            check("native_reclamation", vmhook::reclaim_hook_callbacks() && vmhook::hook_callbacks_idle());
            const auto disabled = vmhook::hook_resources();
            check("disabled_resources", disabled.active_observers == 0 && disabled.retained_method_policies == 1
                && disabled.physical_stubs == 1 && disabled.dll_must_remain_loaded);
            const auto marker_before = vmhook::hook_resources();
            const auto object_hook = vmhook::add_hook<fixture>("target_object", "()Ljava/lang/Object;",
                [](vmhook::return_value& result)
                { const auto value = std::make_unique<vmhook::any_object>(nullptr); result.set(value); });
            const auto returned = fixture::target_object();
            const auto marker_after = vmhook::hook_resources();
            check("resident_return_markers", object_hook && returned && !returned->get_instance()
                && marker_after.resident_return_markers > marker_before.resident_return_markers
                && marker_after.resident_return_marker_bytes == marker_after.resident_return_markers
                    * sizeof(vmhook::detail::callback_return_roots::marker));
            check("remove_object_hook", vmhook::remove_hook(object_hook));
            vmhook::shutdown_hooks();
            check("shutdown_original_body", fixture::target(8) == 9);
            const auto stopped = vmhook::hook_resources();
            check("shutdown_resources", stopped.active_observers == 0 && stopped.retained_method_policies == 0
                && stopped.physical_stubs == 0 && stopped.dll_must_remain_loaded);
            check("resident_markers_after_shutdown", stopped.resident_return_markers == marker_after.resident_return_markers
                && stopped.resident_return_marker_bytes == marker_after.resident_return_marker_bytes);
            output << std::format("hook_composition_proof={}\n", (valid ? "pass" : "fail"));
            return valid ? 0 : 2;
        }
        catch (const std::exception& failure)
        { output << std::format("exception={}\nhook_composition_proof=fail\n", failure.what()); return 3; }
    }
}
extern "C" BOOL __attribute__((ms_abi)) DllMain(HINSTANCE self, DWORD reason, LPVOID)
{
    if (reason == 1)
    {
        ::DisableThreadLibraryCalls(self);
        if (auto thread = ::CreateThread(nullptr, 0, &worker, nullptr, 0, nullptr))
        { vmhook::windows::CloseHandle(thread); }
    }
    return true;
}
