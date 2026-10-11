#if defined(VMHOOK_CONSUMER_MODULE)
import std;
import vmhook;
#else
#include <vmhook/vmhook.hpp>
#endif

auto second_translation_unit() -> bool
{
    const auto java_null = std::make_unique<vmhook::any_object>(nullptr);
    return java_null && java_null->get_instance() == nullptr;
}
