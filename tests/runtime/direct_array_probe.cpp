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
namespace { thread_local bool fail_allocation{}; }
__attribute__((noinline)) void* operator new(std::size_t size) {
    if (fail_allocation) { fail_allocation=false; throw std::bad_alloc{}; }
    if (void* value=std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc{};
}
__attribute__((noinline)) void operator delete(void* value) noexcept { std::free(value); }
__attribute__((noinline)) void operator delete(void* value,std::size_t) noexcept { std::free(value); }
namespace {
namespace pn=vmhook::detail::private_native;
class fixture final : public vmhook::object<fixture> {
public:
    using vmhook::object<fixture>::object;
    static auto holder() -> std::unique_ptr<fixture> { return static_members::get_field("holder")->get(); }
    static auto epoch() -> std::int64_t { return static_members::get_method("epoch")->call(); }
    auto field(std::string_view name) const -> vmhook::member_lookup<vmhook::field_proxy> { return get_field(name); }
    auto verify(std::int32_t value,bool flag) const -> bool { return get_method("verify")->call(value,flag); }
    auto string_unchanged() const -> bool { return get_method("string_unchanged")->call(); }
};
thread_local pn::root_slot_provider original_provider{};
thread_local std::size_t fault_roots{};
auto fail_root_allocation(void* thread) noexcept -> void** {
    void** slot=original_provider ? original_provider(thread) : nullptr;
    ++fault_roots;
    fail_allocation=true;
    return slot;
}
template<class callable>
auto refused(callable call) -> bool {
    try { call(); } catch(const vmhook::exception&) { return true; }
    return false;
}
auto worker(void*) -> DWORD {
    char destination[4096]{};
    if (!::GetEnvironmentVariableA("VMHOOK_TEST_RESULT",destination,sizeof destination)) return 1;
    std::ofstream output{destination};
    try {
        std::vector<std::int32_t> integers(32768);
        std::vector<bool> booleans(32768);
        std::vector<char> characters(32768,static_cast<char>(0xE9));
        const std::vector<std::string> strings{"replacement"};
        std::optional<vmhook::field_proxy> saved;
        bool batch=true, observed{}, allocation{}, recovered{}, layout{}, string_policy{};
        {
            vmhook::native_thread_context context;
            if (!context.valid() || !vmhook::register_class<fixture>("DirectArrayFixture")) throw std::runtime_error("context/class unavailable");
            auto owner=fixture::holder();
            auto ints=owner->field("integers"), bools=owner->field("booleans"), chars=owner->field("characters"), texts=owner->field("strings");
            const auto initial_epoch=fixture::epoch();
            for (std::int32_t iteration=1;iteration<=64;++iteration) {
                std::fill(integers.begin(),integers.end(),iteration);
                std::fill(booleans.begin(),booleans.end(),(iteration%2)!=0);
                // Direct public calls: there is no caller-owned VM operation.
                vmhook::set_prim_array(*ints,integers);
                vmhook::set_bool_array(*bools,booleans);
                vmhook::set_prim_array(*chars,characters);
                batch=owner->verify(iteration,(iteration%2)!=0) && batch;
                if (!batch) break;
            }
            observed=fixture::epoch()>initial_epoch;
            layout=refused([&]{ vmhook::set_prim_array(*ints,std::vector<std::int64_t>{1}); });
            // Ordinary moving collectors must never receive raw String[] stores.
            string_policy=refused([&]{ vmhook::set_str_array(*texts,strings); }) && owner->string_unchanged();
            auto absent=owner->field("absent");
            vmhook::set_prim_array(*absent,integers); // Genuine Java null is a no-op.
            original_provider=pn::root_reuse_callback(); fault_roots=0;
            pn::root_reuse_callback()=&fail_root_allocation;
            try { vmhook::set_prim_array(*ints,integers); }
            catch(const std::bad_alloc&) { allocation=true; }
            pn::root_reuse_callback()=original_provider; fail_allocation=false;
            allocation=allocation && fault_roots==1 && pn::thread_state(pn::rt().current_thread())==4;
            vmhook::set_prim_array(*ints,integers);
            recovered=owner->verify(64,false);
            saved=*ints;
        }
        const bool expired=refused([&]{ vmhook::set_prim_array(*saved,integers); });
        output << "direct_array_batch=" << (batch && observed ? "pass" : "fail") << '\n';
        output << "direct_array_cpp_allocation=" << (allocation ? "pass" : "fail") << '\n';
        output << "direct_array_recovery=" << (recovered ? "pass" : "fail") << '\n';
        output << "direct_array_refusal=" << (layout && expired ? "pass" : "fail") << '\n';
        output << "direct_array_string_policy=" << (string_policy ? "pass" : "fail") << '\n';
        output << "native_complete=pass\n" << std::flush;
        return batch && observed && allocation && recovered && layout && expired && string_policy ? 0 : 2;
    } catch(const std::exception& error) {
        pn::root_reuse_callback()=original_provider;fail_allocation=false;
        output << "exception=" << error.what() << '\n' << std::flush;return 3;
    }
}
}
extern "C" BOOL __attribute__((ms_abi)) DllMain(HINSTANCE instance,DWORD reason,LPVOID) {
    if(reason==1) { ::DisableThreadLibraryCalls(instance);HANDLE thread=::CreateThread(nullptr,0,worker,nullptr,0,nullptr);if(thread)::CloseHandle(thread); }
    return true;
}
