#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
import vmhook.windows;
#else
#include <vmhook/vmhook.hpp>
#include <format>
#endif

namespace
{
    class fixture : public vmhook::object<fixture>
    { public: using vmhook::object<fixture>::object; };
    class api : public vmhook::object<api>
    { public: using vmhook::object<api>::object; };
    std::atomic<unsigned> calls{}, other_calls{}, root_checks{};
    std::atomic<bool> correct{true};
    std::optional<vmhook::interface_proxy<api>> current, other;
    std::weak_ptr<int> callback_lifetime;

    auto check(bool condition) -> void
    { if (!condition) { correct.store(false, std::memory_order_release); } }

    auto callback(const std::unique_ptr<vmhook::reflection_method>& method,
        const std::unique_ptr<vmhook::array<vmhook::any_object>>& arguments)
        -> std::unique_ptr<vmhook::any_object>
    {
        const auto count{calls.fetch_add(1, std::memory_order_acq_rel)};
        check(method && method->get_native_root() && arguments);
        const auto name{method->name()};
        if (name == "fail")
        {
            check(arguments->get_instance() == nullptr);
            (void)fixture::static_members::get_method("raise", "()Ljava/lang/Object;")->call();
            throw std::runtime_error{"Java throw was hidden"};
        }
        if (name == "native_fail") { throw std::runtime_error{"native callback failure"}; }
        if (name == "allocation_fail") { throw std::bad_alloc{}; }
        if (name == "echo" || name == "slow")
        {
            if (name == "slow")
            {
                fixture::static_members::get_field("close_now")->set(true);
                vmhook::windows::Sleep(120);
            }
            if (count % 7 == 0 || name == "slow")
            {
                fixture::static_members::get_method("collect", "()V")->call();
                check(method->name() == name && arguments->size() == 1);
                const auto parameters{method->parameter_types()};
                const auto parameter{parameters->get(0)}, returned{method->return_type()};
                check(parameters->size() == 1
                    && parameter->get_method("getName", "()Ljava/lang/String;")->call().as_string() == "java.lang.String"
                    && returned->get_method("getName", "()Ljava/lang/String;")->call().as_string() == "java.lang.String");
                root_checks.fetch_add(1, std::memory_order_relaxed);
            }
            const auto argument{arguments->get(0)};
            const auto text{argument->get_method("toString", "()Ljava/lang/String;")->call().as_string()};
            return vmhook::new_string(std::format("native:{}", text));
        }
        if (name == "add")
        {
            const auto argument{arguments->get(0)};
            const std::int32_t amount = argument->get_method("intValue", "()I")->call();
            return fixture::static_members::get_method("box_int", "(I)Ljava/lang/Integer;")->call(amount + 5);
        }
        if (name == "nullable") { return arguments->get(0); }
        if (name == "notify_value")
        {
            check(arguments->size() == 1 && arguments->get(0)->get_instance());
            return std::make_unique<vmhook::any_object>(nullptr);
        }
        throw std::runtime_error{"Unexpected interface method"};
    }

