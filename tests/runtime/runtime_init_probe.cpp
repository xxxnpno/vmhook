#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
import vmhook.windows;
#else
#include <vmhook/vmhook.hpp>
#include <format>
#include <fstream>
#endif

namespace allocation_fault
{
    class injected_failure final : public std::bad_alloc
    {
    public:
        auto what() const noexcept -> const char* override
        { return "injected runtime initialization allocation failure"; }
    };
    inline thread_local bool reject_all{};
    inline thread_local bool reject_next{};
    inline thread_local std::size_t rejected_size{};
    inline thread_local std::size_t rejected_allocation{};
}

[[gnu::noinline]] auto operator new(std::size_t size) -> void*
{
    if (allocation_fault::reject_all || allocation_fault::reject_next
        || (size && size == allocation_fault::rejected_size))
    {
        allocation_fault::reject_next = false;
        allocation_fault::rejected_size = 0;
        allocation_fault::rejected_allocation = size;
        throw allocation_fault::injected_failure{};
    }
    if (void* value = std::malloc(size ? size : 1)) { return value; }
    throw std::bad_alloc{};
}
[[gnu::noinline]] auto operator delete(void* value) noexcept -> void { std::free(value); }
[[gnu::noinline]] auto operator delete(void* value, std::size_t) noexcept -> void { std::free(value); }

namespace
{
    class fixture final : public vmhook::object<fixture>
    {
    public:
        using vmhook::object<fixture>::object;
    };

    template<typename function_type>
    auto allocation_failed(function_type&& function) -> bool
    {
        try { function(); }
        catch (const allocation_fault::injected_failure&) { return true; }
        return false;
    }

    auto worker(void*) -> DWORD
    {
        const char* result_path{std::getenv("VMHOOK_TEST_RESULT")};
        std::ofstream output{result_path ? result_path : "runtime-init-result.txt"};
        output.setf(std::ios::unitbuf);
        bool passed{true};
        const auto check = [&](std::string_view name, bool correct)
        { output << std::format("{}={}\n", name, (correct ? "pass" : "fail")); passed = passed && correct; };
        try
        {
            const char* selected{std::getenv("VMHOOK_RUNTIME_INIT_FAULT")};
            const std::string_view fault{selected ? selected : "path"};
            const bool builtin{fault == "builtin"};
            const bool allocation_error{fault == "path" || fault == "persistent_path"
                || fault == "profile" || fault == "diagnostic"};
            if (!builtin)
            {
                const std::filesystem::path invalid_pdb{std::format("{}.invalid.pdb", result_path ? result_path : "runtime-init-result.txt")};
                std::ofstream invalid{invalid_pdb, std::ios::binary};
                const std::array<char, 64> bytes{};
                invalid.write(bytes.data(), bytes.size());
                invalid.close();
                if (!invalid) { throw std::runtime_error{"Cannot create the malformed PDB control"}; }
                vmhook::detail::private_native::native_symbols_path = invalid_pdb;
            }
            // Warm this DLL's thread-local facade storage without initializing
            // the private runtime, attaching a JavaThread, or starting a keeper.
            (void)vmhook::detail::native_backend::current_context_lifetime();
            (void)vmhook::detail::private_native::root_reuse_callback();
            std::string diagnostic_copy;
            diagnostic_copy = "HotSpot PDB is not MSF7";
            check("fresh_runtime", !vmhook::detail::native_backend::current_context_lifetime()
                && !vmhook::detail::persistent_hook_roots::prepared_keeper.load()
                && vmhook::detail::private_native::active_session_count == 0);
            if (fault == "path") { allocation_fault::reject_next = true; }
            if (fault == "persistent_path") { allocation_fault::reject_all = true; }
            if (fault == "profile")
            { allocation_fault::rejected_size = sizeof(vmhook::private_metadata::private_profile); }
            // Target the actual standard-library allocation for copying the
            // malformed-PDB diagnostic, rather than exception construction.
            if (fault == "diagnostic")
            { allocation_fault::rejected_size = diagnostic_copy.capacity() + 1; }
            const auto passive_initialize = []() noexcept -> bool
            { return vmhook::detail::private_native::rt().ready; };
            const bool ready{passive_initialize()};
            allocation_fault::reject_all = false;
            allocation_fault::reject_next = false;
            allocation_fault::rejected_size = 0;
            output << std::format("rejected_allocation={}\n", allocation_fault::rejected_allocation);
            check("fault_consumed", allocation_error ? allocation_fault::rejected_allocation != 0
                                                     : allocation_fault::rejected_allocation == 0);
            const auto report{vmhook::runtime_capabilities()};
            (void)vmhook::vm_capabilities();
            check("passive_preflight", report.jvm_found && ready == builtin
                && report.private_abi_verified == builtin
                && !vmhook::detail::native_backend::current_context_lifetime()
                && !vmhook::detail::persistent_hook_roots::prepared_keeper.load()
                && vmhook::detail::private_native::active_session_count == 0);
            if (allocation_error)
            {
                check("native_context_result", allocation_failed([] {
                    vmhook::native_thread_context context{"runtime-init-failure"};
                }));
                check("registration_result", allocation_failed([] {
                    (void)vmhook::register_class<fixture>("ExecutorFixture");
                }));
                check("observer_result", allocation_failed([] {
                    (void)vmhook::on_class_loaded([](const vmhook::class_load_event&) {});
                }));
                check("failure_cleanup", !vmhook::detail::native_backend::current_context_lifetime()
                    && !vmhook::detail::persistent_hook_roots::prepared_keeper.load()
                    && vmhook::detail::private_native::active_session_count == 0);
            }
            else if (builtin)
            {
                vmhook::native_thread_context context{"runtime-init-control"};
                check("native_context_result", context.valid());
                check("registration_result", vmhook::register_class<fixture>("ExecutorFixture"));
                const auto observer{vmhook::on_class_loaded([](const vmhook::class_load_event&) {})};
                check("observer_result", observer != 0 && vmhook::remove_class_observer(observer));
                check("failure_cleanup", context.close()
                    && !vmhook::detail::native_backend::current_context_lifetime());
            }
            else
            {
                vmhook::native_thread_context context{"runtime-init-unsupported"};
                check("native_context_result", !context.valid());
                check("registration_result", report.reason == "HotSpot PDB is not MSF7");
                check("observer_result", vmhook::on_class_loaded([](const vmhook::class_load_event&) {}) == 0);
                check("failure_cleanup", !vmhook::detail::native_backend::current_context_lifetime()
                    && !vmhook::detail::persistent_hook_roots::prepared_keeper.load()
                    && vmhook::detail::private_native::active_session_count == 0);
            }
            check("runtime_init_complete", passed);
            return passed ? 0 : 2;
        }
        catch (const std::exception& error)
        { output << std::format("exception={}\n", error.what()); return 3; }
    }
}

extern "C" BOOL __attribute__((ms_abi)) DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == 1)
    {
        ::DisableThreadLibraryCalls(instance);
        HANDLE thread{::CreateThread(nullptr, 0, worker, nullptr, 0, nullptr)};
        if (thread) { ::CloseHandle(thread); }
    }
    return true;
}
