#include "support.hpp"

auto run_example(example::report& results) -> void
{
    vmhook::native_thread_context java{"VMHook-example-collections"};
    results.check("native_context", java.valid());
    example::register_wrappers(results);

    const auto players = example::fixture::players();
    results.check("collection", players.size() == 3 && players[0] && players[1] && players[2]);
    results.check("values", players[0]->id() == 7 && players[1]->id() == 11);
    results.check("java_null_element", players[2]->get_instance() == nullptr);

    const auto echoed = example::fixture::echo(players[0]);
    results.check("object_argument", echoed && echoed->id() == 7);
    const auto java_null = std::make_unique<example::player>(nullptr);
    const auto returned_null = example::fixture::echo(java_null);
    results.check("java_null_result", returned_null && returned_null->get_instance() == nullptr);
}
