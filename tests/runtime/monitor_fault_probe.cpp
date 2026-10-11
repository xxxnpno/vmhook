#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
import vmhook.windows;
#else
#include <vmhook/vmhook.hpp>
#include <fstream>
#include <cstdlib>
#include <new>
#endif
namespace {
thread_local bool fail_allocation{};
}
__attribute__((noinline)) void* operator new(std::size_t size) {
    if (fail_allocation) { fail_allocation=false; throw std::bad_alloc{}; }
    if (void* value=std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc{};
}
__attribute__((noinline)) void operator delete(void* value) noexcept { std::free(value); }
__attribute__((noinline)) void operator delete(void* value, std::size_t) noexcept { std::free(value); }
namespace {
namespace pn=vmhook::detail::private_native;
namespace nb=vmhook::detail::native_backend;
class fixture final : public vmhook::object<fixture> {
public:
using vmhook::object<fixture>::object;
static auto monitor() -> std::unique_ptr<fixture> { return static_members::get_field("monitor_object")->get(); }
auto held() const -> bool { return get_method("monitor_held")->call(); }
};
class throwable final : public vmhook::object<throwable> {
public: using vmhook::object<throwable>::object;
};
thread_local pn::root_slot_provider original_provider{};
thread_local void* injected_throwable{};
thread_local int provider_calls{};
auto fault_provider(void* thread) noexcept -> void** {
    void** slot=original_provider ? original_provider(thread) : nullptr;
    ++provider_calls;
    if (provider_calls==1) {
        // Private fault injection: simulate the pending result of the actual
        // acquisition only after its preflight accepted a clean context.
        pn::store<void*>(thread,pn::rt().off("JavaThread","_pending_exception"),injected_throwable);
    } else if (provider_calls==2) {
        // retain_root_slot's real shared-root allocation is the failing step.
        fail_allocation=true;
    }
    return slot;
}
auto worker(void*) -> DWORD {
    char destination[4096]{};
    if (!::GetEnvironmentVariableA("VMHOOK_TEST_RESULT",destination,sizeof destination)) return 1;
    std::ofstream output{destination};
    try {
        vmhook::native_thread_context context;
        if (!context.valid() || !vmhook::register_class<fixture>("NativeFixture")
            || !vmhook::register_class<throwable>("java/lang/IllegalStateException")) throw std::runtime_error("context/class refused");
        auto owner=fixture::monitor();
        auto error=vmhook::make_unique<throwable>();
        if (!owner || !owner->get_instance() || !error || !error->get_instance()) throw std::runtime_error("fixture object refused");
        injected_throwable=error->get_instance();
        original_provider=pn::root_reuse_callback();
        provider_calls=0;
        pn::root_reuse_callback()=&fault_provider;
        bool allocation_failed{};
        try { vmhook::monitor_lock locked{owner}; }
        catch(const std::bad_alloc&) { allocation_failed=true; }
        pn::root_reuse_callback()=original_provider;
        fail_allocation=false;
        bool clean{};
        {
            nb::operation access{};
            clean=!pn::pending_exception(pn::rt().current_thread());
            // Restore the old baseline too, after recording the regression.
            if (!clean) pn::clear_pending_exception(pn::rt().current_thread());
        }
        const bool balanced=!owner->held();
        bool retry{};
        { vmhook::monitor_lock locked{owner}; retry=locked.owns_lock() && owner->held(); }
        output << "fault_provider_calls=" << provider_calls << '\n';
        output << "monitor_cpp_allocation=" << (allocation_failed && provider_calls==2 ? "pass" : "fail") << '\n';
        output << "monitor_pending_cleanup=" << (clean ? "pass" : "fail") << '\n';
        output << "monitor_fault_balance=" << (balanced && !owner->held() ? "pass" : "fail") << '\n';
        output << "monitor_fault_retry=" << (retry ? "pass" : "fail") << '\n';
        output << "native_complete=pass\n" << std::flush;
        return allocation_failed && clean && balanced && retry ? 0 : 2;
    } catch(const std::exception& error) {
        pn::root_reuse_callback()=original_provider;
        fail_allocation=false;
        output << "exception=" << error.what() << '\n' << std::flush;
        return 3;
    }
}
}
extern "C" BOOL __attribute__((ms_abi)) DllMain(HINSTANCE instance,DWORD reason,LPVOID) {
    if(reason==1) { ::DisableThreadLibraryCalls(instance); HANDLE thread=::CreateThread(nullptr,0,worker,nullptr,0,nullptr); if(thread) ::CloseHandle(thread); }
    return true;
}
