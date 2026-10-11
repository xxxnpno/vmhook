#include "../examples/support.hpp"

namespace
{
    using clock_type = std::chrono::steady_clock;
    std::uint64_t blackhole{};

    template<typename operation>
    auto measure(example::report& results, std::string_view name, std::size_t count, operation function) -> void
    {
        std::vector<std::uint64_t> samples;
        samples.reserve(count);
        for (std::size_t index = 0; index < count; ++index)
        {
            const auto start = clock_type::now();
            function();
            samples.push_back(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now() - start).count()));
        }
        results.number(std::string{name} + ".first_ns", samples.front());
        samples.erase(samples.begin());
        std::sort(samples.begin(), samples.end());
        results.number(std::string{name} + ".samples", count);
        results.number(std::string{name} + ".repeated_p50_ns", samples[(samples.size() - 1) * 50 / 100]);
        results.number(std::string{name} + ".repeated_p99_ns", samples[(samples.size() - 1) * 99 / 100]);
    }
}

auto run_baseline(example::report& results) -> void
{
    char text[32]{};
    std::size_t iterations = 1000;
    if (::GetEnvironmentVariableA("VMHOOK_BENCHMARK_ITERATIONS", text, sizeof text))
    { iterations = static_cast<std::size_t>(std::stoull(text)); }
    if (iterations < 20 || iterations > 100000) { throw std::runtime_error{"Invalid iteration count"}; }
    results.value("vmhook_version", vmhook::version_string);
    results.value("clock", "steady_clock per-operation wall time, nanoseconds");
    results.number("iterations", iterations);

    // Record cold attachment separately; constructing nested contexts would
    // measure a refusal, so no context exists around this operation.
    measure(results, "context", std::min<std::size_t>(iterations, 100), [&results]
    {
        vmhook::native_thread_context java{"VMHook-benchmark-context"};
        if (!java.valid()) { throw vmhook::exception{java.error()}; }
        ++blackhole;
    });
    vmhook::native_thread_context java{"VMHook-benchmark"};
    results.check("native_context", java.valid());
    example::register_wrappers(results);
    const auto instance = vmhook::make_unique<example::fixture>(std::int32_t{10});
    results.check("construction", instance && instance->value() == 10);
    measure(results, "field_lookup_read", iterations, [&]
    { blackhole += static_cast<std::uint64_t>(instance->value()); });
    measure(results, "method_lookup_call", iterations, [&]
    { blackhole += static_cast<std::uint64_t>(example::fixture::plain(7)); });
    const auto method = example::fixture::static_members::get_method("plain", "(I)I");
    measure(results, "cached_method_call", iterations, [&]
    {
        const auto value = method->call(std::int32_t{7});
        if (!value.was_invoked() || value.threw() || value.result_failed)
        { throw std::runtime_error{"Benchmark Java call failed"}; }
        blackhole += static_cast<std::uint64_t>(static_cast<std::int32_t>(value));
    });
    measure(results, "utf8_string", iterations, [&]
    { blackhole += example::fixture::thread_name().size(); });
    measure(results, "collection_three_wrappers", std::min<std::size_t>(iterations, 1000), [&]
    {
        const auto players = example::fixture::players();
        if (players.size() != 3 || !players[2] || players[2]->get_instance() != nullptr)
        { throw std::runtime_error{"Benchmark collection conversion failed"}; }
        blackhole += players.size();
    });
    results.number("blackhole", blackhole);
    results.check("no_hooks", vmhook::verify_hooks() == 0);
}

