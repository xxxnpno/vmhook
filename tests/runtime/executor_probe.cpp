#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
import vmhook.windows;
#else
#include <vmhook/vmhook.hpp>
#include <format>
#include <fstream>
#endif

namespace allocation_fault
{
    inline std::atomic<std::size_t> rejected_size{};
    inline thread_local std::int32_t context_budget{-1};
    inline thread_local std::size_t context_allocations{};
    inline thread_local bool count_context_allocations{};
    inline std::atomic<std::uint32_t> executor_fault_origin{};
    inline std::atomic<bool> reject_executor_session{};
}
[[gnu::noinline]] auto operator new(std::size_t size) -> void*
{
    if (allocation_fault::count_context_allocations) { ++allocation_fault::context_allocations; }
    if (allocation_fault::context_budget >= 0 && allocation_fault::context_budget-- == 0)
    { allocation_fault::context_budget = -1; throw std::bad_alloc{}; }
    if (allocation_fault::reject_executor_session.load(std::memory_order_acquire)
        && size == sizeof(vmhook::detail::private_native::java_session)
        && vmhook::windows::GetCurrentThreadId() != allocation_fault::executor_fault_origin.load()
        && allocation_fault::reject_executor_session.exchange(false))
    { throw std::bad_alloc{}; }
    auto expected = size;
    if (size && allocation_fault::rejected_size.compare_exchange_strong(expected, 0))
    { throw std::bad_alloc{}; }
    if (void* value = std::malloc(size ? size : 1)) { return value; }
    throw std::bad_alloc{};
}
[[gnu::noinline]] auto operator delete(void* value) noexcept -> void { std::free(value); }
[[gnu::noinline]] auto operator delete(void* value, std::size_t) noexcept -> void { std::free(value); }

