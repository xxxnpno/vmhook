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
        static auto load_late_class() -> void { static_members::get_method("load_late_class")->call(); }
        static auto stack_trace_events() -> void { static_members::get_method("stack_trace_events")->call(); }
        static auto callback_gc(const std::unique_ptr<vmhook::any_object>& value) -> bool
        { return static_members::get_method("callback_gc")->call(value); }
    };
    class late final : public vmhook::object<late>
    {
    public:
        using vmhook::object<late>::object;
        static auto target(std::int32_t value) -> std::int32_t { return static_members::get_method("target")->call(value); }
    };
    class integer final : public vmhook::object<integer>
    {
    public:
        using vmhook::object<integer>::object;
        static auto parse(std::string_view value) -> std::int32_t
        { return static_members::get_method("parseInt", "(Ljava/lang/String;)I")->call(value); }
    };
    auto worker(void*) -> DWORD
    {
        const char* path = std::getenv("VMHOOK_EVENT_STREAM_REPORT");
        std::ofstream output{path ? path : "vmhook-event-stream-report.txt"};
        output.setf(std::ios::unitbuf); bool valid = true;
        const auto check = [&](std::string_view name, bool value)
        { output << std::format("{}={}\n", name, (value ? "pass" : "fail")); valid = valid && value; };
        try
        {
            vmhook::native_thread_context context{"VMHook-event-stream"};
            if (!context.valid() || !vmhook::register_class<fixture>("EventStreamFixture"))
            { check("context", false); return 2; }
            check("context", true);
            auto events = std::make_shared<vmhook::class_event_queue>(256);
            const auto observer = vmhook::on_class_loaded(events);
            check("queued_observer", observer != 0);
            unsigned late_calls{};
            const auto deferred = vmhook::hook_when_loaded<late>("EventStreamFixture$Late", "target", "(I)I",
                [&](vmhook::return_value& ret, std::int32_t) { ++late_calls; ret.set(std::int32_t{77}); });
            check("deferred_registration", deferred != 0);
            check("initial_wait", vmhook::process_pending_hooks() == 0
                && vmhook::deferred_hook_status(deferred)->state == vmhook::deferred_hook_state::waiting_for_class);
            fixture::load_late_class();
            vmhook::class_load_record record{}; bool found{};
            while (events->try_pop(record))
            {
                if (record.name() == "EventStreamFixture$Late")
                { found = !record.truncated && !record.bootstrap_loader && record.event_id != 0; }
            }
            check("copied_class_record", found);
            check("deferred_native_install", vmhook::process_pending_hooks() == 1
                && vmhook::deferred_hook_status(deferred)->state == vmhook::deferred_hook_state::installed);
            check("deferred_callback", late::target(5) == 77 && late_calls == 1);
            check("deferred_removal", vmhook::remove_deferred_hook(deferred) && !vmhook::remove_deferred_hook(deferred)
                && vmhook::deferred_hook_status(deferred)->state == vmhook::deferred_hook_state::cancelled);
            check("deferred_original_body", late::target(5) == 6 && late_calls == 1);
            check("deferred_reclamation", vmhook::reclaim_deferred_hooks() && vmhook::reclaim_hook_callbacks());
            const auto stubs_before_shared = vmhook::hook_resources().physical_stubs;
            unsigned shared_calls{};
            const auto shared_observer = vmhook::register_class<integer>("java/lang/Integer")
                ? vmhook::add_hook<integer>("parseInt", "(Ljava/lang/String;)I",
                    [&](vmhook::return_value&, const std::unique_ptr<vmhook::any_object>&) { ++shared_calls; }) : 0;
            check("shared_nonsynchronized_observer", shared_observer != 0);
            check("shared_cds_physical_stub", vmhook::hook_resources().physical_stubs == stubs_before_shared);
            check("shared_nonsynchronized_body", integer::parse("12345") == 12345 && shared_calls == 1);
            check("shared_nonsynchronized_removal", vmhook::remove_hook(shared_observer)
                && integer::parse("-7") == -7 && shared_calls == 1);
            unsigned stack_events{}; bool owning = true; bool synchronized_entry_gc = true;
            const auto throwable_observer = vmhook::on_throwable_stack_trace(
                [&](const vmhook::throwable_stack_trace_event& event)
                {
                    if (event.internal_name == "java/lang/RuntimeException")
                    {
                        ++stack_events; owning = owning && event.throwable && event.throwable->get_instance();
                        synchronized_entry_gc = synchronized_entry_gc && fixture::callback_gc(event.throwable);
                    }
                });
            check("throwable_observer", throwable_observer != 0);
            fixture::stack_trace_events();
            check("exact_stack_trace_semantics", stack_events == 2 && owning);
            check("synchronized_entry_gc", synchronized_entry_gc && stack_events == 2);
            check("throwable_removal", vmhook::remove_hook(throwable_observer));
            check("class_removal", vmhook::remove_class_observer(observer) && !vmhook::remove_class_observer(observer)
                && vmhook::class_observers_idle());
            std::weak_ptr<vmhook::class_event_queue> weak = events;
            events.reset();
            check("class_capture_reclamation", vmhook::reclaim_class_observers() && weak.expired());
            vmhook::shutdown_hooks();
            output << std::format("event_stream_proof={}\n", (valid ? "pass" : "fail"));
            return valid ? 0 : 2;
        }
        catch (const std::exception& failure)
        { output << std::format("exception={}\nevent_stream_proof=fail\n", failure.what()); return 3; }
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
