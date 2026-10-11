#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
import vmhook.windows;
#else
#include <vmhook/vmhook.hpp>
#include <fstream>
#endif

namespace
{
    class fixture final : public vmhook::object<fixture>
    {
    public:
        using vmhook::object<fixture>::object;
        static auto first_player() -> std::unique_ptr<vmhook::any_object>
        { return static_members::get_method("first_player")->call(); }
        static auto second_player() -> std::unique_ptr<vmhook::any_object>
        { return static_members::get_method("second_player")->call(); }
        static auto first_holder() -> std::unique_ptr<vmhook::any_object>
        { return static_members::get_method("first_holder")->call(); }
        static auto second_holder() -> std::unique_ptr<vmhook::any_object>
        { return static_members::get_method("second_holder")->call(); }
        static auto run_a() -> std::int32_t { return static_members::get_method("run_a")->call(); }
        static auto run_b() -> std::int32_t { return static_members::get_method("run_b")->call(); }
        static auto run_a_null() -> std::int32_t { return static_members::get_method("run_a_null")->call(); }
        static auto run_player() -> std::int32_t { return static_members::get_method("run_player")->call(); }
        static auto collect() -> void
        {
            const auto result = static_members::get_method("collect")->call();
            if (!result.was_invoked() || result.threw())
            { throw std::runtime_error{"callback System.gc invocation failed"}; }
        }
    };
    class player final : public vmhook::object<player>
    {
    public:
        using vmhook::object<player>::object;
        auto id() const -> std::int32_t { return get_field("id")->get(); }
    };
    class holder final : public vmhook::object<holder>
    {
    public:
        using vmhook::object<holder>::object;
        auto id() const -> std::int32_t { return get_field("id")->get(); }
    };
    class wrong_player final : public vmhook::object<wrong_player>
    { public: using vmhook::object<wrong_player>::object; };
    class wrong_holder final : public vmhook::object<wrong_holder>
    { public: using vmhook::object<wrong_holder>::object; };
    class base_wrapper final : public vmhook::object<base_wrapper>
    { public: using vmhook::object<base_wrapper>::object; };
    using row_type = vmhook::array<player>;
    using matrix_type = vmhook::array<row_type>;
    using wrong_matrix_type = vmhook::array<vmhook::array<wrong_player>>;
    constexpr std::string_view accept_descriptor{
        "(LMethodBindingFixture$Player;[[LMethodBindingFixture$Player;)I"};

    auto require(const bool correct, const char* reason) -> void
    { if (!correct) { throw std::runtime_error{reason}; } }

    // Every construction uses the common holder/player types. An active method
    // callback must select its captured defining loader, rather than globals.
    auto construct_in_scope(const bool second_loader) -> bool
    {
        const auto object = vmhook::make_unique<player>(std::int32_t{second_loader ? 873 : 841});
        const auto receiver = vmhook::make_unique<holder>(std::int32_t{second_loader ? 722 : 711});
        const auto row = vmhook::make_array<player>(1);
        row->set(0, object);
        const auto matrix = vmhook::make_array<row_type>(1);
        matrix->set(0, row);
        fixture::collect();
        const auto nested = matrix->get(0)->get(0);
        return object->id() == (second_loader ? 873 : 841)
            && receiver->id() == (second_loader ? 722 : 711)
            && nested->id() == object->id()
            && object->is<wrong_player>() == second_loader
            && receiver->is<wrong_holder>() == second_loader
            && matrix->is<wrong_matrix_type>() == second_loader;
    }