namespace
{
    class node : public vmhook::object<node>
    {
    public:
        using vmhook::object<node>::object;
        auto value() const -> std::int32_t { return get_field("value")->get(); }
        auto add(std::int32_t amount) const -> std::int32_t { return get_method("add")->call(amount); }
    };
    class fixture : public vmhook::object<fixture>
    {
    public:
        using vmhook::object<fixture>::object;
        static auto create(std::int32_t value) -> std::unique_ptr<node>
        { return static_members::get_method("create")->call(value); }
        static auto thread_name() -> std::string
        { return static_members::get_method("thread_name")->call(); }
        static auto collect() -> void
        { const auto result = static_members::get_method("collect")->call();
          if (!result.was_invoked() || result.threw()) { throw vmhook::exception{"GC invocation failed"}; } }
        static auto fail() -> void { (void)static_members::get_method("fail")->call(); }
    };
    static_assert(vmhook::detail::native_transferable<std::int32_t>);
    static_assert(vmhook::detail::native_transferable<std::vector<std::string>>);
    template<typename value_type>
    struct context_allocator : std::allocator<value_type>
    {
        node* borrowed_wrapper{};
        template<typename other_type> struct rebind { using other = context_allocator<other_type>; };
    };
    static_assert(!vmhook::detail::native_transferable<std::vector<std::int32_t,
        context_allocator<std::int32_t>>>);
    static_assert(vmhook::detail::native_transferable<vmhook::retained_object<node>>);
    static_assert(!vmhook::detail::native_transferable<std::unique_ptr<node>>);
    static_assert(!vmhook::detail::native_transferable<node>);
    static_assert(!vmhook::detail::native_transferable<node*>);
    static_assert(!vmhook::detail::native_transferable<std::string_view>);
    static_assert(!vmhook::detail::native_transferable<std::int32_t&>);
    template<typename callable_type, typename... argument_types>
    concept accepts_task = requires(vmhook::native_executor& executor, callable_type function,
                                    argument_types... arguments)
    { executor.submit(std::move(function), std::move(arguments)...); };
    static_assert(!accepts_task<decltype([captured = std::int32_t{1}] { return captured; })>);
    static_assert(!accepts_task<decltype([]() -> std::unique_ptr<node> { return {}; })>);
    static_assert(!accepts_task<decltype([](std::unique_ptr<node>) { return 0; }), std::unique_ptr<node>>);
    std::atomic<bool> started{}, release_worker{}, cancellation_started{}, closing_started{};
    std::atomic<std::int32_t> unexpected_executions{};
    vmhook::native_executor* active_executor{};
    template<typename function_type>
    auto refused(function_type&& function) -> bool
    { try { function(); } catch (const vmhook::exception&) { return true; } return false; }
    auto check(std::ostream& output, std::string_view name, bool correct) -> bool
    { output << std::format("{}={}\n", name, (correct ? "pass" : "fail")); output.flush(); return correct; }
    auto await(std::atomic<bool>& flag) -> bool
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        while (!flag.load() && std::chrono::steady_clock::now() < deadline) { ::Sleep(1); }
        return flag.load();
    }
    auto context_allocation_failures(std::ostream& output) -> bool
    {
        // Warm profile/TLS and collector lookup independently of this test.
        // The long name then makes the constructor's string allocation visible,
        // followed by the context, attachment and Java-session allocations.
        constexpr std::string_view name{
            "VMHook-context-allocation-fault-with-a-long-name-for-real-attachment-cleanup"};
        allocation_fault::context_allocations = 0;
        allocation_fault::count_context_allocations = true;
        {
            vmhook::native_thread_context context{name};
            allocation_fault::count_context_allocations = false;
            if (!context.valid()) { throw vmhook::exception{context.error()}; }
        }
        const auto allocations = allocation_fault::context_allocations;
        if (!check(output, "context_allocation_coverage", allocations >= 5)) { return false; }
        bool propagated{true};
        bool cleaned{true};
        bool retry{true};
        for (std::size_t index{}; index < allocations; ++index)
        {
            bool rejected{};
            allocation_fault::context_budget = static_cast<std::int32_t>(index);
            try { vmhook::native_thread_context context{name}; }
            catch (const std::bad_alloc&) { rejected = true; }
            allocation_fault::context_budget = -1;
            propagated = propagated && rejected;
            cleaned = cleaned && !vmhook::detail::native_backend::current_context_lifetime()
                && !vmhook::detail::private_native::rt().current_thread()
                && vmhook::detail::private_native::active_session_count == 0;
            vmhook::native_thread_context context{name};
            retry = retry && context.valid() && fixture::thread_name() == name;
        }
        const auto propagation_ok = check(output, "context_cpp_oom", propagated);
        const auto cleanup_ok = check(output, "context_failure_cleanup", cleaned);
        const auto retry_ok = check(output, "context_failure_retry", retry);
        return propagation_ok && cleanup_ok && retry_ok;
    }
    auto legacy_reference_allocation_failures(std::ostream& output) -> bool
    {
        vmhook::native_thread_context context{"VMHook-reference-faults"};
        if (!context.valid()) { throw vmhook::exception{context.error()}; }
        const auto owner = fixture::create(37);
        const vmhook::borrowed<node> borrowed{owner->get_instance(), owner->get_native_root()};
        const auto reference = borrowed.pin();
        const auto owner_class = vmhook::detail::registered_class_klass<fixture>();
        const auto retained_field = vmhook::find_field(owner_class, "retained");
        const auto child_field = vmhook::find_field(vmhook::detail::registered_class_klass<node>(), "child");
        if (!child_field || !retained_field) { throw vmhook::exception{"Reference fixture metadata is absent"}; }
        const auto nodes = vmhook::ref<void>::at_static("ExecutorFixture", "retained_nodes");
        // Keep the raw diagnostic borrow and each faulted expression under one
        // VM operation. The fixture's independent GC cannot invalidate them.
        vmhook::detail::native_backend::operation access{};
        const vmhook::borrowed<node> raw_borrowed{owner->get_instance()};
        const vmhook::borrowed<void> raw_untyped_borrowed{owner->get_instance()};
        auto fails = [](auto&& expression) -> bool
        {
            bool rejected{};
            allocation_fault::context_budget = 0;
            try { expression(); }
            catch (const std::bad_alloc&) { rejected = true; }
            allocation_fault::context_budget = -1;
            return rejected;
        };
        // This boundary used to be noexcept. Failing root-ticket allocation
        // must reach C++, without invoking terminate or crossing a JVM ABI.
        check(output, "legacy_access_fault_begin", true);
        const auto access_oom = fails([&] { (void)reference.operator->(); })
            && fails([&] { (void)reference.operator*(); })
            && fails([&] { (void)reference.erased().operator->(); })
            && fails([&] { (void)reference.erased().operator*(); });
        const auto borrowed_access_oom = fails([&] { (void)raw_borrowed.operator->(); })
            && fails([&] { (void)raw_borrowed.operator*(); })
            && fails([&] { (void)raw_untyped_borrowed.operator->(); })
            && fails([&] { (void)raw_untyped_borrowed.operator*(); });
        const auto anchor_oom = fails([&] { (void)borrowed.pin(); })
            && fails([&] { (void)vmhook::ref<node>::from_native_root(owner->get_native_root()); })
            && fails([&] { (void)vmhook::ephemeral_ref<node>(owner->get_instance()); })
            && fails([&] { (void)reference.field_at<node>(child_field->offset); })
            && fails([&] { (void)nodes.element<node>(0); })
            && fails([&] { (void)vmhook::elements_of<node>(nodes); })
            && fails([&] { (void)reference.field<node>("child"); })
            && fails([&] { (void)borrowed.field<node>("child"); })
            && fails([&] { (void)vmhook::static_ref<node>("ExecutorFixture", "retained"); })
            && fails([&] { (void)vmhook::ref<node>::at_static(owner_class, retained_field->offset); });
        const vmhook::root<node> static_root{"ExecutorFixture", "retained"};
        const auto root_oom = fails([&] { (void)static_root.resolve(); })
            && fails([&] { (void)static_root.bound(); })
            && fails([&] { (void)static_cast<bool>(static_root); });
        const auto diagnostics_oom = fails([&] { (void)reference.class_name(); });
        const auto boundaries_ok = check(output, "legacy_access_cpp_oom", access_oom && borrowed_access_oom);
        const auto anchors_ok = check(output, "legacy_anchor_cpp_oom", anchor_oom && root_oom && diagnostics_oom);
        const auto retry_ok = check(output, "legacy_failure_retry", reference->value() == 37
            && reference.field<node>("child")->value() == 37 && static_root->value() == 17
            && vmhook::ref<node>::ephemeral(nullptr).empty()
            && vmhook::ref<node>{}.field_at<node>(0).empty()
            && vmhook::ref<node>{}.element<node>(-1).empty());
        return boundaries_ok && anchors_ok && retry_ok;
    }
    auto class_observer_allocation_failure(std::ostream& output) -> bool
    {
        const auto warm = vmhook::on_class_loaded([](const vmhook::class_load_event&) {});
        if (!warm || !vmhook::remove_class_observer(warm))
        { throw vmhook::exception{"Class observer setup is unavailable"}; }
        bool rejected{};
        allocation_fault::context_budget = 0;
        try { (void)vmhook::on_class_loaded([](const vmhook::class_load_event&) {}); }
        catch (const std::bad_alloc&) { rejected = true; }
        allocation_fault::context_budget = -1;
        if (!check(output, "class_observer_cpp_oom", rejected)) { return false; }
        static std::atomic<std::size_t> notifications{};
        const auto observer = vmhook::on_class_loaded([](const vmhook::class_load_event& event)
        {
            if (event.internal_name == "ExecutorFixture$ObserverMarker") { ++notifications; }
        });
        if (!observer) { throw vmhook::exception{"Class observer retry failed"}; }
        {
            vmhook::native_thread_context context{"VMHook-class-observer-retry"};
            if (!context.valid()) { throw vmhook::exception{context.error()}; }
            (void)fixture::static_members::get_method("load_observer_marker")->call();
        }
        return check(output, "class_observer_failure_retry", notifications.load() == 1
            && vmhook::remove_class_observer(observer));
    }
    auto worker(void*) -> DWORD
    {
        char destination[4096]{};
        if (!::GetEnvironmentVariableA("VMHOOK_TEST_RESULT", destination, sizeof(destination))) { return 1; }
        std::ofstream output{destination};
        try
        {
            // Prepare lookup/TLS bookkeeping before rejecting exactly the first
            // parked keeper allocation. This must propagate a C++ allocation
            // failure rather than install a name-only class binding.
            (void)vmhook::detail::private_native::rt().current_thread();
            (void)vmhook::detail::native_backend::current_context_lifetime();
            (void)vmhook::vm_capabilities();
            (void)vmhook::detail::read_gc_flags();
            allocation_fault::rejected_size.store(sizeof(vmhook::detail::persistent_hook_roots::keeper));
            bool preparation_oom{};
            try { (void)vmhook::detail::prepare_durable_roots(); }
            catch (const std::bad_alloc&) { preparation_oom = true; }
            allocation_fault::rejected_size.store(0);
            if (!check(output, "preparation_cpp_oom", preparation_oom
                && !vmhook::detail::persistent_hook_roots::prepared_keeper.load())) { return 2; }
            if (!check(output, "metadata", vmhook::register_class<fixture>("ExecutorFixture")
                && vmhook::register_class<node>("ExecutorFixture$Node"))) { return 2; }
            if (!legacy_reference_allocation_failures(output)
                || !context_allocation_failures(output)
                || !class_observer_allocation_failure(output)) { return 2; }
            vmhook::retained_object<node> saved{};
            {
                vmhook::native_thread_context source{"VMHook-executor-source"};
                if (!source.valid()) { throw vmhook::exception{source.error()}; }
                saved = vmhook::retain(fixture::create(41));
            }
            allocation_fault::executor_fault_origin.store(vmhook::windows::GetCurrentThreadId());
            allocation_fault::reject_executor_session.store(true, std::memory_order_release);
            bool executor_initialization_oom{};
            try { vmhook::native_executor rejected{{1, 2}}; }
            catch (const std::bad_alloc&) { executor_initialization_oom = true; }
            const auto session_rejected = !allocation_fault::reject_executor_session.exchange(false);
            if (!check(output, "executor_context_cpp_oom", executor_initialization_oom && session_rejected)) { return 2; }
            vmhook::native_executor executor{{1, 2}};
            active_executor = &executor;
            auto blocker = executor.submit([](const vmhook::native_task_control& control) -> std::string
            {
                started.store(true);
                while (!release_worker.load()) { control.throw_if_cancelled(); ::Sleep(1); }
                return fixture::thread_name();
            });
            if (!await(started)) { throw vmhook::exception{"Executor worker did not start"}; }
            auto cancelled = executor.submit([]() -> std::int32_t { ++unexpected_executions; return 7; });
            auto deadline = executor.submit_before(std::chrono::steady_clock::now() + std::chrono::milliseconds{20},
                []() -> std::int32_t { ++unexpected_executions; return 8; });
            bool full = refused([&]
            { auto task = executor.submit([](vmhook::retained_object<node>) -> std::int32_t { return 0; }, std::move(saved)); (void)task; });
            if (!check(output, "backpressure", full && saved.valid() && executor.stats().queued == 2)) { return 3; }
            bool cancelled_now = cancelled.cancel() && refused([&] { (void)cancelled.get(); });
            if (!check(output, "queued_cancel", cancelled_now && unexpected_executions.load() == 0)) { return 4; }
            ::Sleep(60);
            release_worker.store(true);
            const auto name = blocker.get();
            if (!check(output, "worker_context", name == "VMHook-executor-0")) { return 5; }
            if (!check(output, "queued_deadline", refused([&] { (void)deadline.get(); })
                && unexpected_executions.load() == 0)) { return 6; }
            auto acquired = executor.submit([](vmhook::retained_object<node> retained) -> std::int32_t
            {
                fixture::collect();
                auto current = retained.acquire();
                return current->add(1);
            }, std::move(saved));
            if (!check(output, "retained_argument", !saved.valid() && acquired.get() == 42)) { return 7; }
            auto result = executor.submit([](std::int32_t value) -> vmhook::retained_object<node>
            { return vmhook::retain(fixture::create(value)); }, std::int32_t{73});
            auto returned = result.get();
            {
                vmhook::native_thread_context receiver{"VMHook-executor-receiver"};
                fixture::collect();
                auto current = returned.acquire();
                if (!check(output, "retained_result", current && current->value() == 73)) { return 8; }
            }
            auto failed = executor.submit([]() -> std::int32_t { fixture::fail(); return 0; });
            bool failure_copied{};
            {
                vmhook::native_thread_context receiver{"VMHook-executor-failure-receiver"};
                try { (void)failed.get(); }
                catch (const vmhook::exception& error)
                {
                    failure_copied = error.code() == vmhook::error_code::java_exception
                        && error.exception_class() == "java/lang/IllegalStateException"
                        && error.has_throwable() && refused([&] { (void)error.get_throwable(); });
                }
            }
            if (!check(output, "copied_java_failure", failure_copied)) { return 8; }
            auto cpp_failed = executor.submit([]() -> std::int32_t { throw std::bad_alloc{}; });
            bool allocation_copied{};
            try { (void)cpp_failed.get(); }
            catch (const std::bad_alloc&) { allocation_copied = true; }
            if (!check(output, "executor_task_cpp_oom", allocation_copied)) { return 8; }
            auto throwable = executor.submit([]() -> vmhook::retained_object<vmhook::any_object>
            {
                try { fixture::fail(); }
                catch (const vmhook::exception& error) { return vmhook::retain(error.get_throwable()); }
                throw vmhook::exception{"Expected Java failure did not occur"};
            });
            auto retained_throwable = throwable.get();
            {
                vmhook::native_thread_context receiver{"VMHook-executor-throwable-receiver"};
                fixture::collect();
                auto current = retained_throwable.acquire();
                if (!check(output, "retained_throwable", current->get_method("getMessage")->call().as_string()
                    == "expected executor exception")) { return 8; }
            }
            auto running = executor.submit([](const vmhook::native_task_control& control) -> std::int32_t
            {
                cancellation_started.store(true);
                for (;;) { control.throw_if_cancelled(); ::Sleep(1); }
            });
            if (!await(cancellation_started)) { throw vmhook::exception{"Cancellation worker did not start"}; }
            if (!check(output, "running_cancel", running.cancel() && refused([&] { (void)running.get(); }))) { return 9; }
            auto no_self_wait = executor.submit([]() -> bool
            {
                auto nested = active_executor->submit([]() -> std::int32_t { return 11; });
                const bool polling = !nested.ready()
                    && nested.wait_for(std::chrono::milliseconds{0}) == std::future_status::timeout;
                const bool wait_refused = refused([&] { (void)nested.get(); });
                nested.cancel();
                const bool close_refused = refused([&] { active_executor->close(); });
                return polling && wait_refused && close_refused;
            });
            if (!check(output, "self_wait_refused", no_self_wait.get())) { return 10; }
            executor.close();
            if (!check(output, "joined_close", !executor.stats().accepting && executor.stats().queued == 0
                && executor.stats().executing == 0 && refused([&] { auto task = executor.submit([] { return 0; }); (void)task; }))) { return 11; }
            active_executor = nullptr;
            vmhook::native_executor parallel{{2, 4}};
            auto first = parallel.submit([] { return fixture::create(1)->add(10); });
            auto second = parallel.submit([] { return fixture::create(2)->add(20); });
            if (!check(output, "parallel_workers", first.get() == 11 && second.get() == 22)) { return 12; }
            parallel.close();
            vmhook::native_executor stopping{{1, 2}};
            auto in_flight = stopping.submit([](const vmhook::native_task_control& control) -> bool
            {
                closing_started.store(true);
                while (!control.stop_requested()) { ::Sleep(1); }
                return true;
            });
            if (!await(closing_started)) { throw vmhook::exception{"Close worker did not start"}; }
            auto pending = stopping.submit([] { ++unexpected_executions; return false; });
            stopping.close(false);
            if (!check(output, "cancelled_close", refused([&] { (void)in_flight.get(); })
                && refused([&] { (void)pending.get(); }) && stopping.stats().executing == 0
                && unexpected_executions.load() == 0)) { return 13; }
            stopping.close(false);
            check(output, "executor_complete", true);
            return 0;
        }
        catch (const std::exception& error)
        { output << std::format("exception={}\n", error.what()); output.flush(); return 14; }
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
