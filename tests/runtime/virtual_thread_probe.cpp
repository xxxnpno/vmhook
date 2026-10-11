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
        static auto run_virtual() -> std::int32_t { return static_members::get_method("run_virtual")->call(); }
        static auto run_platform() -> std::int32_t { return static_members::get_method("run_platform")->call(); }
        static auto originals() -> std::int32_t { return static_members::get_method("originals")->call(); }
    };
    auto worker(void*) -> DWORD
    {
        const auto* path = std::getenv("VMHOOK_VIRTUAL_THREAD_REPORT");
        std::ofstream output{path ? path : "vmhook-virtual-thread-report.txt"}; output.setf(std::ios::unitbuf);
        bool valid = true;
        const auto check = [&](std::string_view name, bool value)
        { output << name << '=' << (value ? "pass" : "fail") << '\n'; valid = valid && value; };
        try
        {
            vmhook::native_thread_context context{"VMHook-virtual-thread"};
            check("context", context.valid());
            if (!context.valid() || !vmhook::register_class<fixture>("VirtualThreadFixture"))
            { check("registration", false); return 2; }
            check("registration", true);
            unsigned methods{}, fields{};
            const auto method = vmhook::add_hook<fixture>("target", "(I)I",
                [&](vmhook::return_value& result, std::int32_t) { ++methods; result.set(std::int32_t{99}); });
            const auto field = vmhook::hook_field<fixture, std::int32_t>("field_value",
                [&](vmhook::field_access<std::int32_t>& event)
                { ++fields; event.set(event.is_write() ? std::int32_t{22} : std::int32_t{77}); });
            check("hooks", method != 0 && field);
            const auto before = vmhook::rejected_hook_callback_frames();
            const auto virtual_checks = fixture::run_virtual();
            check("real_virtual_thread", (virtual_checks & 1) != 0);
            check("virtual_original_method", (virtual_checks & 2) != 0 && methods == 0);
            check("virtual_original_fields", (virtual_checks & 4) != 0 && fields == 0);
            check("refusal_count", vmhook::rejected_hook_callback_frames() >= before + 3);
            check("platform_callbacks", fixture::run_platform() == 3 && methods == 1 && fields == 2);
            check("removal", vmhook::remove_hook(method) && vmhook::unhook_field<fixture>("field_value"));
            check("originals_after_remove", fixture::originals() == 1);
            vmhook::shutdown_hooks();
            output << "virtual_thread_proof=" << (valid ? "pass" : "fail") << '\n';
            return valid ? 0 : 2;
        }
        catch (const std::exception& failure)
        { output << "exception=" << failure.what() << "\nvirtual_thread_proof=fail\n"; return 3; }
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
