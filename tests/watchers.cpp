#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
import vmhook.windows;
#else
#include <format>
#include <iostream>
#include <vmhook/vmhook.hpp>
#endif

int main()
{
    int failures{};
    const auto check = [&](const bool condition, const char* description)
    {
        if (!condition) { std::cerr << std::format("FAIL: {}\n", description); ++failures; }
    };
    static_assert(!std::is_copy_constructible_v<vmhook::watch_handle>);
    static_assert(!std::is_copy_assignable_v<vmhook::watch_handle>);
    static_assert(std::is_nothrow_move_constructible_v<vmhook::watch_handle>);
    static_assert(std::is_nothrow_move_assignable_v<vmhook::watch_handle>);

    vmhook::watch_handle empty;
    empty.stop();
    check(!empty.running(), "a failed/default observer has no cleanup ownership");

    unsigned removed{};
    std::weak_ptr<int> capture;
    {
        auto resource = std::make_shared<int>(73);
        capture = resource;
        auto control = std::make_shared<vmhook::watch_handle::control_block>();
        control->on_stop = [resource = std::move(resource), &removed]
        {
            if (*resource == 73) { ++removed; }
        };
        vmhook::watch_handle first{std::move(control)};
        vmhook::watch_handle second{std::move(first)};
        check(!first.running() && second.running(),
              "moving an observer transfers its single removal owner");
        first.stop();
        check(removed == 0 && !capture.expired(),
              "stopping a moved-from handle preserves the live observer capture");
        second.stop();
        second.stop();
        check(removed == 1 && !second.running() && capture.expired(),
              "explicit removal runs once and releases captured resources");
    }
    check(removed == 1, "destruction after explicit stop never removes twice");

    unsigned original_removed{}, replacement_removed{};
    {
        auto original = std::make_shared<vmhook::watch_handle::control_block>();
        original->on_stop = [&] { ++original_removed; };
        auto replacement = std::make_shared<vmhook::watch_handle::control_block>();
        replacement->on_stop = [&] { ++replacement_removed; };
        vmhook::watch_handle first{std::move(original)};
        vmhook::watch_handle second{std::move(replacement)};
        first = std::move(second);
        check(original_removed == 1 && replacement_removed == 0
                  && first.running() && !second.running(),
              "move assignment removes the previous observer before taking ownership");
    }
    check(original_removed == 1 && replacement_removed == 1,
          "destruction removes the replacement observer exactly once");
    return failures ? 1 : 0;
}
