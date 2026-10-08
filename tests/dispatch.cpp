#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
import vmhook.windows;
#else
#include <vmhook/vmhook.hpp>
#endif

int main()
{
    using namespace std::chrono_literals;
    int failures{};
    const auto check = [&](bool condition, const char* description)
    {
        if (!condition)
        {
            std::cerr << "FAIL: " << description << '\n';
            ++failures;
        }
    };

    check(!vmhook::can_call_java(), "a native test thread has no Java dispatch context");
    auto result = vmhook::dispatch([] { return 42; });
    auto exception = vmhook::dispatch([]() -> int { throw std::runtime_error{"expected"}; });
    check(result.wait_for(0ms) == std::future_status::timeout, "native submission remains pending");
    vmhook::detail::pump_dispatch();
    check(result.wait_for(0ms) == std::future_status::timeout,
          "the pump refuses a native thread without a valid detour");

    // Concurrent producers submit real packaged tasks. Each vector has one
    // writer, while VMHook must serialize access to its bounded shared queue.
    std::array<std::vector<std::future<int>>, 4> futures;
    std::array<std::thread, 4> producers;
    for (std::size_t producer = 0; producer < producers.size(); ++producer)
    {
        producers[producer] = std::thread([&, producer]
        {
            for (int item = 0; item < 255; ++item)
            {
                futures[producer].push_back(vmhook::dispatch([item] { return item; }));
            }
        });
    }
    for (auto& producer : producers) { producer.join(); }
    auto penultimate = vmhook::dispatch([] {});
    auto last = vmhook::dispatch([] {});
    bool overflow_refused{};
    try { (void)vmhook::dispatch([] {}); }
    catch (const vmhook::exception&) { overflow_refused = true; }
    check(overflow_refused, "the queue refuses submission beyond its capacity");

    // Drain the queue in the fixture to test future ownership and exception
    // transport. This does not simulate or claim to validate a JavaThread.
    std::vector<std::function<void()>> tasks;
    {
        std::lock_guard lock{vmhook::detail::dispatch_mutex};
        tasks.swap(vmhook::detail::dispatch_queue);
    }
    check(tasks.size() == 1024, "no tasks are lost by concurrent submissions");
    for (auto& task : tasks) { task(); }
    check(result.get() == 42, "a task result reaches its future");
    bool exception_received{};
    try { (void)exception.get(); }
    catch (const std::runtime_error& error)
    {
        exception_received = std::string_view{error.what()} == "expected";
    }
    check(exception_received, "a callable exception reaches its future");
    for (auto& sequence : futures)
    {
        for (std::size_t item = 0; item < sequence.size(); ++item)
        {
            check(sequence[item].get() == static_cast<int>(item), "each producer result is preserved");
        }
    }
    penultimate.get();
    last.get();
    return failures ? 1 : 0;
}