namespace
{
    auto elapsed_ns(clock_type::time_point start) -> std::uint64_t
    { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now() - start).count()); }

    auto parameter(const char* name, std::size_t fallback, std::size_t minimum, std::size_t maximum) -> std::size_t
    {
        char text[32]{};
        const auto length = ::GetEnvironmentVariableA(name, text, sizeof text);
        if (length >= sizeof text) { throw std::runtime_error{"Benchmark parameter is too long"}; }
        const auto value = length ? static_cast<std::size_t>(std::stoull(text)) : fallback;
        if (value < minimum || value > maximum) { throw std::runtime_error{"Invalid benchmark parameter"}; }
        return value;
    }

    auto record_hooks(example::report& results, std::string_view name) -> vmhook::hook_resource_stats
    {
        const auto value = vmhook::hook_resources();
        const auto prefix = std::string{name};
        results.number(prefix + ".active_observers", value.active_observers);
        results.number(prefix + ".retained_method_policies", value.retained_method_policies);
        results.number(prefix + ".physical_stubs", value.physical_stubs);
        results.number(prefix + ".retained_snapshots", value.retained_snapshots);
        results.number(prefix + ".dispatch_readers", value.dispatch_readers);
        results.number(prefix + ".active_callbacks", value.active_callbacks);
        results.number(prefix + ".callback_failures", value.callback_failures);
        results.number(prefix + ".dll_must_remain_loaded", value.dll_must_remain_loaded);
        return value;
    }

    auto record_roots(example::report& results, std::string_view name) -> vmhook::root_resource_snapshot
    {
        const auto value = vmhook::root_resources();
        const auto prefix = std::string{name};
        results.number(prefix + ".durable_allocated_slots", value.durable_allocated_slots);
        results.number(prefix + ".durable_leases", value.durable_leases);
        results.number(prefix + ".durable_occupied_slots", value.durable_occupied_slots);
        results.number(prefix + ".durable_reusable_slots", value.durable_reusable_slots);
        results.number(prefix + ".durable_pending_releases", value.durable_pending_releases);
        results.number(prefix + ".durable_unreclaimed_slots", value.durable_unreclaimed_slots);
        results.number(prefix + ".durable_internal_slots", value.durable_internal_slots);
        results.number(prefix + ".context_active", value.context_active);
        results.number(prefix + ".context_allocated_slots", value.context_allocated_slots);
        results.number(prefix + ".context_live_ticket_slots", value.context_live_ticket_slots);
        results.number(prefix + ".context_occupied_ticket_slots", value.context_occupied_ticket_slots);
        results.number(prefix + ".context_reusable_slots", value.context_reusable_slots);
        results.number(prefix + ".context_pending_releases", value.context_pending_releases);
        return value;
    }

    auto record_runtime(example::report& results, std::string_view name) -> std::uint64_t
    {
        const auto value = vmhook::observe_runtime();
        const auto prefix = std::string{name};
        results.number(prefix + ".heap_used_bytes", static_cast<std::uint64_t>(value.heap.used_bytes));
        results.number(prefix + ".heap_committed_bytes", static_cast<std::uint64_t>(value.heap.committed_bytes));
        results.number(prefix + ".non_heap_used_bytes", static_cast<std::uint64_t>(value.non_heap.used_bytes));
        results.number(prefix + ".live_java_threads", static_cast<std::uint64_t>(value.live_threads));
        if (value.compilation_time_ms) { results.number(prefix + ".compilation_time_ms", static_cast<std::uint64_t>(*value.compilation_time_ms)); }
        std::uint64_t collections{};
        for (std::size_t index{}; index < value.garbage_collectors.size(); ++index)
        {
            const auto& collector = value.garbage_collectors[index];
            const auto key = prefix + ".collector_" + std::to_string(index);
            results.value(key + ".name", collector.name);
            if (collector.collection_count)
            {
                const auto count = static_cast<std::uint64_t>(*collector.collection_count);
                results.number(key + ".collections", count);
                collections += count;
            }
            if (collector.collection_time_ms)
            { results.number(key + ".collection_time_ms", static_cast<std::uint64_t>(*collector.collection_time_ms)); }
        }
        return collections;
    }

    struct jit_snapshot final
    {
        vmhook::hotspot::method* target{};
        vmhook::hotspot::method* caller{};
        vmhook::hotspot::method* unrelated{};
        bool target_compiled{}, caller_compiled{}, unrelated_compiled{};
        bool target_excluded{}, unrelated_excluded{};
    };

    auto find_method(std::string_view name) -> vmhook::hotspot::method*
    {
        vmhook::detail::native_backend::operation access{};
        auto* const klass = vmhook::detail::registered_class_klass<example::fixture>();
        auto* const methods = klass ? klass->get_methods_ptr() : nullptr;
        const auto count = klass ? klass->get_methods_count() : 0;
        for (std::int32_t index{}; methods && index < count; ++index)
        { if (methods[index] && methods[index]->get_name() == name) { return methods[index]; } }
        throw std::runtime_error{"Benchmark Java method metadata unavailable"};
    }

    auto capture_jit(void* opaque) noexcept -> void
    {
        auto& value = *static_cast<jit_snapshot*>(opaque);
        const auto& config = vmhook::detail::jit_backend::config();
        using vmhook::detail::private_native::load;
        value.target_compiled = load<void*>(value.target, config.method_code_offset) != nullptr;
        value.caller_compiled = load<void*>(value.caller, config.method_code_offset) != nullptr;
        value.unrelated_compiled = load<void*>(value.unrelated, config.method_code_offset) != nullptr;
        value.target_excluded = vmhook::detail::jit_backend::method_is_excluded(value.target);
        value.unrelated_excluded = vmhook::detail::jit_backend::method_is_excluded(value.unrelated);
    }

    auto record_jit(example::report& results, std::string_view name, jit_snapshot& value) -> void
    {
        if (!vmhook::detail::jit_backend::at_safepoint(&capture_jit, &value))
        { throw std::runtime_error{"Benchmark JIT snapshot safepoint failed"}; }
        const auto prefix = std::string{name};
        results.number(prefix + ".target_compiled", value.target_compiled);
        results.number(prefix + ".caller_compiled", value.caller_compiled);
        results.number(prefix + ".unrelated_compiled", value.unrelated_compiled);
        results.number(prefix + ".target_excluded", value.target_excluded);
        results.number(prefix + ".unrelated_excluded", value.unrelated_excluded);
    }

    auto soak(example::report& results, std::size_t cycles, std::size_t batch,
              const std::unique_ptr<example::fixture>& instance) -> void
    {
        const auto initial_collections = record_runtime(results, "soak_before");
        const auto started = clock_type::now();
        vmhook::root_resource_snapshot steady{};
        for (std::size_t cycle{}; cycle < cycles; ++cycle)
        {
            {
                std::vector<std::unique_ptr<example::player>> owners;
                owners.reserve(batch);
                for (std::size_t index{}; index < batch; ++index)
                {
                    const auto id = static_cast<std::int32_t>(cycle * batch + index);
                    auto owner = vmhook::make_unique<example::player>(id);
                    if (!owner || owner->id() != id) { throw std::runtime_error{"Soak construction failed"}; }
                    instance->selected(owner);
                    instance->append(owner);
                    owners.push_back(std::move(owner));
                }
                const auto from_field = instance->selected();
                const auto from_list = instance->live_players();
                auto durable = vmhook::retain(from_field);
                instance->clear_live_players();
                const auto java_null = std::make_unique<example::player>(nullptr);
                instance->selected(java_null);
                example::fixture::collect();
                if (!durable.is_same_object(from_field) || from_list.size() != batch
                    || from_field->id() != static_cast<std::int32_t>((cycle + 1) * batch - 1))
                { throw std::runtime_error{"Soak reference identity failed after collection"}; }
                for (std::size_t index{}; index < batch; ++index)
                {
                    const auto expected = static_cast<std::int32_t>(cycle * batch + index);
                    if (!owners[index] || owners[index]->id() != expected || !from_list[index] || from_list[index]->id() != expected)
                    { throw std::runtime_error{"Soak collection owner failed after collection"}; }
                }
                const auto null_field = instance->selected();
                if (!null_field || null_field->get_instance() != nullptr)
                { throw std::runtime_error{"Soak Java-null contract failed"}; }
                durable.reset();
                blackhole += from_list.size();
            }
            // A normal Java operation drains context releases; retaining a
            // temporary durable reference drains keeper releases. The getter
            // itself deliberately performs neither action.
            blackhole += static_cast<std::uint64_t>(instance->value());
            const auto drain = vmhook::retain(instance);
            const auto current = vmhook::root_resources();
            if (cycle == 0) { steady = record_roots(results, "roots_after_warm_cycle"); }
            else if (current.durable_allocated_slots != steady.durable_allocated_slots
                || current.durable_leases != steady.durable_leases
                || current.context_allocated_slots != steady.context_allocated_slots
                || current.context_live_ticket_slots != steady.context_live_ticket_slots)
            { throw std::runtime_error{"Soak root slots or leases grew after the warm cycle"}; }
        }
        example::fixture::collect();
        results.number("soak.elapsed_ns", elapsed_ns(started));
        results.number("soak.cycles", cycles);
        results.number("soak.batch_size", batch);
        results.number("soak.cpp_player_owners_created", cycles * batch);
        record_roots(results, "roots_after_soak");
        results.check("soak_root_capacity_reused", true);
        results.check("soak_gc_progress", record_runtime(results, "soak_after") > initial_collections);
        results.check("soak_ownership", true);
    }
}

