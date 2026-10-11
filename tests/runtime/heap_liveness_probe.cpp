#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
import vmhook.windows;
#else
#include <vmhook/vmhook.hpp>
#include <fstream>
#endif
namespace {
class fixture final : public vmhook::object<fixture> {
public:
    using vmhook::object<fixture>::object;
    auto value() const -> std::int32_t { return get_field("value")->get(); }
};
auto worker(void*) -> DWORD {
    char path[4096]{}, expected[8]{};
    if (!::GetEnvironmentVariableA("VMHOOK_TEST_RESULT",path,sizeof path)) return 1;
    ::GetEnvironmentVariableA("VMHOOK_TEST_HEAP_REFUSAL",expected,sizeof expected);
    const bool refusal=expected[0]=='1';
    std::ofstream output{path};
    try {
        vmhook::native_thread_context context;
        if (!context.valid() || !vmhook::register_class<fixture>("NativeFixture")) throw std::runtime_error("context refused");
        bool accepted{}, filtered_refusal{};
        try {
            auto snapshot=vmhook::get_instances<fixture>(1);
            accepted=snapshot.size()==1 && snapshot[0] && snapshot[0]->value()==41;
        } catch(const vmhook::exception& error) {
            filtered_refusal=std::string_view{error.what()}.find("dead-object filter")!=std::string_view::npos;
        }
        const bool support=vmhook::detail::verified_heap_iteration_available();
        output << "heap_liveness=" << ((refusal ? filtered_refusal && !support : accepted && support) ? "pass" : "fail") << '\n';
        output << "native_complete=pass\n" << std::flush;
        return refusal ? (filtered_refusal && !support ? 0 : 2) : (accepted && support ? 0 : 3);
    } catch(const std::exception& error) { output << "exception=" << error.what() << '\n' << std::flush; return 4; }
}
}
extern "C" BOOL __attribute__((ms_abi)) DllMain(HINSTANCE instance,DWORD reason,LPVOID) {
    if(reason==1) { ::DisableThreadLibraryCalls(instance); HANDLE thread=::CreateThread(nullptr,0,worker,nullptr,0,nullptr); if(thread)::CloseHandle(thread); }
    return true;
}
