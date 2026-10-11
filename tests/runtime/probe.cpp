#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
import vmhook.windows;
#else
#include <vmhook/vmhook.hpp>
#include <format>
#include <fstream>
#endif

namespace
{
    class fixture : public vmhook::object<fixture>
    {
    public:
        using vmhook::object<fixture>::object;
        static auto total() -> std::int32_t { return static_members::get_field("total")->get(); }
        static auto arguments_mutated() -> bool { return static_members::get_field("argumentMutation")->get(); }
        static auto request_cancellation() -> void { static_members::get_field("cancelRequested")->set(true); }
        static auto cancellation_body() -> std::int32_t { return static_members::get_field("cancelBody")->get(); }
        static auto count_result() -> vmhook::method_proxy::value_t
        {
            return static_members::get_method("count", "()I")->call();
        }
        auto add(std::int32_t amount) -> std::int32_t
        {
            return get_method("add", "(I)I")->call(amount);
        }
        auto value() const -> std::int32_t { return get_field("value")->get(); }
        static auto touch(const std::unique_ptr<fixture>& other) -> std::int32_t
        {
            return static_members::get_method("touch", "(LVMHookFixture;)I")->call(other);
        }
        static auto touch_twice(const std::unique_ptr<fixture>& other) -> std::int32_t
        {
            return static_members::get_method("touchTwice", "(LVMHookFixture;LVMHookFixture;)I")->call(other, other);
        }
        static auto text_result() -> vmhook::method_proxy::value_t
        {
            return static_members::get_method("text", "()Ljava/lang/String;")->call();
        }
        static auto latin1_result() -> vmhook::method_proxy::value_t
        {
            return static_members::get_method("latin1", "()Ljava/lang/String;")->call();
        }
        auto text_value() const -> std::string { return get_field("textValue")->get(); }
        static auto throw_result() -> vmhook::method_proxy::value_t
        {
            return static_members::get_method("fail", "()V")->try_call();
        }
    };

    struct call_results
    {
        bool callback_thread{};
        bool invoked{};
        bool count_correct{};
        bool constructed{};
        bool construction_correct{};
        bool exception_correct{};
        bool string_correct{};
    };

    auto verify_string_returns() -> bool
    {
        const auto text = fixture::text_result();
        const auto latin1 = fixture::latin1_result();
        return text.was_invoked() && !text.threw()
            && static_cast<std::string>(text) == std::string{"A\0\xf0\x9f\x98\x80\xc3\xa9", 8}
            && latin1.was_invoked() && !latin1.threw()
            && static_cast<std::string>(latin1) == "caf\xc3\xa9";
    }

    std::atomic<unsigned> cancelled_calls{};

    auto verify_argument_mutation() -> bool
    {
        vmhook::native_thread_context context{"VMHook-argument-fixture"};
        if (!context.valid()) { return false; }
        struct check_state
        {
            std::atomic<unsigned> checked{};
            std::atomic<unsigned> observed{};
            std::atomic<bool> valid{true};
        };
        const auto state = std::make_shared<check_state>();
        const auto installed = vmhook::add_hook<fixture>("mixed", "(IJIDI[Ljava/lang/Object;)I",
            [state](vmhook::return_value& result, const std::unique_ptr<fixture>& self,
                              std::int32_t first, std::int64_t wide, std::int32_t following,
                              double floating, std::int32_t last,
                              const std::unique_ptr<vmhook::any_object>& array)
        {
            bool correct = self && self->get_instance() && self->value() == 15
                && first == 3 && wide == 7 && following == 5 && floating == 6.5
                && last == 9 && array && array->get_instance() != nullptr;
            const auto java_null = std::make_unique<vmhook::any_object>(nullptr);
            correct = correct && !result.set_arg(-1, std::int32_t{1})
                && !result.set_arg(6, std::int32_t{1}) && !result.set_arg(0, std::int64_t{1})
                && !result.set_arg(1, double{1}) && !result.set_arg(5, std::int32_t{1})
                && result.set_arg(0, std::int32_t{21})
                && result.set_arg(1, std::int64_t{0x1122334455667788LL})
                && result.set_arg(2, std::int32_t{-3}) && result.set_arg(3, double{42.25})
                && result.set_arg(4, std::int32_t{-7}) && result.set_arg(5, array)
                && result.set_arg(5, java_null);
            const auto arguments = result.frame()->get_arguments<vmhook::oop_t, std::int32_t,
                std::int64_t, std::int32_t, double, std::int32_t, vmhook::oop_t>();
            correct = correct && std::get<0>(arguments) == self->get_instance()
                && std::get<1>(arguments) == 21 && std::get<2>(arguments) == 0x1122334455667788LL
                && std::get<3>(arguments) == -3 && std::get<4>(arguments) == 42.25
                && std::get<5>(arguments) == -7 && std::get<6>(arguments) == nullptr;
            if (!correct) { state->valid.store(false, std::memory_order_relaxed); }
            state->checked.fetch_add(1, std::memory_order_release);
        }, {.priority = 0});
        const auto observer = vmhook::add_hook<fixture>(
            "mixed", "(IJIDI[Ljava/lang/Object;)I", [state](vmhook::return_value& result)
        {
            const auto arguments = result.frame()->get_arguments<vmhook::oop_t, std::int32_t,
                std::int64_t, std::int32_t, double, std::int32_t, vmhook::oop_t>();
            const bool correct = std::get<1>(arguments) == 21
                && std::get<2>(arguments) == 0x1122334455667788LL
                && std::get<3>(arguments) == -3 && std::get<4>(arguments) == 42.25
                && std::get<5>(arguments) == -7 && std::get<6>(arguments) == nullptr;
            if (!correct) { state->valid.store(false, std::memory_order_relaxed); }
            state->observed.fetch_add(1, std::memory_order_release);
        }, {.priority = 1});
        for (int attempt = 0; installed && attempt < 500; ++attempt)
        {
            if (state->checked.load(std::memory_order_acquire) > 0
                && state->observed.load(std::memory_order_acquire) > 0
                && fixture::arguments_mutated()) { break; }
            ::Sleep(10);
        }
        const bool correct = installed && observer
            && state->checked.load(std::memory_order_acquire) > 0
            && state->observed.load(std::memory_order_acquire) > 0
            && state->valid.load(std::memory_order_relaxed) && fixture::arguments_mutated();
        vmhook::shutdown_hooks();
        return correct;
    }

