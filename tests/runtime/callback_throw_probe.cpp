#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
import vmhook.windows;
#else
#include <vmhook/vmhook.hpp>
#endif
namespace
{
    class fixture final : public vmhook::object<fixture>
    {
    public:
        using vmhook::object<fixture>::object;
        static auto collect() -> void { static_members::get_method("collect")->call(); }
        static auto run_checks() -> std::int32_t { return static_members::get_method("run_checks")->call(); }
        static auto original_checks() -> std::int32_t { return static_members::get_method("original_checks")->call(); }
    };
    struct allocation_failure final
    {
        allocation_failure() = default;
        allocation_failure(const allocation_failure&) { throw std::bad_alloc{}; }
        auto operator()(vmhook::return_value&, const std::unique_ptr<vmhook::any_object>&) const -> void {}
    };
    auto worker(void*) -> DWORD
    {
        const auto* path = std::getenv("VMHOOK_CALLBACK_THROW_REPORT");
        std::ofstream output{path ? path : "vmhook-callback-throw-report.txt"}; output.setf(std::ios::unitbuf);
        bool valid = true;
        const auto check = [&](std::string_view name, bool value)
        { output << name << '=' << (value ? "pass" : "fail") << '\n'; valid = valid && value; };
        try
        {
            vmhook::native_thread_context context{"VMHook-callback-throw"};
            check("context", context.valid());
            if (!context.valid() || !vmhook::register_class<fixture>("CallbackThrowFixture"))
            { check("registration", false); return 2; }
            check("registration", true);
            allocation_failure exhausted{}; bool propagated{};
            try { (void)vmhook::add_hook<fixture>("target_int", "(Ljava/lang/Throwable;)I", exhausted); }
            catch (const std::bad_alloc&) { propagated = true; }
            check("allocation_failure_propagated", propagated);
            unsigned forwarded{}, invalid{}, null_ref{}, later{};
            const auto integer = vmhook::add_hook<fixture>("target_int", "(Ljava/lang/Throwable;)I",
                [&](vmhook::return_value& result, const std::unique_ptr<vmhook::any_object>& failure)
                { fixture::collect(); result.set(std::int32_t{3}); try { result.throw_exception(failure); ++forwarded; } catch (const std::exception& error) { output << "throw_error=" << error.what() << '\n'; throw; } });
            const auto ignored = vmhook::add_hook<fixture>("target_int", "(Ljava/lang/Throwable;)I",
                [&](vmhook::return_value&, const std::unique_ptr<vmhook::any_object>&) { ++later; }, {.priority = 1});
            const auto nothing = vmhook::add_hook<fixture>("target_void", "(Ljava/lang/Throwable;)V",
                [&](vmhook::return_value& result, const std::unique_ptr<vmhook::any_object>& failure)
                { try { result.throw_exception(failure); ++forwarded; } catch (const std::exception& error) { output << "throw_error=" << error.what() << '\n'; throw; } });
            const auto object = vmhook::add_hook<fixture>("target_object", "(Ljava/lang/Throwable;)Ljava/lang/Object;",
                [&](vmhook::return_value& result, const std::unique_ptr<vmhook::any_object>& failure)
                { try { result.throw_exception(failure); ++forwarded; } catch (const std::exception& error) { output << "throw_error=" << error.what() << '\n'; throw; } });
            const auto wrong = vmhook::add_hook<fixture>("invalid_object", "(Ljava/lang/Object;)I",
                [&](vmhook::return_value& result, const std::unique_ptr<vmhook::any_object>& value)
                { try { result.throw_exception(value); } catch (const vmhook::exception&) { ++invalid; } });
            const auto empty = vmhook::add_hook<fixture>("null_throw", "(Ljava/lang/Throwable;)I",
                [&](vmhook::return_value& result, const std::unique_ptr<vmhook::any_object>& value)
                { if (value && !value->get_instance()) { try { result.throw_exception(value); } catch (const vmhook::exception&) { ++null_ref; } } });
            const auto resume = vmhook::add_hook<fixture>("sync_resume", "(Ljava/lang/Throwable;)I",
                [](vmhook::return_value&, const std::unique_ptr<vmhook::any_object>&) { fixture::collect(); });
            const auto early = vmhook::add_hook<fixture>("sync_return", "(Ljava/lang/Throwable;)I",
                [](vmhook::return_value& result, const std::unique_ptr<vmhook::any_object>&)
                { fixture::collect(); result.set(std::int32_t{11}); });
            const auto instance = vmhook::add_hook<fixture>("instance_throw", "(Ljava/lang/Throwable;)I",
                [&](vmhook::return_value& result, const std::unique_ptr<vmhook::any_object>& self,
                    const std::unique_ptr<vmhook::any_object>& failure)
                { if (!self || !self->get_instance()) { throw vmhook::exception{"Missing instance receiver"}; }
                  fixture::collect(); result.throw_exception(failure); ++forwarded; });
            check("hooks", integer && ignored && nothing && object && wrong && empty && resume && early && instance);
            const auto checks = fixture::run_checks();
            check("same_throwable_primitive", (checks & 1) != 0);
            check("same_throwable_void", (checks & 2) != 0);
            check("same_throwable_reference", (checks & 4) != 0);
            check("body_skip_and_finally", (checks & 8) != 0);
            check("unrelated_object_refused", (checks & 16) != 0 && invalid == 1);
            check("java_null_refused", (checks & 32) != 0 && null_ref == 1);
            check("exception_wins_return", forwarded == 4 && later == 0);
            check("synchronized_resume_after_gc", (checks & 64) != 0);
            check("synchronized_early_return_after_gc", (checks & 128) != 0);
            check("synchronized_instance_throw_after_gc", (checks & 256) != 0);
            check("removal", vmhook::remove_hook(integer) && vmhook::remove_hook(ignored)
                && vmhook::remove_hook(nothing) && vmhook::remove_hook(object)
                && vmhook::remove_hook(wrong) && vmhook::remove_hook(empty)
                && vmhook::remove_hook(resume) && vmhook::remove_hook(early) && vmhook::remove_hook(instance));
            check("synchronized_original_after_remove", fixture::original_checks() == 1);
            vmhook::shutdown_hooks();
            output << "callback_throw_proof=" << (valid ? "pass" : "fail") << '\n';
            return valid ? 0 : 2;
        }
        catch (const std::exception& failure)
        { output << "exception=" << failure.what() << "\ncallback_throw_proof=fail\n"; return 3; }
    }
}
extern "C" BOOL __attribute__((ms_abi)) DllMain(HINSTANCE self, DWORD reason, LPVOID)
{
    if (reason == 1)
    {
        ::DisableThreadLibraryCalls(self);
        if (auto thread = ::CreateThread(nullptr, 0, &worker, nullptr, 0, nullptr)) { vmhook::windows::CloseHandle(thread); }
    }
    return true;
}
