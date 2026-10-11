#include "support.hpp"

namespace { std::atomic<std::uint64_t> definitions{}; }

auto run_example(example::report& results) -> void
{
    vmhook::native_thread_context java{"VMHook-example-observer"};
    results.check("native_context", java.valid());
    example::register_wrappers(results);
    const auto observer = vmhook::on_class_loaded([](const vmhook::class_load_event& event) noexcept
    {
        // Copy/count what is needed here. VM metadata is borrowed for this
        // notification; calling Java or installing a hook here is forbidden.
        if (event.internal_name == "ExampleFixture$Later")
        { definitions.fetch_add(1, std::memory_order_relaxed); }
    });
    results.check("observer", observer != 0);
    results.check("field_hook", vmhook::hook_field<example::fixture, std::int32_t>("total",
        [](vmhook::field_access<std::int32_t>& event)
        {
            if (event.is_write() && event.value() < 0) { event.prevent(); }
            else if (event.is_read()) { event.set(42); }
        }));
    results.check("read_replacement", example::fixture::read_total() == 42);
    example::fixture::write_total(-1);
    results.check("write_prevention", example::fixture::get_total() == 100);
    example::fixture::load_later();
    results.check("class_notification", definitions.load(std::memory_order_relaxed) == 1);
    results.check("field_removal", vmhook::unhook_field<example::fixture>("total"));
    results.check("observer_removal", vmhook::remove_class_observer(observer));
    results.check("restored", example::fixture::read_total() == 100);
}