    auto install_dispatch_hook() -> bool
    {
        vmhook::native_thread_context context{"VMHook-dispatch-admin"};
        return context.valid()
            && vmhook::hook<fixture>("tick", "()V", [](vmhook::return_value&) {});
    }

    auto verify_hook_lifecycle(std::ostream& output) -> void
    {
        // Keep field control and observations safe across the fixture's moving GC.
        vmhook::native_thread_context context{"VMHook-lifecycle-fixture"};
        vmhook::shutdown_hooks();
        cancelled_calls.store(0, std::memory_order_relaxed);
        const auto body_before = context.valid() ? fixture::cancellation_body() : -1;
        // This method is never called before installation. Keep tick's normal
        // pacing so interpreter coverage does not depend on JIT warmup timing.
        const bool installed = context.valid()
            && vmhook::hook<fixture>("cancelProbe", "()V", [](vmhook::return_value& value)
        {
            cancelled_calls.fetch_add(1, std::memory_order_relaxed);
            value.cancel();
        });
        if (installed) { fixture::request_cancellation(); }
        for (int attempt = 0; installed && attempt < 500
            && cancelled_calls.load(std::memory_order_relaxed) < 16; ++attempt)
        {
            ::Sleep(10);
        }
        output << std::format("cancellation={}\n", (installed && cancelled_calls.load(std::memory_order_relaxed) >= 16
                   && body_before == 0 && fixture::cancellation_body() == 0 ? "pass" : "fail"));
        vmhook::shutdown_hooks();
        ::Sleep(100);
        const auto after = cancelled_calls.load(std::memory_order_relaxed);
        const auto body_after = context.valid() ? fixture::cancellation_body() : -1;
        ::Sleep(200);
        output << std::format("shutdown={}\n", (cancelled_calls.load(std::memory_order_relaxed) == after
                   && vmhook::verify_hooks() == 0 && context.valid()
                   && fixture::cancellation_body() > body_after ? "pass" : "fail"));
    }