auto run_example(example::report& results) -> void
{
    run_baseline(results);
    const auto iterations = parameter("VMHOOK_BENCHMARK_ITERATIONS", 1000, 20, 100000);
    const auto cycles = parameter("VMHOOK_BENCHMARK_SOAK_CYCLES", 8, 2, 10000);
    const auto batch = parameter("VMHOOK_BENCHMARK_SOAK_BATCH", 16, 1, 256);
    results.value("deoptimization_scope", "VM-wide compiled-code deoptimization; target JIT exclusion while policy is retained");
    vmhook::retained_object<example::player> cross_context;
    {
        vmhook::native_thread_context java{"VMHook-benchmark-soak"};
        results.check("soak_native_context", java.valid());
        const auto instance = vmhook::make_unique<example::fixture>(std::int32_t{10});
        measure(results, "object_construction", iterations, []
        { const auto object = vmhook::make_unique<example::player>(std::int32_t{7}); if (object->id() != 7) { throw std::runtime_error{"Construction result failed"}; } ++blackhole; });
        const auto player = vmhook::make_unique<example::player>(std::int32_t{71});
        cross_context = vmhook::retain(player);
        measure(results, "reference_field_write_read", iterations, [&]
        { instance->selected(player); const auto value = instance->selected(); if (!cross_context.is_same_object(value) || value->id() != 71) { throw std::runtime_error{"Reference field failed"}; } ++blackhole; });
        jit_snapshot jit{find_method("plain"), find_method("throughput_loop"), find_method("unrelated_loop")};
        record_jit(results, "jit_before_hook", jit);
        results.check("warmed_java_methods", jit.target_compiled && jit.caller_compiled && jit.unrelated_compiled);
        const auto loop_samples = std::min<std::size_t>(iterations, 32);
        constexpr std::int32_t loop_count{1024};
        results.number("java_loop.operations_per_sample", loop_count);
        const auto expected_sum = static_cast<std::int64_t>(loop_count) * (loop_count + 1) / 2;
        const auto target_loop = [&]
        { const auto value = example::fixture::throughput_loop(loop_count); if (value != expected_sum) { throw std::runtime_error{"Java loop changed semantics"}; } blackhole += static_cast<std::uint64_t>(value); };
        const auto unrelated_result = example::fixture::unrelated_loop(loop_count);
        const auto unrelated_loop = [&]
        { const auto value = example::fixture::unrelated_loop(loop_count); if (value != unrelated_result) { throw std::runtime_error{"Unrelated loop changed semantics"}; } blackhole += static_cast<std::uint64_t>(value); };
        measure(results, "java_target_loop_before_hook", loop_samples, target_loop);
        measure(results, "java_unrelated_loop_before_hook", loop_samples, unrelated_loop);
        const auto before_hooks = record_hooks(results, "hooks_before");
        results.check("initial_hook_resources", before_hooks.active_observers == 0 && before_hooks.physical_stubs == 0);
        const auto observed = std::make_shared<std::atomic<std::uint64_t>>(0);
        const auto install_started = clock_type::now();
        const auto observer = vmhook::add_hook<example::fixture>("plain", "(I)I",
            [observed](vmhook::return_value&, std::int32_t) { observed->fetch_add(1, std::memory_order_relaxed); });
        results.number("method_hook.install_wall_ns", elapsed_ns(install_started));
        results.check("method_hook_installed", observer != 0);
        record_jit(results, "jit_after_install", jit);
        results.check("target_jit_excluded", !jit.target_compiled && jit.target_excluded && !jit.unrelated_excluded);
        measure(results, "java_target_loop_active_hook", loop_samples, target_loop);
        measure(results, "java_unrelated_loop_active_hook", loop_samples, unrelated_loop);
        results.check("entry_count", observed->load(std::memory_order_relaxed) == loop_samples * static_cast<std::uint64_t>(loop_count));
        const auto active = record_hooks(results, "hooks_active");
        results.check("active_hook_resources", active.active_observers == 1 && active.physical_stubs == 1);
        measure(results, "shared_observer_add_remove", std::min<std::size_t>(iterations, 100), []
        {
            const auto id = vmhook::add_hook<example::fixture>("plain", "(I)I", [](vmhook::return_value&, std::int32_t) {});
            if (!id || !vmhook::remove_hook(id)) { throw std::runtime_error{"Observer add/remove failed"}; }
        });
        soak(results, cycles, batch, instance);
        results.check("hook_survives_gc", example::fixture::plain(9) == 10 && observed->load(std::memory_order_relaxed) > loop_samples * static_cast<std::uint64_t>(loop_count));
        results.check("remove_observer", vmhook::remove_hook(observer) && vmhook::reclaim_hook_callbacks());
        const auto removed = record_hooks(results, "hooks_removed");
        results.check("removed_policy_retained", removed.active_observers == 0 && removed.retained_method_policies == 1);
        const auto shutdown_started = clock_type::now();
        vmhook::shutdown_hooks();
        results.number("method_hook.shutdown_wall_ns", elapsed_ns(shutdown_started));
        record_jit(results, "jit_after_shutdown", jit);
        const auto stopped = record_hooks(results, "hooks_stopped");
        results.check("shutdown_resources", stopped.active_observers == 0 && stopped.physical_stubs == 0
            && stopped.retained_method_policies == 0 && !jit.target_excluded && stopped.dll_must_remain_loaded);
        measure(results, "java_target_loop_after_shutdown", loop_samples, target_loop);
        measure(results, "java_unrelated_loop_after_shutdown", loop_samples, unrelated_loop);
        results.check("hooks_integrity", vmhook::verify_hooks() == 0);
    }
    {
        vmhook::native_thread_context java{"VMHook-benchmark-retained-root"};
        results.check("fresh_context", java.valid());
        example::fixture::collect();
        const auto acquired = cross_context.acquire();
        results.check("retained_cross_context", acquired && acquired->id() == 71 && cross_context.is_same_object(acquired));
        cross_context.reset();
        results.check("retained_released", !cross_context.valid());
    }
    const auto closed = record_roots(results, "roots_after_contexts_closed");
    results.check("contexts_closed", !closed.context_active);
    results.number("blackhole", blackhole);
}
