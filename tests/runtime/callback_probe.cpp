#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
import vmhook.windows;
#else
#include <vmhook/vmhook.hpp>
#include <chrono>
#include <fstream>
#endif

namespace
{
    class callback_fixture : public vmhook::object<callback_fixture>
    { public: using vmhook::object<callback_fixture>::object; };
    class callback_node : public vmhook::object<callback_node>
    {
    public:
        using vmhook::object<callback_node>::object;
        auto value() const -> std::int32_t { return get_field("value")->get(); }
    };
    std::atomic<unsigned> callback_count{}, nested_count{}, observed_gc{};
    std::atomic<bool> correct{true};
    std::atomic<const char*> first_failure{};
    std::optional<vmhook::hotspot::method_args> escaped_arguments;
    std::ofstream report;

    auto check(const bool condition, const char* const label) noexcept -> void
    {
        if (condition) { return; }
        const char* expected{};
        (void)first_failure.compare_exchange_strong(expected, label,
            std::memory_order_release, std::memory_order_relaxed);
        correct.store(false, std::memory_order_release);
    }

    auto read_gc() -> std::int32_t
    { return callback_fixture::static_members::get_field("gcEpoch")->get(); }

    auto worker(void*) -> vmhook::windows::DWORD
    {
        const auto worker_started{std::chrono::steady_clock::now()};
        char destination[4096]{};
        if (!vmhook::windows::GetEnvironmentVariableA("VMHOOK_CALLBACK_RESULT", destination, sizeof(destination)))
        { return 1; }
        report.open(destination);
        report.setf(std::ios::unitbuf);
        try
        {
            vmhook::native_thread_context context{"VMHook-callback-regression"};
            if (!context) { throw vmhook::exception{context.error()}; }
            if (!vmhook::register_class<callback_fixture>("ObservationCallbackFixture")
                || !vmhook::register_class<callback_node>("ObservationCallbackFixture$Node"))
            { throw vmhook::exception{"Callback fixture classes unavailable"}; }
            const bool nested{vmhook::hook<callback_fixture>("nested", "(LObservationCallbackFixture$Node;)I",
                [](vmhook::return_value&, const std::unique_ptr<callback_node>& original)
                {
                    check(vmhook::detail::native_backend::active(), "nested.context_active");
                    check(vmhook::detail::private_native::thread_state(
                        vmhook::hotspot::current_java_thread) == 4, "nested.native_state");
                    if (!original) { check(false, "nested.input_present"); }
                    else { check(original->value() > 0, "nested.input_value"); }
                    nested_count.fetch_add(1, std::memory_order_release);
                })};
            const bool main{vmhook::hook<callback_fixture>("target",
                "(LObservationCallbackFixture$Node;LObservationCallbackFixture$Node;I)LObservationCallbackFixture$Node;",
                [](vmhook::return_value& result, const std::unique_ptr<callback_node>& input,
                   const std::unique_ptr<callback_node>& nullable, std::int32_t expected)
                {
                    check(vmhook::detail::native_backend::active(), "outer.context_active");
                    check(vmhook::detail::private_native::thread_state(
                        vmhook::hotspot::current_java_thread) == 4, "outer.native_state");
                    const auto frame_arguments = result.frame()->get_arguments();
                    check(frame_arguments.size() == 3, "outer.frame_argument_count");
                    const auto before{read_gc()};
                    vmhook::windows::Sleep(35);
                    if (read_gc() > before) { observed_gc.fetch_add(1, std::memory_order_relaxed); }
                    if (!input) { check(false, "outer.input_present_after_gc"); }
                    else { check(input->value() == expected, "outer.input_value_after_gc"); }
                    check(static_cast<bool>(nullable), "outer.nullable_wrapper_allocated");
                    if (nullable)
                    {
                        check(expected % 2 == 0 ? nullable->get_instance() == nullptr
                            : nullable->get_instance() != nullptr && nullable->value() == -expected,
                            "outer.nullable_java_instance");
                    }
                    auto* const parent{vmhook::detail::native_backend::current_context_lifetime()};
                    const auto nested_result{callback_fixture::static_members::get_method("nested", "(LObservationCallbackFixture$Node;)I")->call(input)};
                    if (!nested_result.was_invoked()) { check(false, "outer.nested_invoked"); }
                    else { check(static_cast<std::int32_t>(nested_result) == expected, "outer.nested_value"); }
                    check(vmhook::detail::native_backend::current_context_lifetime() == parent,
                        "outer.parent_context_restored");
                    const auto refreshed_input = frame_arguments.as<callback_node>(0);
                    const auto refreshed_nullable = frame_arguments.as<callback_node>(1);
                    check(refreshed_input && refreshed_input->value() == expected,
                        "outer.frame_root_after_nested_gc");
                    check(refreshed_nullable && (expected % 2 == 0
                        ? refreshed_nullable->get_instance() == nullptr
                        : refreshed_nullable->value() == -expected), "outer.frame_nullable_owner");
                    try { (void)frame_arguments.as<callback_node>(2); check(false, "outer.frame_primitive_refused"); }
                    catch (const vmhook::exception&) {}
                    try { (void)frame_arguments[3]; check(false, "outer.frame_index_refused"); }
                    catch (const vmhook::exception&) {}
                    escaped_arguments = frame_arguments;
                    auto allocated{vmhook::make_unique<callback_node>(std::int32_t{expected + 1000})};
                    if (!allocated) { check(false, "outer.allocated_present"); }
                    else { check(static_cast<bool>(allocated->get_native_root()), "outer.allocated_root"); }
                    vmhook::windows::Sleep(35);
                    if (!allocated) { check(false, "outer.allocated_present_after_gc"); }
                    else { check(allocated->value() == expected + 1000, "outer.allocated_value_after_gc"); }
                    if (allocated) { result.set(allocated); }
                    callback_count.fetch_add(1, std::memory_order_release);
                })};
            if (!nested || !main) { throw vmhook::exception{"Callback hooks refused"}; }
            callback_fixture::static_members::get_field("start")->set(true);
            // Field reads can wait for safepoints during the concurrent GC storm.
            const auto wait_started{std::chrono::steady_clock::now()};
            const auto deadline{wait_started + std::chrono::seconds{120}};
            unsigned polls{};
            bool deadline_reached{};
            while (correct.load(std::memory_order_acquire))
            {
                if (std::chrono::steady_clock::now() >= deadline)
                { deadline_reached = true; break; }
                ++polls;
                const auto observed{static_cast<std::int32_t>(callback_fixture::static_members::get_field("validReturns")->get())};
                if (std::chrono::steady_clock::now() >= deadline)
                { deadline_reached = true; break; }
                if (observed >= 24) { break; }
                vmhook::windows::Sleep(10);
            }
            if (deadline_reached) { check(false, "worker.wait_deadline"); }
            const auto returned{static_cast<std::int32_t>(callback_fixture::static_members::get_field("validReturns")->get())};
            const auto finished{std::chrono::steady_clock::now()};
            report << "callbacks=" << callback_count.load() << "\nnested=" << nested_count.load()
                   << "\ngc_observed=" << observed_gc.load() << "\nreturns=" << returned
                   << "\nwait_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(finished - wait_started).count()
                   << "\nworker_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(finished - worker_started).count()
                   << "\npolls=" << polls << "\nwait_completed=" << (returned == 24)
                   << "\nreturn_roots_scanned=" << vmhook::detail::callback_return_roots::scanned_slots.load() << '\n';
            if (callback_count.load() != 24) { check(false, "worker.callback_count"); }
            else if (nested_count.load() != 24) { check(false, "worker.nested_count"); }
            else if (observed_gc.load() == 0) { check(false, "worker.gc_observed"); }
            else { check(returned == 24, "worker.valid_returns"); }
            try
            {
                if (escaped_arguments) { (void)escaped_arguments->as<callback_node>(0); }
                check(false, "worker.expired_frame_context_refused");
            }
            catch (const vmhook::exception&) {}
            if (const char* const failure{first_failure.load(std::memory_order_acquire)})
            { report << "first_failure=" << failure << '\n'; }
            report << "callback_proof=" << (correct.load() ? "pass" : "fail") << '\n';
            return correct.load() ? 0 : 2;
        }
        catch (const std::exception& failure)
        { report << "exception=" << failure.what() << "\ncallback_proof=fail\n"; return 3; }
    }
}

extern "C" BOOL __attribute__((ms_abi)) DllMain(HINSTANCE self, DWORD reason, LPVOID)
{
    if (reason == 1)
    {
        ::DisableThreadLibraryCalls(self);
        if (auto thread{::CreateThread(nullptr, 0, &worker, nullptr, 0, nullptr)})
        { vmhook::windows::CloseHandle(thread); }
    }
    return true;
}