    auto worker(void*) -> vmhook::windows::DWORD
    {
        char destination[4096]{};
        if (!vmhook::windows::GetEnvironmentVariableA("VMHOOK_INTERFACE_RESULT", destination, sizeof(destination)))
        { return 1; }
        std::ofstream report{destination};
        report.setf(std::ios::unitbuf);
        const auto passed = [&](const char* label, bool condition)
        { report << std::format("{}={}\n", label, (condition ? "pass" : "fail")); check(condition); return condition; };
        try
        {
            {
                vmhook::native_thread_context creator{"VMHook-interface-creator"};
                if (!creator || !vmhook::register_class<fixture>("interface_fixture")
                    || !vmhook::register_class<api>("interface_fixture$api"))
                { throw vmhook::exception{"Interface fixture setup failed"}; }
                const std::unique_ptr<vmhook::any_object> handler = fixture::static_members::get_method(
                    "handler", "()Lvmhook/callback_handler;")->call();
                const auto capture{std::make_shared<int>(7)};
                callback_lifetime = capture;
                current.emplace(vmhook::make_interface_proxy<api>(handler,
                    [capture](const std::unique_ptr<vmhook::reflection_method>& method,
                        const std::unique_ptr<vmhook::array<vmhook::any_object>>& arguments)
                        -> std::unique_ptr<vmhook::any_object>
                    { check(*capture == 7); return callback(method, arguments); }));
                const std::unique_ptr<vmhook::any_object> second_handler = fixture::static_members::get_method(
                    "handler", "()Lvmhook/callback_handler;")->call();
                other.emplace(vmhook::make_interface_proxy<api>(second_handler,
                    [](const std::unique_ptr<vmhook::reflection_method>& method,
                        const std::unique_ptr<vmhook::array<vmhook::any_object>>& arguments)
                        -> std::unique_ptr<vmhook::any_object>
                    {
                        other_calls.fetch_add(1, std::memory_order_relaxed);
                        if (method->name() != "echo") { throw std::runtime_error{"Unexpected second interface method"}; }
                        const auto argument{arguments->get(0)};
                        return vmhook::new_string(std::format("other:{}", argument->get_method("toString",
                            "()Ljava/lang/String;")->call().as_string()));
                    }));
                const auto first{current->acquire()}, second{other->acquire()};
                fixture::static_members::get_method("set_current", "(Linterface_fixture$api;)V")->call(first);
                fixture::static_members::get_method("set_other", "(Linterface_fixture$api;)V")->call(second);
                bool duplicate_refused{};
                try { auto duplicate = vmhook::make_interface_proxy<api>(handler, callback); }
                catch (const vmhook::exception& error)
                { duplicate_refused = error.code() == vmhook::error_code::java_exception && error.has_throwable(); }
                if (!passed("interface_setup", current->valid() && other->valid() && duplicate_refused)) { return 2; }
            }
            // Source context is gone. Persistent roots are reacquired by the
            // independent administrator and by each Java caller's callback.
            vmhook::native_thread_context administrator{"VMHook-interface-admin"};
            if (!administrator) { throw vmhook::exception{administrator.error()}; }
            const auto first{current->acquire()}, second{other->acquire()};
            if (!passed("interface_lifetime", first->get_instance() && second->get_instance())) { return 2; }
            fixture::static_members::get_field("start")->set(true);
            const auto deadline{std::chrono::steady_clock::now() + std::chrono::seconds{120}};
            bool closed{};
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (!closed && static_cast<bool>(fixture::static_members::get_field("close_now")->get()))
                {
                    closed = current->close();
                    fixture::static_members::get_field("closed")->set(closed);
                }
                if (static_cast<bool>(fixture::static_members::get_field("finished")->get())) { break; }
                vmhook::windows::Sleep(2);
            }
            const bool finished = fixture::static_members::get_field("finished")->get();
            const std::int32_t valid = fixture::static_members::get_field("valid")->get();
            const std::int32_t errors = fixture::static_members::get_field("error_count")->get();
            const std::unique_ptr<vmhook::any_object> failure = fixture::static_members::get_field("failure")->get();
            if (failure->get_instance())
            { report << std::format("java_failure={}\n", failure->get_method("toString", "()Ljava/lang/String;")->call().as_string()); }
            report << std::format(
                "callbacks={}\nother_callbacks={}\nroot_checks={}\njava_checks={:d}\njava_errors={}\n",
                calls.load(), other_calls.load(), root_checks.load(), valid, errors);
            passed("interface_roundtrip", finished && valid == 18 && !failure->get_instance());
            passed("interface_errors", errors == 3);
            passed("interface_gc", root_checks.load() > 0);
            bool reacquire_refused{};
            try { (void)current->acquire(); }
            catch (const vmhook::exception&) { reacquire_refused = true; }
            passed("interface_close", closed && !current->valid() && !current->close()
                && reacquire_refused && calls.load() == 73 && other_calls.load() == 2 && other->close());
            passed("interface_release", callback_lifetime.expired());
            (void)vmhook::reclaim_hook_callbacks();
            passed("interface_proof", correct.load());
            return correct.load() ? 0 : 2;
        }
        catch (const std::exception& error)
        { report << std::format("exception={}\ninterface_proof=fail\n", error.what()); return 3; }
    }
}

extern "C" BOOL __attribute__((ms_abi)) DllMain(HINSTANCE self, DWORD reason, LPVOID)
{
    if (reason == 1)
    {
        ::DisableThreadLibraryCalls(self);
        if (auto thread{::CreateThread(nullptr, 0, &worker, nullptr, 0, nullptr)})
        { vmhook::windows::CloseHandle(thread); }
    }
    return true;
}
