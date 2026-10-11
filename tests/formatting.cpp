#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
import vmhook.windows;
#else
#include <vmhook/vmhook.hpp>
#endif

struct diagnostic_value { std::int32_t value{}; };

template<>
struct std::formatter<diagnostic_value> : std::formatter<std::int32_t>
{
    auto format(const diagnostic_value& value, std::format_context& context) const
    { return std::formatter<std::int32_t>::format(value.value, context); }
};

namespace
{
    class injected_failure final : public std::bad_alloc {};
    std::atomic<bool> fail_next_allocation{};
    std::atomic<bool> fail_every_allocation{};
    std::atomic<unsigned> rejected_allocations{};
}

[[gnu::noinline]] auto operator new(const std::size_t size) -> void*
{
    if (fail_every_allocation.load() || fail_next_allocation.exchange(false))
    { ++rejected_allocations; throw injected_failure{}; }
    if (void* result = std::malloc(size ? size : 1)) { return result; }
    throw std::bad_alloc{};
}
[[gnu::noinline]] auto operator new[](const std::size_t size) -> void* { return ::operator new(size); }
[[gnu::noinline]] auto operator delete(void* value) noexcept -> void { std::free(value); }
[[gnu::noinline]] auto operator delete[](void* value) noexcept -> void { std::free(value); }
[[gnu::noinline]] auto operator delete(void* value, std::size_t) noexcept -> void { std::free(value); }
[[gnu::noinline]] auto operator delete[](void* value, std::size_t) noexcept -> void { std::free(value); }

int main()
{
    int failures{};
    const auto check = [&](const bool correct, const char* description)
    {
        if (!correct) { std::cerr << std::format("FAIL: {}\n", description); ++failures; }
    };
    static_assert(noexcept(vmhook::detail::log_line(std::declval<std::format_string<int>>(), 1)));
    static_assert(noexcept(vmhook::detail::log_line_runtime(std::string_view{"{}"}, 1)));
    check(vmhook::detail::format_log("{} {:016x} {:.2f} {{}}", std::string_view{"value"},
          std::uint64_t{0xabcdef}, 1.25) == "value 0000000000abcdef 1.25 {}",
          "literal formats preserve width, scalar precision, and escaped braces");
    int value{};
    check(vmhook::detail::format_log("{:p}", static_cast<const void*>(&value))
          == std::format("{:p}", static_cast<const void*>(&value)),
          "pointer diagnostics use an explicitly formattable void pointer");
    check(vmhook::detail::format_log("{:04x}", diagnostic_value{42}) == "002a",
          "custom formatters receive the literal format specification");
    std::string dynamic_format{"{}:{:04x}"};
    check(vmhook::detail::format_log_runtime(dynamic_format, std::string_view{"field"}, 42)
          == "field:002a", "dynamic format strings use standard formatting semantics");
    bool invalid_format{};
    try { (void)vmhook::detail::format_log_runtime("{:bad}", 1); }
    catch (const std::format_error&) { invalid_format = true; }
    check(invalid_format, "an invalid dynamic format is reported instead of returning raw text");
    check(vmhook::object_id{}.to_string() == "oid:0000000000000000"
          && vmhook::object_id{0x0123456789abcdef}.to_string() == "oid:0123456789abcdef"
          && vmhook::object_id{std::numeric_limits<std::uint64_t>::max()}.to_string()
             == "oid:ffffffffffffffff",
          "object identifiers retain exactly sixteen lowercase hexadecimal digits");
    const std::string long_value(512, 'x');
    bool literal_allocation{};
    fail_next_allocation.store(true);
    try { (void)vmhook::detail::format_log("{}", long_value); }
    catch (const injected_failure&) { literal_allocation = true; }
    fail_next_allocation.store(false);
    check(literal_allocation, "literal formatting propagates the original allocation exception");
    bool dynamic_allocation{};
    fail_next_allocation.store(true);
    try { (void)vmhook::detail::format_log_runtime("{}", long_value); }
    catch (const injected_failure&) { dynamic_allocation = true; }
    fail_next_allocation.store(false);
    check(dynamic_allocation, "dynamic formatting propagates the original allocation exception");
    bool identity_allocation{};
    fail_next_allocation.store(true);
    try { (void)vmhook::object_id{1}.to_string(); }
    catch (const injected_failure&) { identity_allocation = true; }
    fail_next_allocation.store(false);
    check(identity_allocation, "object identifier rendering preserves allocation failure");
    // Warm stream initialization before faulting formatting at the logging boundary.
    vmhook::detail::log_line("Formatting boundary ready");
    fail_next_allocation.store(true);
    vmhook::detail::log_line("{}", long_value);
    const bool literal_consumed{!fail_next_allocation.exchange(false)};
    check(literal_consumed == vmhook::debug_logs,
          "enabled logging contains allocation failure; disabled logging does not allocate");
    fail_next_allocation.store(true);
    vmhook::detail::log_line_runtime("{}", long_value);
    const bool dynamic_consumed{!fail_next_allocation.exchange(false)};
    check(dynamic_consumed == vmhook::debug_logs,
          "dynamic logging contains allocation failure at its noexcept boundary");
    rejected_allocations.store(0);
    fail_every_allocation.store(true);
    vmhook::detail::log_line("Persistent allocation failure in diagnostic: {}", long_value);
    fail_every_allocation.store(false);
    check((rejected_allocations.load() != 0) == vmhook::debug_logs,
          "logging survives persistent allocation failure without an allocating fallback");
    vmhook::detail::log_line_runtime("{:bad}", 1);
    return failures ? 1 : 0;
}
