#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
import vmhook.windows;
#else
#include <vmhook/vmhook.hpp>
#include <format>
#include <fstream>
#include <cstdlib>
#include <new>
#endif

namespace
{
    thread_local bool fail_allocation{};
    thread_local std::size_t fail_countdown{};
    thread_local std::size_t fail_size{};
    struct allocation_marker final : std::bad_alloc
    {
        auto what() const noexcept -> const char* override { return "deadlock allocation marker"; }
    };
}

__attribute__((noinline)) void* operator new(const std::size_t size)
{
    if (fail_allocation && (fail_size == 0 || fail_size == size))
    {
        if (fail_countdown == 0) { fail_allocation = false; throw allocation_marker{}; }
        --fail_countdown;
    }
    if (void* value = std::malloc(size ? size : 1)) { return value; }
    throw std::bad_alloc{};
}
__attribute__((noinline)) void operator delete(void* value) noexcept { std::free(value); }
__attribute__((noinline)) void operator delete(void* value, std::size_t) noexcept { std::free(value); }

namespace
{
    class fixture final : public vmhook::object<fixture>
    {
    public:
        using vmhook::object<fixture>::object;
        static auto start_monitors() -> void
        { (void)static_members::get_method("start_monitor_cycle", "()V")->call(); }
        static auto start_synchronizers() -> void
        { (void)static_members::get_method("start_synchronizer_cycle", "()V")->call(); }
        static auto id(const std::string_view name) -> std::int64_t
        { return static_members::get_method(name, "()J")->call(); }
        static auto hash(const std::string_view name) -> std::int32_t
        { return static_members::get_method(name, "()I")->call(); }
        static auto bean(const std::string_view name) -> std::unique_ptr<vmhook::any_object>
        { return static_members::get_method(name, "()Ljava/lang/management/ThreadMXBean;")->call(); }
    };

    template<typename callable>
    auto refused(callable call) -> bool
    {
        try { call(); }
        catch (const vmhook::exception&) { return true; }
        return false;
    }

    auto find_thread(const std::vector<vmhook::java_thread_snapshot>& threads, const std::int64_t id)
        -> const vmhook::java_thread_snapshot*
    {
        const auto found{std::find_if(threads.begin(), threads.end(),
            [id](const auto& thread) { return thread.id == id; })};
        return found == threads.end() ? nullptr : &*found;
    }

    auto cycle(const vmhook::deadlock_snapshot& snapshot, const std::int64_t first,
        const std::int64_t second, const std::string_view prefix) -> bool
    {
        const auto* a{find_thread(snapshot.threads, first)};
        const auto* b{find_thread(snapshot.threads, second)};
        return a && b && a->alive && b->alive && a->waiting_on && b->waiting_on
            && a->lock_owner_id == second && b->lock_owner_id == first
            && a->lock_owner_name == b->name && b->lock_owner_name == a->name
            && a->name == std::format("{}a", prefix) && b->name == std::format("{}b", prefix)
            && snapshot.capture_finished >= snapshot.capture_started;
    }

