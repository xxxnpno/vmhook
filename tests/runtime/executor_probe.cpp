#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
import vmhook.windows;
#else
#include <vmhook/vmhook.hpp>
#include <fstream>
#endif

namespace allocation_fault
{
    inline std::atomic<std::size_t> rejected_size{};
}
auto operator new(std::size_t size) -> void*
{
    auto expected = size;
    if (size && allocation_fault::rejected_size.compare_exchange_strong(expected, 0))
    { throw std::bad_alloc{}; }
    if (void* value = std::malloc(size ? size : 1)) { return value; }
    throw std::bad_alloc{};
}
auto operator delete(void* value) noexcept -> void { std::free(value); }
auto operator delete(void* value, std::size_t) noexcept -> void { std::free(value); }

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
    { output << name << '=' << (correct ? "pass" : "fail") << '\n'; output.flush(); return correct; }
    auto await(std::atomic<bool>& flag) -> bool
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        while (!flag.load() && std::chrono::steady_clock::now() < deadline) { ::Sleep(1); }
        return flag.load();
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
            vmhook::retained_object<node> saved{};
            {
                vmhook::native_thread_context source{"VMHook-executor-source"};
                if (!source.valid()) { throw vmhook::exception{source.error()}; }
                saved = vmhook::retain(fixture::create(41));
            }
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
        { output << "exception=" << error.what() << '\n'; output.flush(); return 14; }
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
