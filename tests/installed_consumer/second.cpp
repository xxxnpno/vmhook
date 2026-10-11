#if defined(VMHOOK_CONSUMER_MODULE)
import std;
import vmhook;
#else
#include <vmhook/vmhook.hpp>
#endif

auto second_translation_unit() -> bool
{
    try { (void)vmhook::get_instances<vmhook::any_object>(1); return false; }
    catch (const vmhook::exception&) {}
    const std::variant<int, std::string> variant{std::string{"second"}};
    if (std::get<std::string>(variant) != "second") { return false; }
    const auto java_null = std::make_unique<vmhook::any_object>(nullptr);
    return java_null && java_null->get_instance() == nullptr;
}