    auto worker(void*) -> DWORD
    {
        const char* destination = std::getenv("VMHOOK_METHOD_BINDING_REPORT");
        std::ofstream output{destination ? destination : "vmhook-method-binding-report.txt"};
        output.setf(std::ios::unitbuf);
        bool valid{true};
        const auto check = [&](const std::string_view name, const bool correct)
        { output << name << '=' << (correct ? "pass" : "fail") << '\n'; valid = valid && correct; };
        try
        {
            vmhook::native_thread_context context{"VMHook-method-binding"};
            check("context", context.valid());
            require(context.valid(), "method-binding native context");
            require(vmhook::register_class<fixture>("MethodBindingFixture"), "fixture class registration");
            const auto first_player = fixture::first_player();
            const auto second_player = fixture::second_player();
            const auto first_holder = fixture::first_holder();
            const auto second_holder = fixture::second_holder();
            const bool registered = vmhook::register_class<player>("MethodBindingFixture$Player", first_player)
                && vmhook::register_class<holder>("MethodBindingFixture$Holder", first_holder)
                && vmhook::register_class<wrong_player>("MethodBindingFixture$Player", second_player)
                && vmhook::register_class<wrong_holder>("MethodBindingFixture$Holder", second_holder)
                && vmhook::register_class<base_wrapper>("java/lang/Object");
            check("registration", registered);
            require(registered, "exact method receiver/argument registrations");
            check("two_loader_identity", first_player->is<player>() && !first_player->is<wrong_player>()
                && second_player->is<wrong_player>() && !second_player->is<player>()
                && first_holder->is<holder>() && !first_holder->is<wrong_holder>()
                && second_holder->is<wrong_holder>() && !second_holder->is<holder>());

            unsigned refused_callback_calls{};
            const auto refuse = [&](const vmhook::method_hook_id observer) -> bool
            { if (observer) { (void)vmhook::remove_hook(observer); } return observer == 0; };
            check("wrong_loader_receiver_refusal", refuse(vmhook::add_hook<holder>("accept", accept_descriptor,
                [&](vmhook::return_value&, const std::unique_ptr<wrong_holder>&,
                    const std::unique_ptr<player>&, const std::unique_ptr<matrix_type>&)
                { ++refused_callback_calls; })));
            check("wrong_loader_argument_refusal", refuse(vmhook::add_hook<holder>("accept", accept_descriptor,
                [&](vmhook::return_value&, const std::unique_ptr<holder>&,
                    const std::unique_ptr<wrong_player>&, const std::unique_ptr<matrix_type>&)
                { ++refused_callback_calls; })));
            check("wrong_loader_array_refusal", refuse(vmhook::add_hook<holder>("accept", accept_descriptor,
                [&](vmhook::return_value&, const std::unique_ptr<holder>&,
                    const std::unique_ptr<player>&, const std::unique_ptr<wrong_matrix_type>&)
                { ++refused_callback_calls; })));
            const bool narrowing_refused = refuse(vmhook::add_hook<holder>("accept_any", "(Ljava/lang/Object;)I",
                [&](vmhook::return_value&, const std::unique_ptr<holder>&, const std::unique_ptr<player>&)
                { ++refused_callback_calls; }));
            unsigned base_calls{};
            bool base_argument_valid{true};
            const auto base_hook = vmhook::add_hook<holder>("accept_player", "(LMethodBindingFixture$Player;)I",
                [&](vmhook::return_value& ret, const std::unique_ptr<holder>& self,
                    const std::unique_ptr<base_wrapper>& argument)
                {
                    ++base_calls;
                    fixture::collect();
                    base_argument_valid = base_argument_valid && self && self->id() == 11
                        && argument && argument->get_instance() && argument->is<player>();
                    require(base_argument_valid, "declared Player was not admitted as Object callback argument");
                    ret.set(std::int32_t{303});
                });
            const auto base_result = fixture::run_player();
            const auto base_stats = vmhook::observer_stats(base_hook);
            const bool base_removed = base_hook && vmhook::remove_hook(base_hook);
            check("base_to_subtype_refusal", narrowing_refused && base_hook && base_result == 303
                && base_calls == 1 && base_argument_valid && base_stats && base_stats->failures == 0
                && base_stats->calls == 1 && base_stats->active_callbacks == 0
                && base_removed && fixture::run_player() == 52 && base_calls == 1);
            require(valid, "incompatible method callback was admitted");

            unsigned calls_a{}, calls_b{}, calls_null{};
            bool captured_a{true}, captured_b{true}, array_scope{true}, nested_scope{true};
            bool constructor_scope{true}, null_owners{true};
            const auto hook_a = vmhook::add_hook<holder>("accept", accept_descriptor,
                [&](vmhook::return_value& ret, const std::unique_ptr<holder>& self,
                    const std::unique_ptr<player>& argument, const std::unique_ptr<matrix_type>& matrix)
                {
                    ++calls_a;
                    require(self && argument && matrix, "empty C++ callback owner");
                    require(vmhook::detail::active_callback_class_bindings != nullptr,
                        "method callback binding scope is missing");
                    fixture::collect();
                    captured_a = captured_a && self->id() == 11 && self->is<holder>()
                        && !self->is<wrong_holder>();
                    if (!argument->get_instance() || !matrix->get_instance())
                    {
                        ++calls_null;
                        null_owners = null_owners && !argument->get_instance() && !matrix->get_instance();
                        require(null_owners && captured_a, "Java null callback wrapper identity");
                        ret.set(std::int32_t{301});
                        return;
                    }
                    captured_a = captured_a && argument->id() == 41 && argument->is<player>()
                        && !argument->is<wrong_player>();
                    array_scope = array_scope && matrix->size() == 1 && matrix->get(0)->size() == 1
                        && matrix->get(0)->get(0)->id() == 41 && !matrix->is<wrong_matrix_type>();
                    constructor_scope = constructor_scope && construct_in_scope(false);
                    require(captured_a && array_scope && constructor_scope, "captured loader A callback types");
                    nested_scope = nested_scope && fixture::run_b() == 202;
                    constructor_scope = constructor_scope && construct_in_scope(false);
                    nested_scope = nested_scope && vmhook::detail::active_callback_class_bindings != nullptr
                        && !self->is<wrong_holder>() && !argument->is<wrong_player>();
                    fixture::collect();
                    require(nested_scope && constructor_scope && matrix->get(0)->get(0)->id() == 41,
                        "outer binding scope was not restored after nested B callback");
                    ret.set(std::int32_t{101});
                });
            check("hook_a", hook_a != 0);
            require(hook_a != 0, "loader A method hook installation");
            const bool rebound = vmhook::register_class<holder>("MethodBindingFixture$Holder", second_holder)
                && vmhook::register_class<player>("MethodBindingFixture$Player", second_player);
            check("captured_rebinding", rebound && first_holder->is<wrong_holder>() == false
                && second_holder->is<holder>() && second_player->is<player>()
                && vmhook::detail::active_callback_class_bindings == nullptr);
            require(rebound, "global method wrapper rebind");
            const auto hook_b = vmhook::add_hook<holder>("accept", accept_descriptor,
                [&](vmhook::return_value& ret, const std::unique_ptr<holder>& self,
                    const std::unique_ptr<player>& argument, const std::unique_ptr<matrix_type>& matrix)
                {
                    ++calls_b;
                    require(self && argument && matrix && self->get_instance() && argument->get_instance()
                        && matrix->get_instance(), "empty loader B callback object");
                    fixture::collect();
                    captured_b = captured_b && self->id() == 22 && argument->id() == 73
                        && self->is<wrong_holder>() && argument->is<wrong_player>();
                    array_scope = array_scope && matrix->get(0)->get(0)->id() == 73
                        && matrix->is<wrong_matrix_type>();
                    constructor_scope = constructor_scope && construct_in_scope(true);
                    require(captured_b && array_scope && constructor_scope
                        && vmhook::detail::active_callback_class_bindings != nullptr,
                        "captured loader B callback types");
                    fixture::collect();
                    require(matrix->get(0)->get(0)->id() == 73, "loader B array argument lost during GC");
                    ret.set(std::int32_t{202});
                });
            check("hook_b", hook_b != 0);
            require(hook_b != 0, "loader B method hook installation");
            const auto result_a = fixture::run_a();
            const auto result_b = fixture::run_b();
            const auto result_null = fixture::run_a_null();
            const auto stats_a = vmhook::observer_stats(hook_a);
            const auto stats_b = vmhook::observer_stats(hook_b);
            const bool clean_callbacks = stats_a && stats_b && stats_a->failures == 0 && stats_b->failures == 0
                && stats_a->calls == 2 && stats_b->calls == 2
                && stats_a->active_callbacks == 0 && stats_b->active_callbacks == 0;
            check("array_argument_scope", array_scope && result_a == 101 && result_b == 202 && clean_callbacks);
            check("nested_binding_scope", nested_scope && captured_a && captured_b && calls_a == 2 && calls_b == 2);
            check("callback_constructor_scope", constructor_scope && clean_callbacks);
            check("java_null_wrapper", null_owners && calls_null == 1 && result_null == 301 && clean_callbacks);
            check("callback_tls_restored", vmhook::detail::active_callback_class_bindings == nullptr
                && construct_in_scope(true) && vmhook::detail::active_callback_class_bindings == nullptr);
            require(vmhook::remove_hook(hook_a) && vmhook::remove_hook(hook_b), "method observer removal");
            const auto calls_before_a = calls_a;
            const auto calls_before_b = calls_b;
            check("original_after_removal", fixture::run_a() == 93 && fixture::run_b() == 168
                && fixture::run_a_null() == 11 && calls_a == calls_before_a && calls_b == calls_before_b
                && refused_callback_calls == 0);
            const bool reclaimed = vmhook::reclaim_hook_callbacks() && vmhook::hook_callbacks_idle();
            const auto resources = vmhook::hook_resources();
            vmhook::shutdown_hooks();
            const auto stopped = vmhook::hook_resources();
            check("reclamation", reclaimed && resources.active_observers == 0
                && stopped.retained_method_policies == 0 && stopped.physical_stubs == 0
                && fixture::run_a() == 93 && fixture::run_b() == 168);
            output << "method_binding_proof=" << (valid ? "pass" : "fail") << '\n';
            return valid ? 0 : 2;
        }
        catch (const std::exception& error)
        { output << "exception=" << error.what() << "\nmethod_binding_proof=fail\n"; return 3; }
    }
}

extern "C" BOOL __attribute__((ms_abi)) DllMain(HINSTANCE self, DWORD reason, LPVOID)
{
    if (reason == 1)
    {
        ::DisableThreadLibraryCalls(self);
        if (auto thread = ::CreateThread(nullptr, 0, &worker, nullptr, 0, nullptr))
        { vmhook::windows::CloseHandle(thread); }
    }
    return true;
}
