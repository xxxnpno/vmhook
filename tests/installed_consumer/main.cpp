#if defined(VMHOOK_CONSUMER_MODULE)
import std;
import vmhook;
#else
#include <vmhook/vmhook.hpp>
#endif

auto second_translation_unit() -> bool;

class counter : public vmhook::object<counter>
{
public:
    using vmhook::object<counter>::object;

    auto value() const -> std::int32_t { return get_field("value")->get(); }
    auto add(std::int32_t amount) const -> std::int32_t
    { return get_method("add", "(I)I")->call(amount); }
    static auto total() -> std::int32_t
    { return static_members::get_field("total")->get(); }
};

auto main() -> int
{
    // This process has no JVM. Exercise ownership and no-JVM refusal without
    // relying on a checkout, a Java installation or a compiler cache in it.
    const auto java_null = std::make_unique<counter>(nullptr);
    if (!java_null || java_null->get_instance() != nullptr) { return 1; }
    if (vmhook::version_string.empty() || !second_translation_unit()) { return 2; }
    if (vmhook::register_class<counter>("example/Counter")) { return 3; }
    vmhook::native_thread_context context{"VMHook-installed-consumer"};
    if (context.valid()) { return 4; }
    // GNU C++26 constexpr exception constructors must retain the runtime's
    // copy/move ABI even in an unoptimized importer linked statically.
    const std::logic_error original_error{"consumer"};
    const std::logic_error copied_error{original_error};
    auto movable_error = original_error;
    const std::logic_error moved_error{std::move(movable_error)};
    const std::runtime_error original_runtime{"runtime"};
    const std::runtime_error copied_runtime{original_runtime};
    auto movable_runtime = original_runtime;
    const std::runtime_error moved_runtime{std::move(movable_runtime)};
    if (std::string{copied_error.what()} != "consumer" || std::string{moved_error.what()} != "consumer"
        || std::string{copied_runtime.what()} != "runtime" || std::string{moved_runtime.what()} != "runtime")
    { return 6; }
    std::filesystem::path path{"installed-consumer"};
    auto future = std::async(std::launch::deferred, [] { return 42; });
    return path.filename() == "installed-consumer" && future.get() == 42 ? 0 : 5;
}
