#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;

class [[= vmhook::java_class("test/Annotated")]] annotated : public vmhook::object<annotated>
{
public:
    using vmhook::object<annotated>::object;
};
static_assert(vmhook::detail::annotated_class_name<annotated>() == "test/Annotated");
static_assert(vmhook::detail::descriptor_of_v<annotated> == "Ltest/Annotated;");
auto register_annotated_wrapper() -> bool { return vmhook::register_class<annotated>(); }
#else
#include <vmhook/vmhook.hpp>
#endif

auto other_translation_unit() -> bool
{
    return vmhook::decode_array_oop(0) == nullptr
        && vmhook::version == vmhook::make_version(vmhook::version_major,
                                                   vmhook::version_minor,
                                                   vmhook::version_patch);
}
