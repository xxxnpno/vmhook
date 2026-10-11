#include "support.hpp"

auto run_example(example::report& results) -> void
{
    vmhook::native_thread_context java{"VMHook-example-worker"};
    results.check("native_context", java.valid());
    example::register_wrappers(results);

    const auto instance = vmhook::make_unique<example::fixture>(std::int32_t{10});
    results.check("construction", instance && instance->get_instance() != nullptr);
    results.check("call", instance->add(5) == 15 && instance->value() == 15);
    results.check("same_thread", example::fixture::thread_name() == "VMHook-example-worker");
    results.check("static_field", example::fixture::get_total() == 100);
    results.check("no_hooks", vmhook::verify_hooks() == 0);
}
