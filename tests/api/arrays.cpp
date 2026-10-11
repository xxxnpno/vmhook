#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
#else
#include <vmhook/vmhook.hpp>
#endif

class array_element : public vmhook::object<array_element>
{
public:
    using vmhook::object<array_element>::object;
};

static_assert(vmhook::detail::primitive_array_descriptor<bool> == "[Z");
static_assert(vmhook::detail::primitive_array_descriptor<std::int8_t> == "[B");
static_assert(vmhook::detail::primitive_array_descriptor<std::uint16_t> == "[C");
static_assert(vmhook::detail::primitive_array_descriptor<std::int16_t> == "[S");
static_assert(vmhook::detail::primitive_array_descriptor<std::int32_t> == "[I");
static_assert(vmhook::detail::primitive_array_descriptor<std::int64_t> == "[J");
static_assert(vmhook::detail::primitive_array_descriptor<float> == "[F");
static_assert(vmhook::detail::primitive_array_descriptor<double> == "[D");
static_assert(!vmhook::detail::java_array_element<std::string>);
static_assert(std::is_same_v<decltype(std::declval<const vmhook::array<array_element>&>().get(0)),
                             std::unique_ptr<array_element>>);
static_assert(std::is_same_v<decltype(std::declval<const vmhook::array<std::int32_t>&>().to_vector()),
                             std::vector<std::int32_t>>);
static_assert(std::is_same_v<decltype(std::declval<const vmhook::array<array_element>&>().to_vector()),
                             std::vector<std::unique_ptr<array_element>>>);

auto exercise_array_api(const vmhook::method_proxy& method,
                        const vmhook::field_proxy& field,
                        const std::unique_ptr<array_element>& element) -> void
{
    const std::unique_ptr<vmhook::array<std::int32_t>> integers = field.get();
    const std::unique_ptr<vmhook::array<array_element>> objects = method.call();
    const auto allocated = vmhook::make_array<std::int32_t>(1024);
    allocated->copy_from(std::array<std::int32_t, 3>{1, 2, 3}, 2);
    allocated->set(0, 42);
    (void)allocated->get(0);
    (void)allocated->to_vector();
    const auto flags = vmhook::make_array<bool>(2);
    flags->set(0, true);
    (void)flags->to_vector();
    const auto players = vmhook::make_array<array_element>(2);
    players->set(0, element);
    (void)players->get(1);
    (void)players->to_vector();
    const auto rows = vmhook::make_array<vmhook::array<std::int32_t>>(2);
    rows->set(0, allocated);
    (void)rows->get(0);
    (void)integers; (void)objects;
    // More than eight arguments with wide values and rooted references.
    (void)method.call(std::int32_t{1}, std::int64_t{2}, double{3}, element,
                      std::int32_t{4}, std::int64_t{5}, double{6}, element,
                      std::string{"last"}, std::int32_t{10});
}

int main()
{
    try { (void)vmhook::make_array<std::int32_t>(1); }
    catch (const vmhook::exception&) { return 0; }
    return 1;
}
