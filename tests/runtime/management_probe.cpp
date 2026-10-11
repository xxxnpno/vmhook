#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
import vmhook.windows;
#else
#include <vmhook/vmhook.hpp>
#endif

namespace
{
    class fixture final : public vmhook::object<fixture>
    {
    public:
        using vmhook::object<fixture>::object;
        static auto collect() -> void
        { (void)static_members::get_method("collect", "()V")->call(); }
        static auto current_thread_id() -> std::int64_t
        { return static_members::get_method("currentThreadId", "()J")->call(); }
        static auto allocate_burst() -> void
        { (void)static_members::get_method("allocateBurst", "()V")->call(); }
    };

    auto worker(void*) -> DWORD
    {
        const auto* path{ std::getenv("VMHOOK_MANAGEMENT_REPORT") };
        std::ofstream output{ path ? path : "vmhook-management-report.txt" };
        output.setf(std::ios::unitbuf);
        bool valid{ true };
        const auto check = [&](std::string_view name, bool passed)
        { output << name << '=' << (passed ? "pass" : "fail") << '\n'; valid = valid && passed; };
        try
        {
            check("passive_preflight", !vmhook::detail::native_backend::current_context_lifetime());
            const auto capabilities{ vmhook::runtime_capabilities() };
            check("exact_runtime_identity", capabilities.jvm_found && capabilities.private_abi_verified
                && capabilities.java_feature >= 8 && capabilities.java_feature <= 27
                && capabilities.codeview_age > 0 && capabilities.image_size > 0
                && capabilities.gc_flags_resolved);
            check("preflight_no_attachment", !vmhook::detail::native_backend::current_context_lifetime());
            vmhook::native_thread_context context{ "VMHook-management" };
            if (!context.valid() || !vmhook::register_class<fixture>("ManagementFixture"))
            { check("context", false); return 2; }
            const auto before{ vmhook::observe_runtime() };
            check("memory_observation", before.heap.used_bytes >= 0 && before.heap.committed_bytes > 0
                && before.heap.committed_bytes >= before.heap.used_bytes
                && before.non_heap.used_bytes > 0);
            check("runtime_counters", before.uptime_ms > 0 && before.started_unix_ms > 0
                && before.loaded_classes > 0 && before.total_loaded_classes >= before.loaded_classes
                && before.live_threads > 0 && before.peak_threads >= before.live_threads
                && before.total_started_threads >= before.live_threads
                && before.capture_finished >= before.capture_started);
            fixture::collect();
            const auto after{ vmhook::observe_runtime() };
            std::int64_t first_collections{}, later_collections{};
            for (const auto& gc : before.garbage_collectors)
            { if (gc.collection_count) first_collections += *gc.collection_count; }
            for (const auto& gc : after.garbage_collectors)
            { if (gc.collection_count) later_collections += *gc.collection_count; }
            check("real_gc_counters", !after.garbage_collectors.empty()
                && later_collections > first_collections && after.uptime_ms >= before.uptime_ms);
            const auto pools{vmhook::observe_memory_pools()};
            check("memory_pool_observation", !pools.empty() && std::all_of(pools.begin(), pools.end(),
                [](const auto& pool) {
                    return !pool.name.empty() && !pool.type.empty()
                        && (!pool.usage || (pool.usage->used_bytes >= 0
                            && pool.usage->committed_bytes >= pool.usage->used_bytes));
                }));
            const auto id{fixture::current_thread_id()};
            const auto first_allocation{vmhook::observe_thread_allocations()};
            fixture::allocate_burst();
            const auto later_allocation{vmhook::observe_thread_allocations()};
            const auto allocated_bytes = [id](const auto& snapshot) -> std::optional<std::int64_t> {
                for (const auto& thread : snapshot.threads)
                { if (thread.thread_id == id) { return thread.allocated_bytes; } }
                return {};
            };
            const auto first_bytes{allocated_bytes(first_allocation)};
            const auto later_bytes{allocated_bytes(later_allocation)};
            check("real_allocation_counters", first_allocation.supported && first_allocation.enabled
                && later_allocation.supported && later_allocation.enabled && first_bytes && later_bytes
                && *later_bytes - *first_bytes >= 2 * 1024 * 1024
                && later_allocation.capture_finished >= later_allocation.capture_started);
            bool allocation_bounded{};
            try { (void)vmhook::observe_thread_allocations(1); }
            catch (const vmhook::exception&) { allocation_bounded = true; }
            check("bounded_allocation_snapshot", allocation_bounded);
            const auto threads{ vmhook::observe_java_threads() };
            bool found{}, caller{}, leaf{};
            for (const auto& thread : threads)
            {
                if (thread.name != "VMHook-observed-thread") { continue; }
                found = thread.alive && thread.id > 0 && thread.state == "WAITING";
                for (const auto& frame : thread.stack)
                {
                    caller = caller || (frame.class_name == "ManagementFixture" && frame.method_name == "compiledCaller");
                    leaf = leaf || (frame.class_name == "ManagementFixture" && frame.method_name == "blockedLeaf");
                }
            }
            check("java_thread_identity", found);
            check("compiled_java_stack", caller && leaf);
            const auto shallow{ vmhook::observe_java_threads(0) };
            check("zero_depth", !shallow.empty() && std::all_of(shallow.begin(), shallow.end(),
                [](const auto& thread) { return thread.stack.empty(); }));
            bool refused{};
            try { (void)vmhook::observe_java_threads(64, 1); }
            catch (const vmhook::exception&) { refused = true; }
            check("bounded_snapshot", refused);
            check("context_closed", context.close());
            check("copied_observations_survive_close", before.heap.committed_bytes > 0 && found && !threads.empty()
                && !pools.empty() && later_bytes.has_value());
            output << "management_complete=" << (valid ? "pass" : "fail") << '\n';
            return valid ? 0 : 3;
        }
        catch (const std::exception& error)
        { output << "exception=" << error.what() << '\n'; return 4; }
    }
}

extern "C" BOOL __attribute__((ms_abi)) DllMain(HMODULE module, DWORD reason, void*)
{
    if (reason == 1)
    {
        DisableThreadLibraryCalls(module);
        if (const HANDLE thread{ CreateThread(nullptr, 0, worker, nullptr, 0, nullptr) }) { CloseHandle(thread); }
    }
    return true;
}
