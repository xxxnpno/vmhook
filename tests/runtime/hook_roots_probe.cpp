#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
import vmhook.windows;
#else
#include <vmhook/vmhook.hpp>
#include <format>
#endif

namespace allocation_fault
{
    class injected_failure final : public std::bad_alloc {};
    inline thread_local std::size_t target{}, observed{}, skip{};
    inline thread_local bool measure{}, require_field_mode{}, rejected{};
}
[[gnu::noinline]] auto operator new(std::size_t size) -> void*
{
    if (allocation_fault::measure) { allocation_fault::observed = size; }
    if (size == allocation_fault::target && (!allocation_fault::require_field_mode
        || vmhook::detail::jit_backend::field_mode.load(std::memory_order_acquire)))
    {
        if (allocation_fault::skip) { --allocation_fault::skip; }
        else
        { allocation_fault::target = 0; allocation_fault::rejected = true;
          throw allocation_fault::injected_failure{}; }
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
        static auto target(std::int32_t value) -> std::int32_t
        { return static_members::get_method("target", "(I)I")->call(value); }
        static auto read_counter() -> std::int32_t
        { return static_members::get_method("read_counter", "()I")->call(); }
    };
    template<typename function_type>
    auto fails(std::size_t size, function_type&& function, std::size_t skip = 0,
               bool require_field_mode = false) -> bool
    {
        allocation_fault::target = size;
        allocation_fault::skip = skip;
        allocation_fault::rejected = false;
        allocation_fault::require_field_mode = require_field_mode;
        bool propagated{};
        try { function(); }
        catch (const allocation_fault::injected_failure&) { propagated = true; }
        catch (...) {}
        allocation_fault::target = 0;
        allocation_fault::require_field_mode = false;
        return propagated && allocation_fault::rejected;
    }
    auto install_field() -> bool
    {
        return vmhook::hook_field<fixture, std::int32_t>("counter", [](auto& event) {
            if (!event.is_write()) { event.set(77); }
        });
    }
    auto install_method() -> vmhook::method_hook_id
    {
        return vmhook::add_hook<fixture>("target", "(I)I", [](vmhook::return_value& result, std::int32_t) {
            result.set(std::int32_t{99});
        });
    }
    auto worker(void*) -> DWORD
    {
        const auto* path{std::getenv("VMHOOK_TEST_RESULT")};
        std::ofstream output{path ? path : "hook-roots-result.txt"};
        output.setf(std::ios::unitbuf);
        bool passed{true};
        const auto check = [&](std::string_view name, bool correct)
        { output << std::format("{}={}\n", name, (correct ? "pass" : "fail")); passed = passed && correct; };
        try
        {
            namespace detail = vmhook::detail;
            vmhook::native_thread_context context{"hook-root-errors"};
            check("context", context.valid());
            if (!context.valid()) { return 2; }
            check("preparation_cpp_oom", fails(sizeof(detail::persistent_hook_roots::keeper), [] {
                (void)detail::prepare_hook_roots_checked();
            }));
            check("preparation_failure_cleanup", context.valid()
                && !detail::persistent_hook_roots::prepared_keeper.load());
            check("preparation_retry", detail::prepare_hook_roots_checked()
                && vmhook::register_class<fixture>("RootFixture"));
            allocation_fault::measure = true;
            auto measurement{std::make_shared<detail::persistent_hook_roots::ticket>()};
            allocation_fault::measure = false;
            const auto ticket_size{allocation_fault::observed};
            measurement.reset();
            output << std::format("ticket_allocation={}\n", ticket_size);
            vmhook::hotspot::klass* klass{};
            vmhook::hotspot::method* method{};
            {
                detail::native_backend::operation operation{};
                klass = detail::registered_class_klass<fixture>();
                vmhook::hotspot::method** methods{};
                std::int32_t count{};
                if (vmhook::hotspot::safe_klass_methods(klass, methods, count))
                {
                    for (std::int32_t index{}; index < count; ++index)
                    {
                        if (methods[index] && methods[index]->get_name() == "target"
                            && methods[index]->get_signature() == "(I)I")
                        { method = methods[index]; break; }
                    }
                }
                check("ticket_cpp_oom", fails(ticket_size, [&] {
                    (void)detail::retain_hook_klass_checked(klass);
                }));
            }
            detail::flush_hook_roots();
            check("ticket_failure_cleanup", context.valid() && method
                && !detail::jit_backend::method_is_excluded(method));
            // Warm root bookkeeping before targeting the keeper's shared ticket.
            if (fixture::target(3) != 4 || fixture::read_counter() != 7)
            { throw std::runtime_error{"Unhooked metadata warm-up failed"}; }
            // The local root map node has the same size as a keeper ticket.
            // Field installation also copies its binding vector and acquires a
            // second local root before retaining the field's declaring Class.
            check("method_pin_cpp_oom", fails(ticket_size, [] { (void)install_method(); }, 1));
            check("field_binding_cpp_oom", fails(ticket_size, [] { (void)install_field(); }, 1));
            check("field_metadata_cpp_oom", fails(ticket_size, [] { (void)install_field(); }, 4));
            (void)detail::prepare_callback_return_roots();
            check("method_policy_cpp_oom", fails(sizeof(detail::jit_backend::method_policy), [&] {
                std::shared_ptr<void> policy{};
                (void)detail::jit_backend::install_method_policy(method, [](void*) noexcept { return true; },
                                                               nullptr, policy);
            }));
            const auto& config{detail::jit_backend::config()};
            const auto jobs_before{*config.compile_jobs};
            const bool compiler_before{*config.use_compiler};
            check("field_policy_cpp_oom", fails(ticket_size, [] { (void)install_field(); }, 0, true));
            detail::flush_hook_roots();
            check("failure_cleanup", context.valid() && !detail::jit_backend::field_mode.load()
                && *config.compile_jobs == jobs_before && *config.use_compiler == compiler_before
                && vmhook::hook_resources().active_observers == 0
                && !detail::jit_backend::method_is_excluded(method)
                && fixture::target(3) == 4 && fixture::read_counter() == 7);
            const auto installed{install_method()};
            check("method_retry", installed && fixture::target(3) == 99);
            check("method_original", vmhook::remove_hook(installed) && fixture::target(3) == 4);
            vmhook::shutdown_hooks();
            check("field_retry", install_field() && fixture::read_counter() == 77);
            check("field_original", vmhook::unhook_field<fixture>("counter") && fixture::read_counter() == 7);
            check("closed", context.close());
            check("contextless_cpp_oom", fails(sizeof(detail::private_native::scoped_attachment), [] {
                (void)install_method();
            }));
            const auto contextless{install_method()};
            vmhook::native_thread_context retry{"hook-root-retry"};
            check("contextless_retry", contextless && retry.valid() && fixture::target(3) == 99
                && vmhook::remove_hook(contextless) && fixture::target(3) == 4);
            vmhook::shutdown_hooks();
            check("hook_roots_complete", passed && retry.close());
            return passed ? 0 : 3;
        }
        catch (const std::exception& error)
        { output << std::format("exception={}\n", error.what()); return 4; }
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
