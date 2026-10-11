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
    const auto check = [&](bool value, const char* name)
    { if (!value) { std::cerr << std::format("FAIL: {}\n", name); ++failures; } };
    vmhook::bounded_event_queue<std::uint64_t> tiny{2};
    std::uint64_t value{};
    check(!tiny.try_pop(value), "empty queue refuses reads");
    check(tiny.try_push(7) && tiny.try_push(9) && !tiny.try_push(11),
          "bounded writes drop without overwriting unread events");
    check(tiny.dropped() == 1 && tiny.pushed() == 2 && tiny.capacity() == 2,
          "drop and capacity diagnostics are explicit");
    check(tiny.try_pop(value) && value == 7 && tiny.try_push(13)
          && tiny.try_pop(value) && value == 9 && tiny.try_pop(value) && value == 13,
          "slot reuse preserves FIFO publication");
    bool invalid{};
    try { vmhook::bounded_event_queue<std::uint64_t> bad{3}; }
    catch (const std::invalid_argument&) { invalid = true; }
    check(invalid, "invalid queue capacities are refused before allocation");

    constexpr unsigned writers = 4, per_writer = 2048, total = writers * per_writer;
    vmhook::bounded_event_queue<std::uint64_t> concurrent{128};
    std::array<std::atomic<unsigned>, total> seen{};
    std::atomic<unsigned> consumed{};
    std::atomic<bool> corrupt{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    std::vector<std::thread> readers, producers;
    for (unsigned i = 0; i < 2; ++i) readers.emplace_back([&]
    {
        while (consumed.load() < total && std::chrono::steady_clock::now() < deadline)
        {
            std::uint64_t current{};
            if (!concurrent.try_pop(current)) { std::this_thread::yield(); continue; }
            if (current >= total || seen[current].fetch_add(1) != 0) corrupt.store(true);
            ++consumed;
        }
    });
    for (unsigned producer = 0; producer < writers; ++producer) producers.emplace_back([&, producer]
    {
        for (unsigned i = 0; i < per_writer; ++i)
        {
            const std::uint64_t current = producer * per_writer + i;
            while (!concurrent.try_push(current))
            {
                if (std::chrono::steady_clock::now() >= deadline) return;
                std::this_thread::yield();
            }
        }
    });
    for (auto& producer : producers) producer.join();
    for (auto& reader : readers) reader.join();
    check(consumed.load() == total && !corrupt.load()
          && std::all_of(seen.begin(), seen.end(), [](const auto& entry) { return entry.load() == 1; }),
          "multiple producers and consumers publish each accepted event exactly once");
    check(concurrent.pushed() == total && concurrent.popped() == total,
          "concurrent accounting matches accepted traffic");

    std::string name = "example/Player";
    vmhook::class_load_event event{};
    event.internal_name = name; event.event_id = 71;
    event.phase = vmhook::class_load_phase::instance_loaded;
    auto snapshot = vmhook::copy_class_event(event);
    auto record = vmhook::record_class_event(event);
    name.assign("mutated");
    check(snapshot.internal_name == "example/Player" && snapshot.event_id == 71
          && snapshot.bootstrap_loader && record.name() == "example/Player" && !record.truncated,
          "copied class metadata survives its borrowed callback source");
    name.assign(2048, 'x'); event.internal_name = name;
    record = vmhook::record_class_event(event);
    check(record.truncated && record.name().size() == 1024,
          "bounded metadata explicitly records name truncation");
    static_assert(std::is_trivially_copyable_v<vmhook::class_load_record>);
    return failures ? 1 : 0;
}
