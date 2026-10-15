#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
import vmhook.windows;
#else
#include <vmhook/vmhook.hpp>
#include <fstream>
#include <cstdlib>
#include <new>
#endif

namespace {
thread_local bool fail_allocation{};
thread_local std::size_t fail_countdown{};
thread_local bool count_allocation{};
thread_local std::size_t allocation_count{};
struct allocation_marker final : std::bad_alloc {
    auto what() const noexcept -> const char* override { return "collection views allocation marker"; }
};
}
__attribute__((noinline)) void* operator new(std::size_t size) {
    if (count_allocation) ++allocation_count;
    if (fail_allocation) {
        if (fail_countdown == 0) { fail_allocation = false; throw allocation_marker{}; }
        --fail_countdown;
    }
    if (void* value = std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc{};
}
__attribute__((noinline)) void operator delete(void* value) noexcept { std::free(value); }
__attribute__((noinline)) void operator delete(void* value, std::size_t) noexcept { std::free(value); }

namespace {
class player : public vmhook::object<player> {
public:
    using object::object;
    auto id() const -> std::int32_t { return get_field("id")->get(); }
};
using players = std::vector<std::unique_ptr<player>>;
class fixture : public vmhook::object<fixture> {
public:
    using object::object;
    static auto method(std::string_view name) -> vmhook::member_lookup<vmhook::method_proxy>
    { return static_members::get_method(name); }
    static auto field(std::string_view name) -> vmhook::member_lookup<vmhook::field_proxy>
    { return static_members::get_field(name); }
    static auto make(std::int32_t kind, std::int32_t size = 3) -> std::unique_ptr<vmhook::iterable>
    { return method("make_iterable")->call(kind, size); }
    static auto collect() -> void { (void)method("collect")->call(); }
    static auto count(std::int32_t index) -> std::int32_t { return method("counter")->call(index); }
};
auto require(bool condition, std::string_view message) -> void {
    if (!condition) throw std::runtime_error{std::string{message}};
}
template<typename callable> auto refused(callable&& call) -> bool {
    try { call(); } catch (const vmhook::exception& error) { return error.code() != vmhook::error_code::java_exception; }
    return false;
}
template<typename callable> auto java_failure(callable&& call, std::string_view name) -> bool {
    try { call(); }
    catch (const vmhook::exception& error) {
        fixture::collect();
        return error.code() == vmhook::error_code::java_exception && error.has_throwable()
            && error.exception_class() == name && bool(error.get_throwable()->get_instance());
    }
    return false;
}
auto ids(const players& values) -> std::vector<std::int32_t> {
    std::vector<std::int32_t> result;
    for (const auto& value : values) {
        require(bool(value), "Java null became empty C++ owner");
        result.push_back(value->get_instance() ? value->id() : -1);
    }
    return result;
}
auto balanced(const vmhook::root_resource_snapshot& before, const vmhook::root_resource_snapshot& after) -> bool {
    return before.context_live_ticket_slots == after.context_live_ticket_slots
        && after.durable_unreclaimed_slots == 0;
}
auto buffer_case(bool direct) -> std::unique_ptr<vmhook::byte_buffer> {
    auto source = direct ? vmhook::byte_buffer::allocate_direct(8) : vmhook::byte_buffer::allocate(8);
    const std::array<std::int8_t, 8> original{0, 1, 2, 3, 4, 5, 6, 7};
    source->put(original); source->position(2); source->limit(6);
    auto duplicate = source->duplicate(); auto slice = source->slice(); auto read_only = source->as_read_only_buffer();
    require(source->is_direct() == direct && duplicate->is_direct() == direct && slice->is_direct() == direct
        && read_only->is_direct() == direct, "buffer storage kind");
    require(!source->is_read_only() && !duplicate->is_read_only() && !slice->is_read_only()
        && read_only->is_read_only(), "buffer mutability");
    require(duplicate->position() == 2 && duplicate->limit() == 6 && duplicate->capacity() == 8
        && slice->position() == 0 && slice->limit() == 4 && slice->capacity() == 4
        && read_only->position() == 2 && read_only->limit() == 6, "buffer initial windows");
    duplicate->position(3); duplicate->limit(5);
    require(source->position() == 2 && source->limit() == 6 && slice->position() == 0
        && slice->limit() == 4 && read_only->position() == 2 && read_only->limit() == 6,
        "buffer positions/limits unexpectedly shared");
    slice->put(0, 42); duplicate->put(3, 43); fixture::collect();
    require(source->get(2) == 42 && source->get(3) == 43 && read_only->get(2) == 42
        && slice->get(1) == 43 && source->to_vector() == std::vector<std::int8_t>{42, 43, 4, 5}
        && source->position() == 2, "buffer shared bytes or nonadvancing snapshot");
    require(java_failure([&] { read_only->put(2, 9); }, "java/nio/ReadOnlyBufferException"),
        "read-only view accepted write");
    require(java_failure([&] { source->limit(9); }, "java/lang/IllegalArgumentException")
        && source->limit() == 6 && source->position() == 2, "invalid limit accepted or corrupted buffer");
    source->limit(1); require(source->position() == 1 && source->remaining() == 0, "limit did not clamp position");
    source.reset(); duplicate.reset(); read_only.reset(); fixture::collect();
    require(slice->get(0) == 42 && slice->get(1) == 43, "view lost storage after source release/GC");
    return slice;
}
auto worker(void*) -> DWORD {
    char destination[4096]{};
    if (!::GetEnvironmentVariableA("VMHOOK_TEST_RESULT", destination, sizeof destination)) return 1;
    std::ofstream output{destination};
    const auto check = [&](std::string_view label) { output << std::format("{}=pass\n", label) << std::flush; };
    try {
        std::unique_ptr<vmhook::iterable> expired_source;
        std::unique_ptr<vmhook::map_entry> expired_entry;
        std::unique_ptr<vmhook::byte_buffer> expired_buffer;
        using deferred_type = decltype(fixture::method("temporary")->call().to_vector());
        std::optional<deferred_type> expired_conversion;
        {
            vmhook::native_thread_context context{"VMHook-collection-views"};
            require(context.valid(), context.error());
            require(vmhook::register_class<fixture>("CollectionViewsFixture")
                && vmhook::register_class<player>("CollectionViewsFixture$Player"), "fixture registration");
            check("views_context");
            const auto expected = std::vector<std::int32_t>{11, -1, 13};
            const players field_values = fixture::field("custom")->get().to_vector();
            const players method_values = fixture::method("temporary")->call().to_vector();
            auto source = fixture::make(0);
            const players inferred_values = source->to_vector();
            const auto explicit_values = source->to_vector<player>();
            require(ids(field_values) == expected && ids(method_values) == expected
                && ids(inferred_values) == expected && ids(explicit_values) == expected, "Iterable conversions");
            check("iterable_conversion");
            auto deferred = fixture::method("temporary")->call().to_vector();
            fixture::collect(); const players delayed = deferred; fixture::collect();
            require(ids(delayed) == expected && static_cast<bool>(fixture::field("live_roots")->get()), "deferred roots");
            check("iterable_gc_roots");
            const std::unique_ptr<vmhook::iterable> java_null = fixture::field("null_iterable")->get();
            const players null_field = fixture::field("null_iterable")->get().to_vector();
            require(java_null && !java_null->get_instance() && java_null->to_vector<player>().empty()
                && null_field.empty(), "Java-null iterable contract");
            auto null_cursor = fixture::make(1);
            require(refused([&] { (void)null_cursor->to_vector<player>(); }), "null iterator accepted");
            check("iterable_nulls");
            auto exact = fixture::make(0);
            require(ids(exact->to_vector<player>(3)) == expected && fixture::count(0) == 1
                && fixture::count(1) == 4 && fixture::count(2) == 3, "exact Iterable budget");
            auto over = fixture::make(0);
            require(refused([&] { (void)over->to_vector<player>(2); }) && fixture::count(2) == 2
                && fixture::count(1) == 3, "one-over consumed forbidden next");
            auto zero = fixture::make(0);
            require(refused([&] { (void)zero->to_vector<player>(0); }) && fixture::count(2) == 0
                && fixture::count(1) == 1, "zero budget consumed element");
            auto empty = fixture::make(0, 0);
            require(empty->to_vector<player>(0).empty() && fixture::count(2) == 0, "empty zero budget refused");
            auto huge = fixture::make(0);
            require(refused([&] { (void)huge->to_vector<player>(1'048'577); })
                && fixture::count(0) == 0 && fixture::count(2) == 0, "hard limit did not refuse before Java");
            auto infinite = fixture::make(6);
            require(refused([&] { (void)infinite->to_vector<player>(3); }) && fixture::count(2) == 3
                && fixture::count(1) == 4, "infinite iterable exceeded finite budget");
            check("iterable_budgets");
            for (std::int32_t kind = 2; kind <= 4; ++kind) {
                auto throwing = fixture::make(kind); bool original{};
                try { (void)throwing->to_vector<player>(); }
                catch (const vmhook::exception& error) {
                    fixture::collect();
                    original = error.code() == vmhook::error_code::java_exception && error.has_throwable()
                        && static_cast<bool>(fixture::method("same_failure")->call(error.get_throwable(), kind - 2));
                }
                require(original, "Iterator original Java Throwable lost");
            }
            check("iterable_java_failures");
            auto wrong = fixture::make(5);
            require(refused([&] { (void)wrong->to_vector<player>(); }), "wrong runtime element accepted");
            auto mutating = fixture::make(7);
            require(java_failure([&] { (void)mutating->to_vector<player>(); }, "java/util/ConcurrentModificationException"),
                "mutating iterator failed to preserve Java failure");
            check("iterable_type_and_mutation");
            const players snapshot = fixture::field("snapshot_only")->get().to_vector();
            const std::unique_ptr<vmhook::collection> collection = fixture::field("snapshot_only")->get();
            require(ids(snapshot) == std::vector<std::int32_t>{41, -1, 43}
                && ids(collection->to_vector<player>()) == std::vector<std::int32_t>{41, -1, 43},
                "Collection conversion abandoned toArray snapshot");
            check("collection_snapshot_route");
            const std::unique_ptr<vmhook::iterable> defaults = fixture::field("defaults")->get();
            const players default_values = defaults->to_vector();
            const std::unique_ptr<vmhook::iterator> default_cursor = defaults->iterator();
            require(ids(default_values) == std::vector<std::int32_t>{77}
                && java_failure([&] { default_cursor->remove(); }, "java/lang/UnsupportedOperationException"),
                "selected interface defaults unavailable");
            check("iterable_interface_defaults");
            // Warm metadata before accounting and inject distinct C++ allocation sites.
            (void)source->to_vector<player>();
            std::size_t failures{};
            for (const auto* allocation_source : {source.get(), defaults.get()}) {
                (void)allocation_source->to_vector<player>();
                allocation_count = 0; count_allocation = true;
                (void)allocation_source->to_vector<player>();
                count_allocation = false;
                const auto total_allocations = allocation_count;
                require(total_allocations >= 16, "allocation instrumentation unavailable");
                for (std::size_t sample = 0; sample < 32; ++sample) {
                    const auto fault = sample < 16 ? sample : (sample - 16) * (total_allocations - 1) / 15;
                    const auto before = vmhook::root_resources();
                    fail_countdown = fault; fail_allocation = true; bool original{};
                    try { (void)allocation_source->to_vector<player>(); }
                    catch (const allocation_marker&) { original = true; }
                    catch (const std::bad_alloc&) { fail_allocation = false; throw std::runtime_error{"original C++ bad_alloc lost"}; }
                    const bool consumed = !fail_allocation; fail_allocation = false;
                    // Root-ticket release/recycling deliberately contains its own
                    // bookkeeping OOM. Result/lookup failures must retain their
                    // derived identity; contained bookkeeping faults may return.
                    if (original) ++failures;
                    if (!consumed) require(!original, "fault instrumentation inconsistent");
                    const auto thread = vmhook::detail::private_native::rt().current_thread();
                    const auto after = vmhook::root_resources();
                    if (vmhook::detail::private_native::thread_state(thread) != 4
                        || vmhook::detail::private_native::pending_exception(thread) || !balanced(before, after)) {
                        throw std::runtime_error{std::format("allocation sample={} fault={} total={} default={} state={} pending={} live={}/{} occupied={}/{} durable={}/{}",
                            sample, fault, total_allocations, allocation_source == defaults.get(),
                            vmhook::detail::private_native::thread_state(thread),
                            bool(vmhook::detail::private_native::pending_exception(thread)),
                            before.context_live_ticket_slots, after.context_live_ticket_slots,
                            before.context_occupied_ticket_slots, after.context_occupied_ticket_slots,
                            before.durable_unreclaimed_slots, after.durable_unreclaimed_slots)};
                    }
                    const auto wanted = allocation_source == source.get() ? expected : std::vector<std::int32_t>{77};
                    require(ids(allocation_source->to_vector<player>()) == wanted, "allocation retry failed");
                }
            }
            require(failures >= 32, "insufficient allocation fault coverage");
            check("iterable_cpp_allocation");
            const std::unique_ptr<vmhook::map_entry> entry = fixture::field("live_entry")->get();
            const std::unique_ptr<player> key = entry->get_key();
            const std::unique_ptr<player> old_value = entry->get_value();
            const auto replacement = vmhook::make_unique<player>(std::int32_t{63});
            const std::unique_ptr<player> replaced = entry->set_value(replacement);
            fixture::collect();
            require(key && key->id() == 5 && old_value && old_value->id() == 6 && replaced && replaced->id() == 6
                && static_cast<std::int32_t>(fixture::method("mapped_id")->call()) == 63,
                "live Map.Entry did not update backing map");
            const std::unique_ptr<vmhook::map_entry> null_entry = fixture::field("null_entry")->get();
            const std::unique_ptr<player> null_key = null_entry->get_key(), null_value = null_entry->get_value();
            const auto java_null_player = std::make_unique<player>(nullptr);
            const std::unique_ptr<player> previous_null = null_entry->set_value(java_null_player);
            require(null_key && !null_key->get_instance() && null_value && !null_value->get_instance()
                && previous_null && !previous_null->get_instance(), "Map.Entry null ownership");
            std::unique_ptr<player> absent;
            require(refused([&] { (void)entry->set_value(absent); }), "empty C++ owner accepted");
            const std::unique_ptr<vmhook::map_entry> immutable = fixture::field("immutable_entry")->get();
            require(java_failure([&] { (void)immutable->set_value(replacement); }, "java/lang/UnsupportedOperationException"),
                "immutable entry lost Java exception");
            check("map_entry_views");
            auto heap_view = buffer_case(false); check("nio_heap_views");
            auto direct_view = buffer_case(true); check("nio_direct_views");
            std::atomic<bool> foreign_safe{};
            std::thread foreign([&] {
                vmhook::native_thread_context other{"VMHook-views-foreign"};
                foreign_safe = other.valid() && refused([&] { (void)source->to_vector<player>(); })
                    && refused([&] { const players values = deferred; (void)values; })
                    && refused([&] { (void)entry->get_key(); })
                    && refused([&] { (void)direct_view->limit(); });
            });
            foreign.join(); require(foreign_safe, "foreign rooted view accepted");
            check("views_foreign_context");
            expired_source = std::move(source); expired_entry = fixture::field("live_entry")->get();
            expired_buffer = std::move(direct_view); expired_conversion.emplace(std::move(deferred));
        }
        require(refused([&] { (void)expired_source->to_vector<player>(); })
            && refused([&] { const players values = *expired_conversion; (void)values; })
            && refused([&] { (void)expired_entry->get_value(); })
            && refused([&] { (void)expired_buffer->slice(); }), "closed context accepted");
        require(!vmhook::root_resources().context_active, "closed context retained local root arena");
        check("views_expired_context"); check("collection_views_proof");
        return 0;
    } catch (const std::exception& error) {
        fail_allocation = false; output << std::format("exception={}\n", error.what()) << std::flush; return 3;
    }
}
}
extern "C" BOOL __attribute__((ms_abi)) DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == 1) {
        ::DisableThreadLibraryCalls(instance);
        HANDLE thread = ::CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
        if (thread) ::CloseHandle(thread);
    }
    return true;
}