    auto worker(void*) -> DWORD
    {
        char destination[4096]{};
        if (!::GetEnvironmentVariableA("VMHOOK_TEST_RESULT", destination, sizeof destination))
        {
            return 1;
        }
        std::ofstream output{destination};
        output << std::unitbuf;
        try
        {
            {
                vmhook::native_thread_context context{"VMHook-metadata-fixture"};
                if (!context.valid() || !vmhook::register_class<fixture>("VMHookFixture")
                    || fixture::total() != 41)
                {
                    output << "metadata=fail\n";
                    return 2;
                }
                output << "metadata=pass\n";
            }
            // Checked call() throws outside a Java context; try_call() reports
            // this deliberate refusal without hiding checked-call regressions.
            const auto refused = fixture::static_members::get_method("count", "()I")->try_call();
            output << std::format("native_refusal={}\n", (!vmhook::can_call_java() && !refused.was_invoked() ? "pass" : "fail"));
            char mode[32]{};
            if (::GetEnvironmentVariableA("VMHOOK_TEST_MODE", mode, sizeof mode)
                && std::string_view{mode} == "metadata")
            {
                output << "baseline=pass\n";
                return 0;
            }
            if (!install_dispatch_hook())
            {
                output << "hook=unsupported\n";
                return 3;
            }
            output << "hook=pass\n";
            const bool arguments_correct = verify_argument_mutation();
            output << std::format("argument_mutation={}\n", (arguments_correct ? "pass" : "fail"));
            if (!arguments_correct
                || !install_dispatch_hook()) { return 7; }
            const auto worker_thread = ::GetCurrentThreadId();
            if (std::string_view{mode} == "hooks")
            {
                auto future = vmhook::dispatch([worker_thread]
                {
                    auto nested = vmhook::dispatch([] { return 17; });
                    return ::GetCurrentThreadId() != worker_thread && vmhook::can_call_java()
                        && nested.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready
                        && nested.get() == 17;
                });
                const bool dispatched = future.wait_for(std::chrono::seconds{15}) == std::future_status::ready
                    && future.get();
                output << std::format("dispatch={}\n", (dispatched ? "pass" : "fail"));
                verify_hook_lifecycle(output);
                return dispatched ? 0 : 4;
            }
            auto future = vmhook::dispatch([worker_thread]
            {
                call_results results;
                results.callback_thread = ::GetCurrentThreadId() != worker_thread
                    && vmhook::can_call_java();
                auto count = fixture::count_result();
                results.invoked = count.was_invoked();
                if (results.invoked && !count.threw())
                {
                    results.count_correct = static_cast<std::int32_t>(count) == 41;
                }
                const auto thrown = fixture::throw_result();
                const auto after_exception = fixture::count_result();
                results.exception_correct = thrown.was_invoked() && thrown.threw()
                    && thrown.exception_class == "java/lang/IllegalStateException"
                    && after_exception.was_invoked() && !after_exception.threw()
                    && static_cast<std::int32_t>(after_exception) == 41;
                results.string_correct = verify_string_returns();
                auto created = vmhook::make_unique<fixture>(std::int32_t{10});
                results.constructed = created && created->get_instance();
                if (results.constructed)
                {
                    results.construction_correct = created->add(5) == 15
                        && created->add(7) == 22 && created->value() == 22
                        && fixture::touch(created) == 25 && created->value() == 25
                        && created->add(1) == 26 && fixture::touch_twice(created) == 27
                        && created->value() == 27 && created->add(1) == 28;
                    results.string_correct = results.string_correct
                        && created->text_value() == std::string{"A\0\xf0\x9f\x98\x80\xc3\xa9", 8};
                }
                return results;
            });
            if (future.wait_for(std::chrono::seconds{15}) != std::future_status::ready)
            {
                output << "dispatch=timeout\n";
                return 4;
            }
            const auto results = future.get();
            output << std::format(
                "dispatch={}\ninvocation={}\nconstruction={}\njava_exception={}\nstring_read={}\n",
                results.callback_thread ? "pass" : "fail",
                results.invoked ? (results.count_correct ? "pass" : "fail") : "unsupported",
                results.constructed ? (results.construction_correct ? "pass" : "fail") : "unsupported",
                results.exception_correct ? "pass" : "fail", results.string_correct ? "pass" : "fail");
            if (!results.callback_thread || !results.invoked || !results.count_correct
                || !results.constructed || !results.construction_correct
                || !results.exception_correct || !results.string_correct)
            {
                return 6;
            }
            bool stress_correct{true};
            for (int iteration = 0; iteration < 32 && stress_correct; ++iteration)
            {
                auto stress = vmhook::dispatch([]
                {
                    const auto count = fixture::count_result();
                    const auto thrown = fixture::throw_result();
                    const auto after_exception = fixture::count_result();
                    auto created = vmhook::make_unique<fixture>(std::int32_t{10});
                    return count.was_invoked() && !count.threw()
                        && static_cast<std::int32_t>(count) == 41
                        && thrown.was_invoked() && thrown.threw()
                        && thrown.exception_class == "java/lang/IllegalStateException"
                        && after_exception.was_invoked() && !after_exception.threw()
                        && static_cast<std::int32_t>(after_exception) == 41
                        && created && created->get_instance() && created->add(5) == 15
                        && created->add(7) == 22 && created->value() == 22
                        && fixture::touch(created) == 25 && created->value() == 25
                        && created->add(1) == 26 && fixture::touch_twice(created) == 27
                        && created->value() == 27 && created->add(1) == 28
                        && created->text_value() == std::string{"A\0\xf0\x9f\x98\x80\xc3\xa9", 8}
                        && verify_string_returns();
                });
                stress_correct = stress.wait_for(std::chrono::seconds{5}) == std::future_status::ready
                    && stress.get();
            }
            output << std::format("gc_stress={}\n", (stress_correct ? "pass" : "fail"));
            verify_hook_lifecycle(output);
        }
        catch (const std::exception& error)
        {
            output << std::format("exception={}\n", error.what());
            return 5;
        }
        return 0;
    }
}

extern "C" BOOL __attribute__((ms_abi)) DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == 1)
    {
        ::DisableThreadLibraryCalls(instance);
        // Execute VMHook only after DllMain releases the Windows loader lock.
        HANDLE thread = ::CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
        if (thread) { ::CloseHandle(thread); }
    }
    return true;
}
