#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
#else
#include <vmhook/vmhook.hpp>
#endif

static_assert(vmhook::detail::container_wrapper<vmhook::java_string>);
static_assert(vmhook::detail::container_wrapper<vmhook::class_object>);
static_assert(vmhook::detail::container_wrapper<vmhook::class_loader>);
static_assert(vmhook::detail::container_wrapper<vmhook::throwable>);
static_assert(vmhook::detail::container_wrapper<vmhook::java_enum>);
static_assert(vmhook::detail::container_wrapper<vmhook::optional>);
static_assert(vmhook::detail::container_wrapper<vmhook::byte_buffer>);
static_assert(std::is_same_v<decltype(vmhook::java_integer::value_of(1)), std::unique_ptr<vmhook::java_integer>>);

auto exercise_standard_wrappers(const std::unique_ptr<vmhook::java_string>& text,
                               const std::unique_ptr<vmhook::class_loader>& loader) -> void
{
    (void)text->length(); (void)text->char_at(0); (void)text->to_string();
    (void)text->to_utf16(); (void)text->substring(0, 0); (void)text->starts_with("a"); (void)text->ends_with("z");
    const auto type = vmhook::class_object::for_name("java.lang.String", loader);
    (void)type->name(); (void)type->is_array(); (void)type->is_primitive(); (void)type->is_interface();
    (void)type->loader(); (void)type->super_class(); (void)type->component_type();
    const std::unique_ptr<vmhook::class_object> loaded = loader->load_class("java.lang.String");
    const auto present = vmhook::optional::of_nullable(text);
    const std::unique_ptr<vmhook::java_string> contained = present->get();
    const std::unique_ptr<vmhook::java_string> fallback = present->or_else(text);
    (void)present->is_present(); (void)present->is_empty(); (void)vmhook::optional::empty();
    const auto heap = vmhook::byte_buffer::allocate(3);
    const auto direct = vmhook::byte_buffer::allocate_direct(3);
    direct->put(std::array<std::int8_t, 3>{1, 2, 3}); direct->position(0);
    (void)direct->position(); (void)direct->capacity(); (void)direct->remaining(); (void)direct->is_direct();
    (void)direct->get(1); direct->put(1, 42); (void)direct->to_vector();
    (void)heap; (void)contained; (void)fallback; (void)loaded;
}
int main()
{
    try { (void)vmhook::java_integer::value_of(1); }
    catch (const vmhook::exception&) { return 0; }
    return 1;
}
