#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
import vmhook.windows;
#else
#include <vmhook/vmhook.hpp>
#include <fstream>
#endif
namespace {
class payload final : public vmhook::object<payload> {
public:
    using vmhook::object<payload>::object;
    auto value() const -> std::int32_t { return get_field("value")->get(); }
};
class weak final : public vmhook::object<weak> {
public:
    using vmhook::object<weak>::object;
    auto referent() const -> vmhook::member_lookup<vmhook::field_proxy> { return get_field("referent"); }
    auto get() const -> std::unique_ptr<payload> { return get_method("get")->call(); }
};
class ordinary final : public vmhook::object<ordinary> {
public:
    using vmhook::object<ordinary>::object;
    auto referent() const -> vmhook::member_lookup<vmhook::field_proxy> { return get_field("referent"); }
};
class shadowed final : public vmhook::object<shadowed> {
public:
    using vmhook::object<shadowed>::object;
    auto referent() const -> vmhook::member_lookup<vmhook::field_proxy> { return get_field("referent"); }
};
class fixture final : public vmhook::object<fixture> {
public:
    using vmhook::object<fixture>::object;
    template<class value_type>
    static auto field(std::string_view name) -> std::unique_ptr<value_type>
    { return static_members::get_field(name)->get(); }
};
template<class callback>
auto refused(callback operation) -> bool {
    try { operation(); }
    catch(const vmhook::exception& error) {
        return std::string_view{error.what()}.find("Reference.referent access is refused") != std::string_view::npos;
    }
    return false;
}
auto worker(void*) -> DWORD {
    char destination[4096]{};
    if (!::GetEnvironmentVariableA("VMHOOK_TEST_RESULT",destination,sizeof destination)) return 1;
    std::ofstream output{destination};
    try {
        vmhook::native_thread_context context;
        if (!context.valid() || !vmhook::register_class<fixture>("SpecialReferenceFixture")
            || !vmhook::register_class<payload>("SpecialReferenceFixture$Payload")
            || !vmhook::register_class<weak>("java/lang/ref/WeakReference")
            || !vmhook::register_class<ordinary>("SpecialReferenceFixture$Ordinary")
            || !vmhook::register_class<shadowed>("SpecialReferenceFixture$Shadowed"))
            throw std::runtime_error("context/class unavailable");
        auto reference=fixture::field<weak>("weak");
        auto empty=fixture::field<weak>("cleared");
        auto target=fixture::field<payload>("target");
        auto regular=fixture::field<ordinary>("ordinary");
        auto shadow=fixture::field<shadowed>("shadowed");
        auto java_null=std::make_unique<payload>(nullptr);
        auto field=reference->referent();
        if (!field) throw std::runtime_error("Reference.referent metadata missing");
        const auto checked=field->try_get();
        const bool read=refused([&]{ (void)field->get(); })
            && checked.status()==vmhook::error_code::refused && checked.error()
            && std::string_view{checked.error()->what()}.find("Reference.referent access is refused")!=std::string_view::npos;
        const bool write=refused([&]{ field->set(java_null); });
        const bool atomic=refused([&]{ (void)field->compare_exchange(target,java_null); });
        const bool raw=refused([&]{ (void)field->get_compressed_oop(); })
            && refused([&]{ (void)vmhook::field_oop(*field); });
        const bool hook=!vmhook::hook_field<weak,std::unique_ptr<payload>>("referent",
            [](vmhook::field_access<std::unique_ptr<payload>>&){ });
        auto read_ordinary=[&](auto& object) -> std::unique_ptr<payload> { return object->referent()->get(); };
        const bool initial=read_ordinary(regular)->value()==73 && read_ordinary(shadow)->value()==89;
        regular->referent()->set(target);
        shadow->referent()->set(target);
        const bool changed=read_ordinary(regular)->value()==41 && read_ordinary(shadow)->value()==41;
        const bool swapped=regular->referent()->compare_exchange(target,java_null)
            && shadow->referent()->compare_exchange(target,java_null);
        auto regular_null=read_ordinary(regular), shadow_null=read_ordinary(shadow);
        auto live=reference->get(), absent=empty->get();
        const bool ordinary_java_api=live && live->value()==41 && absent && !absent->get_instance();
        output << "reference_read_refusal=" << (read ? "pass" : "fail") << '\n';
        output << "reference_write_refusal=" << (write ? "pass" : "fail") << '\n';
        output << "reference_atomic_refusal=" << (atomic ? "pass" : "fail") << '\n';
        output << "reference_raw_refusal=" << (raw ? "pass" : "fail") << '\n';
        output << "reference_hook_refusal=" << (hook ? "pass" : "fail") << '\n';
        output << "ordinary_referent=" << (initial && changed && swapped && regular_null && shadow_null
            && !regular_null->get_instance() && !shadow_null->get_instance() ? "pass" : "fail") << '\n';
        output << "reference_java_api=" << (ordinary_java_api ? "pass" : "fail") << '\n';
        output << "native_complete=pass\n" << std::flush;
        return read && write && atomic && raw && hook && initial && changed && swapped && ordinary_java_api ? 0 : 2;
    } catch(const std::exception& error) { output << "exception=" << error.what() << '\n' << std::flush; return 3; }
}
}
extern "C" BOOL __attribute__((ms_abi)) DllMain(HINSTANCE instance,DWORD reason,LPVOID) {
    if(reason==1) { ::DisableThreadLibraryCalls(instance); HANDLE thread=::CreateThread(nullptr,0,worker,nullptr,0,nullptr); if(thread)::CloseHandle(thread); }
    return true;
}
