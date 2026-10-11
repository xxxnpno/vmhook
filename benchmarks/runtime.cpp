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
        std::sort(samples.begin(), samples.end());
        results.number(std::string{name} + ".samples", count);
        results.number(std::string{name} + ".p50_ns", samples[(count - 1) * 50 / 100]);
        results.number(std::string{name} + ".p99_ns", samples[(count - 1) * 99 / 100]);
    }
}

auto run_example(example::report& results) -> void
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