    auto worker(void*) -> DWORD
    {
        const auto* path{std::getenv("VMHOOK_TEST_RESULT")};
        if (!path) { return 1; }
        std::ofstream output{path};
        output.setf(std::ios::unitbuf);
        bool valid{true};
        const auto check = [&](const std::string_view name, const bool passed)
        {
            output << std::format("{}={}\n", name, passed ? "pass" : "fail");
            valid = valid && passed;
        };
        try
        {
            bool options_valid{true};
            for (const auto options : {
                vmhook::deadlock_options{vmhook::deadlock_scope::monitors, 0, 0},
                vmhook::deadlock_options{vmhook::deadlock_scope::monitors, 65537, 0},
                vmhook::deadlock_options{vmhook::deadlock_scope::monitors, 1, -1},
                vmhook::deadlock_options{vmhook::deadlock_scope::monitors, 1, 1025},
                vmhook::deadlock_options{static_cast<vmhook::deadlock_scope>(99), 1, 0}})
            { options_valid = refused([&] { (void)vmhook::observe_deadlocks(options); }) && options_valid; }
            check("deadlock_options", options_valid
                && !vmhook::detail::native_backend::current_context_lifetime());
            check("deadlock_context_refusal", refused([] { (void)vmhook::observe_deadlocks(); })
                && !vmhook::detail::native_backend::current_context_lifetime());
            vmhook::deadlock_snapshot copied{};
            {
                vmhook::native_thread_context context{"VMHook-deadlock-observation"};
                if (!context.valid() || !vmhook::register_class<fixture>("DeadlockFixture"))
                { throw std::runtime_error{"deadlock fixture registration refused"}; }
                const auto baseline{vmhook::observe_deadlocks()};
                const auto monitor_baseline{vmhook::observe_deadlocks({vmhook::deadlock_scope::monitors})};
                check("deadlock_empty", baseline.complete && baseline.detected_thread_count == 0
                    && baseline.threads.empty() && !baseline.thread_limit && !baseline.thread_info_raced
                    && monitor_baseline.complete && monitor_baseline.threads.empty());
                const auto threads{vmhook::observe_java_threads()};
                const auto blocked{std::find_if(threads.begin(), threads.end(), [](const auto& thread)
                    { return thread.name == "VMHook-contention-contender"; })};
                const auto owner{fixture::id("contention_owner_id")};
                check("deadlock_normal_contention", blocked != threads.end() && blocked->alive
                    && blocked->state == "BLOCKED" && blocked->waiting_on
                    && blocked->waiting_on->class_name == "java.lang.Object"
                    && blocked->waiting_on->identity_hash == fixture::hash("contention_hash")
                    && blocked->lock_owner_id == owner
                    && blocked->lock_owner_name == "VMHook-contention-holder");
                const auto virtual_id{fixture::id("virtual_id")};
                const auto feature{vmhook::runtime_capabilities().java_feature};
                check("deadlock_platform_scope", feature < 21 ? virtual_id == 0
                    : virtual_id > 0 && find_thread(threads, virtual_id) == nullptr);

                fixture::start_monitors();
                const auto monitor_a{fixture::id("monitor_a_id")}, monitor_b{fixture::id("monitor_b_id")};
                const auto monitors{vmhook::observe_deadlocks({vmhook::deadlock_scope::monitors})};
                const auto* a{find_thread(monitors.threads, monitor_a)};
                const auto* b{find_thread(monitors.threads, monitor_b)};
                check("deadlock_monitor_cycle", monitors.complete && monitors.detected_thread_count == 2
                    && monitors.threads.size() == 2 && cycle(monitors, monitor_a, monitor_b, "VMHook-monitor-"));
                check("deadlock_monitor_identity", a && b && a->waiting_on && b->waiting_on
                    && a->waiting_on->class_name == "java.lang.Object"
                    && b->waiting_on->class_name == "java.lang.Object"
                    && a->waiting_on->identity_hash == fixture::hash("monitor_right_hash")
                    && b->waiting_on->identity_hash == fixture::hash("monitor_left_hash")
                    && a->state == "BLOCKED" && b->state == "BLOCKED"
                    && !a->stack.empty() && !b->stack.empty());

                fixture::start_synchronizers();
                const auto synchronizer_a{fixture::id("synchronizer_a_id")};
                const auto synchronizer_b{fixture::id("synchronizer_b_id")};
                copied = vmhook::observe_deadlocks();
                check("deadlock_synchronizer_cycle", copied.complete && copied.detected_thread_count == 4
                    && copied.threads.size() == 4 && cycle(copied, monitor_a, monitor_b, "VMHook-monitor-")
                    && cycle(copied, synchronizer_a, synchronizer_b, "VMHook-synchronizer-"));
                const auto* synchronizer{find_thread(copied.threads, synchronizer_a)};
                check("deadlock_synchronizer_identity", synchronizer && synchronizer->waiting_on
                    && synchronizer->waiting_on->class_name.starts_with("java.util.concurrent.locks.ReentrantLock$")
                    && synchronizer->state == "WAITING");
                const auto only_monitors{vmhook::observe_deadlocks({vmhook::deadlock_scope::monitors})};
                check("deadlock_scope_distinction", only_monitors.complete
                    && only_monitors.scope == vmhook::deadlock_scope::monitors
                    && only_monitors.detected_thread_count == 2 && only_monitors.threads.size() == 2
                    && find_thread(only_monitors.threads, synchronizer_a) == nullptr
                    && copied.scope == vmhook::deadlock_scope::monitors_and_synchronizers);
                const auto bounded{vmhook::observe_deadlocks({vmhook::deadlock_scope::monitors_and_synchronizers, 1, 0})};
                check("deadlock_limits", !bounded.complete && bounded.thread_limit && !bounded.thread_info_raced
                    && bounded.detected_thread_count == 4 && bounded.threads.size() == 1
                    && bounded.max_threads == 1 && bounded.max_depth == 0
                    && bounded.threads.front().stack.empty() && bounded.threads.front().waiting_on);
                const auto shallow{vmhook::observe_deadlocks({vmhook::deadlock_scope::monitors_and_synchronizers, 256, 1})};
                check("deadlock_stack_budget", shallow.complete && shallow.threads.size() == 4 && shallow.max_depth == 1
                    && std::all_of(shallow.threads.begin(), shallow.threads.end(),
                        [](const auto& thread) { return thread.stack.size() == 1; }));
                const auto unsupported{fixture::bean("unsupported_bean")};
                check("deadlock_unsupported_scope", refused([&]
                    { (void)vmhook::detail::observe_deadlocks_from_bean(unsupported, {}); }));
                const auto raced{fixture::bean("raced_bean")};
                const auto vanished{vmhook::detail::observe_deadlocks_from_bean(raced, {})};
                check("deadlock_raced_info", !vanished.complete && vanished.thread_info_raced && !vanished.thread_limit
                    && vanished.detected_thread_count == 1 && vanished.threads.size() == 1
                    && vanished.threads.front().id > 0 && !vanished.threads.front().alive
                    && vanished.threads.front().name.empty() && !vanished.threads.front().waiting_on
                    && !vanished.threads.front().lock_owner_id && !vanished.threads.front().lock_owner_name);

                // Warm all caches and the peak local-root capacity before
                // faulting allocation and checking root-resource plateaus.
                (void)vmhook::observe_deadlocks();
                (void)vmhook::observe_deadlocks();
                const auto roots{vmhook::root_resources()};
                bool allocation{true}, cleanup{true}, retry{true};
                std::size_t checked_failures{}, completed_after_consumed_fault{};
                for (std::size_t fault{}; fault < 48; ++fault)
                {
                    fail_countdown = fault;
                    fail_allocation = true;
                    bool original{}, succeeded{};
                    try
                    {
                        const auto observed{vmhook::observe_deadlocks()};
                        succeeded = observed.complete && observed.detected_thread_count == 4
                            && observed.threads.size() == 4;
                    }
                    catch (const allocation_marker&) { original = true; }
                    catch (const std::bad_alloc&) { original = false; }
                    const bool consumed{!fail_allocation};
                    fail_allocation = false;
                    allocation = allocation && consumed && (original || succeeded);
                    checked_failures += original ? 1 : 0;
                    completed_after_consumed_fault += succeeded && consumed ? 1 : 0;
                    const auto thread{vmhook::detail::private_native::rt().current_thread()};
                    const auto now{vmhook::root_resources()};
                    output << std::format("allocation_fault_{}={}\n", fault,
                        original ? "checked_exception" : succeeded && consumed ? "completed_after_consumed_fault" : "unexpected");
                    cleanup = cleanup && vmhook::detail::private_native::thread_state(thread) == 4
                        && !vmhook::detail::private_native::pending_exception(thread)
                        && now.context_live_ticket_slots == roots.context_live_ticket_slots;
                    const auto again{vmhook::observe_deadlocks()};
                    retry = retry && again.complete && again.detected_thread_count == 4;
                }
                // Result-vector reservation is a checked allocation owned by
                // this API, rather than the existing noexcept root-release
                // bookkeeping that may deliberately retain a scanned slot.
                fail_size = 4 * sizeof(vmhook::java_thread_snapshot);
                fail_countdown = 0;
                fail_allocation = true;
                bool result_buffer_failed{};
                try { (void)vmhook::observe_deadlocks(); }
                catch (const allocation_marker&) { result_buffer_failed = true; }
                catch (const std::bad_alloc&) {}
                const bool result_buffer_consumed{!fail_allocation};
                fail_allocation = false;
                fail_size = 0;
                output << std::format("allocation_checked_failures={}\nallocation_completed_after_consumed_fault={}\n",
                    checked_failures, completed_after_consumed_fault);
                check("deadlock_cpp_allocation", allocation && checked_failures >= 16
                    && checked_failures + completed_after_consumed_fault == 48
                    && result_buffer_failed && result_buffer_consumed);
                check("deadlock_failure_cleanup", cleanup);
                check("deadlock_failure_retry", retry);
                // Resource equality is measured after draining on the owner;
                // queued releases and slots retained by a bookkeeping OOM are
                // separate from live tickets, and retire at context closure.
                (void)vmhook::observe_deadlocks();
                { vmhook::detail::native_backend::operation drain{}; }
                const auto stable{vmhook::root_resources()};
                for (unsigned iteration{}; iteration < 24; ++iteration)
                {
                    const auto again{vmhook::observe_deadlocks()};
                    if (!again.complete || again.detected_thread_count != 4)
                    { throw std::runtime_error{"deadlock resource retry failed"}; }
                    vmhook::detail::native_backend::operation drain{};
                }
                const auto after{vmhook::root_resources()};
                check("deadlock_resources", stable.context_allocated_slots == after.context_allocated_slots
                    && stable.context_live_ticket_slots == after.context_live_ticket_slots
                    && stable.context_occupied_ticket_slots == after.context_occupied_ticket_slots
                    && stable.context_pending_releases == 0 && after.context_pending_releases == 0);
                check("deadlock_context_closed", context.close() && !vmhook::root_resources().context_active
                    && !vmhook::detail::native_backend::current_context_lifetime());
            }
            check("deadlock_copied_lifetime", copied.complete && copied.threads.size() == 4
                && std::all_of(copied.threads.begin(), copied.threads.end(), [](const auto& thread)
                    { return thread.alive && !thread.name.empty() && thread.waiting_on
                        && thread.lock_owner_id && thread.lock_owner_name && !thread.stack.empty(); }));
            check("deadlock_closed_refusal", refused([] { (void)vmhook::observe_deadlocks(); }));
            check("deadlock_complete", valid);
            return valid ? 0 : 2;
        }
        catch (const std::exception& error)
        {
            fail_allocation = false;
            output << std::format("exception={}\n", error.what());
            return 3;
        }
    }
}

extern "C" BOOL __attribute__((ms_abi)) DllMain(HMODULE module, DWORD reason, void*)
{
    if (reason == 1)
    {
        DisableThreadLibraryCalls(module);
        if (const HANDLE thread{CreateThread(nullptr, 0, worker, nullptr, 0, nullptr)}) { CloseHandle(thread); }
    }
    return true;
}
