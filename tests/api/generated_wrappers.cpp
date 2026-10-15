#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
import vmhook.generated_fixture;
#else
#include "generated_wrappers.hpp"
#endif

using generated_fixture = generator_fixtures::generated_wrapper_fixture;
using generated_unicode = generator_fixtures::generated_wrapper_fixture_type;
using generated_containers = generator_fixtures::generated_wrapper_fixture_container_types;
using generated_registration_name = generator_fixtures::java_register_java_classes;
using generated_std_name = generator_fixtures::std;
using generated_vmhook_name = generator_fixtures::vmhook;

static_assert(std::is_base_of_v<vmhook::object<generated_fixture>, generated_fixture>);
static_assert(std::is_base_of_v<vmhook::object<generated_registration_name>, generated_registration_name>);
static_assert(std::is_base_of_v<vmhook::object<generated_std_name>, generated_std_name>);
static_assert(std::is_base_of_v<vmhook::object<generated_vmhook_name>, generated_vmhook_name>);
static_assert(std::is_same_v<decltype(std::declval<const generated_fixture&>().java_and_eq()), std::int32_t>);
static_assert(std::is_same_v<decltype(std::declval<const generated_fixture&>().java_generated_wrapper_fixture()), std::int32_t>);
static_assert(std::is_same_v<decltype(std::declval<const generated_fixture&>().get_native_root()),
                             vmhook::detail::native_backend::root>);
static_assert(std::is_same_v<decltype(std::declval<const generated_containers&>().get_iterable()),
                             std::unique_ptr<vmhook::iterable>>);
static_assert(std::is_same_v<decltype(std::declval<const generated_containers&>().get_entry()),
                             std::unique_ptr<vmhook::map_entry>>);
static_assert(std::is_same_v<decltype(std::declval<const generated_unicode&>().method(
                                 std::declval<const std::unique_ptr<generated_unicode>&>())),
                             std::unique_ptr<generated_unicode>>);

int main()
{
    const generated_fixture java_null;
    return java_null.get_instance() == nullptr && !java_null.get_native_root() ? 0 : 1;
}
