#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
import vmhook.windows;
import vmhook.generated_fixture;
#else
#include "generated_wrappers.hpp"
#include <fstream>
#endif

namespace
{
    using fixture = generator_fixtures::generated_wrapper_fixture;
    using unicode_type = generator_fixtures::generated_wrapper_fixture_type;
    using unicode_child = generator_fixtures::generated_wrapper_fixture_type_child;

    auto check(std::ostream& output, const std::string_view label, const bool condition) -> void
    {
        if (!condition) { throw std::runtime_error{std::string{label}}; }
        output << std::format("{}=pass\n", label);
        output.flush();
    }

    auto worker(void*) -> DWORD
    {
        char destination[4096]{};
        if (!::GetEnvironmentVariableA("VMHOOK_TEST_RESULT", destination, sizeof destination)) { return 1; }
        std::ofstream output{destination};
        try
        {
            vmhook::native_thread_context context{"VMHook-generated-wrapper"};
            check(output, "generated_context", context.valid());
            check(output, "generated_registration", generator_fixtures::register_java_classes());
            const auto value = fixture::create();
            check(output, "generated_reserved_tokens", value->java_and_eq() == 101 && value->java_or_eq() == 102
                && value->java_not_eq() == 103 && value->java_xor_eq() == 104);
            check(output, "generated_class_name", value->java_generated_wrapper_fixture() == 105
                && generator_fixtures::arg_0::create(1)->get_instance());
            check(output, "generated_facade_names", value->java_get_field() == 11 && value->get_java_instance() == 12
                && value->get_java_native_root() == 13 && value->java_get_instance(1) == 107
                && value->java_get_native_root(1) == 108 && value->java_set_native_root(1) == 109
                && value->java_get_field(1) == 110 && value->java_get_method(1) == 111
                && value->java_static_members() == 111 && value->java_self() == 112
                && value->java_is() == 113 && value->java_java_contract() == 114 && value->java_descriptor() == 115
                && value->get_instance() && value->get_native_root()
                && fixture::worker_thread_name()->to_string() == "VMHook-generated-wrapper");
            check(output, "generated_constructor_customization", fixture::get_construct_calls() == 0
                && value->java_construct() == 116 && fixture::get_construct_calls() == 1);
            const auto unicode = unicode_type::create();
            check(output, "generated_unicode_names", unicode->java_get_field() == 211
                && value->generated_wrapper_fixture_type()->java_get_field() == 211);
            std::unique_ptr<unicode_type> returned;
            std::int32_t most_specific{};
            std::string_view unicode_stage{"reference_field"};
            try
            {
                unicode->set_reference(unicode);
                const auto field = unicode->get_reference();
                field->set_field(212);
                unicode_stage = "exact_method";
                returned = unicode->method(unicode);
                returned->set_field(212);
                unicode_stage = "child_constructor";
                const auto child = unicode_child::create();
                unicode_stage = "inferred_overload";
                most_specific = fixture::static_members::get_method("unicodeChoice")->call(child);
            }
            catch (const vmhook::exception& failure)
            {
                output << std::format("generated_unicode_stage={}\n", unicode_stage);
                try
                {
                    const auto java_failure = failure.get_throwable();
                    if (java_failure && java_failure->get_instance())
                    {
                        const std::string message = java_failure->get_method("toString", "()Ljava/lang/String;")->call();
                        output << std::format("generated_java_exception={}\n", message);
                    }
                }
                catch (...) {}
                output.flush();
                throw;
            }
            check(output, "generated_unicode_descriptor", returned && returned->get_instance()
                && unicode->java_get_field() == 212 && most_specific == 2);
            for (std::int32_t index{}; index < 16; ++index)
            {
                const auto next = unicode->method(unicode);
                if (next->java_get_field() != 212) { throw std::runtime_error{"Generated Unicode binding changed across GC"}; }
            }
            check(output, "generated_gc", unicode->java_get_field() == 212 && value->java_and_eq() == 101);
            check(output, "generated_complete", vmhook::verify_hooks() == 0);
            return 0;
        }
        catch (const std::exception& failure)
        {
            output << std::format("exception={}\n", failure.what());
            output.flush();
            return 2;
        }
    }
}

extern "C" BOOL __attribute__((ms_abi)) DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == 1)
    {
        ::DisableThreadLibraryCalls(instance);
        if (const HANDLE thread = ::CreateThread(nullptr, 0, worker, nullptr, 0, nullptr)) { ::CloseHandle(thread); }
    }
    return true;
}
