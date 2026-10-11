#include "support.hpp"

namespace { std::atomic<std::uint64_t> observed{}; }

auto run_example(example::report& results) -> void
{
    vmhook::native_thread_context java{"VMHook-example-method-hook"};
    results.check("native_context", java.valid());
    example::register_wrappers(results);
    results.check("original", example::fixture::plain(7) == 8);
    results.check("hook", vmhook::hook<example::fixture>("plain", "(I)I",
        [](vmhook::return_value& result, std::int32_t value)
        {
            observed.fetch_add(1, std::memory_order_relaxed);
            if (value == 7) { result.set<std::int32_t>(42); }
        }));
    results.check("replacement", example::fixture::plain(7) == 42);
    results.check("original_body", example::fixture::plain(11) == 12);
    results.check("callback", observed.load(std::memory_order_relaxed) >= 2);
    vmhook::shutdown_hooks();
    results.check("restored", example::fixture::plain(7) == 8);
}
