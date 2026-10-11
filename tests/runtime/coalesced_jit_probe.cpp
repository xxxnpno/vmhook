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
        static auto invoke(const std::string_view name, std::int32_t value) -> std::int32_t
        { return static_members::get_method(name, "(I)I")->call(value); }
        static auto warm_up() -> void { (void)static_members::get_method("warm_up", "()V")->call(); }
        static auto warm_named(const std::string_view name) -> void
        {
            const auto method{static_members::get_method(name, "(I)I")};
            for (unsigned index{}; index < 20000; ++index)
            { (void)static_cast<std::int32_t>(method->call(std::int32_t{7})); }
        }
    };
    namespace jit = vmhook::detail::jit_backend;
    namespace pn = vmhook::detail::private_native;
    constexpr std::array<std::string_view, 8> names{
        "target", "caller", "unrelated", "folded_target", "folded_caller",
        "handle_caller", "interface_caller", "lambda_caller"};
    struct snapshot final
    {
        std::array<vmhook::hotspot::method*, names.size()> methods{};
        std::array<void*, names.size()> code{};
        std::array<unsigned, names.size()> levels{};
        bool visible_inline{}, folded_metadata{}, excluded{}, okay{};
    };
    auto capture(void* const opaque) noexcept -> void
    {
        auto& value{*static_cast<snapshot*>(opaque)};
        try
        {
            auto* const klass{vmhook::detail::registered_class_klass<fixture>()};
            vmhook::hotspot::method** methods{};
            std::int32_t count{};
            if (!vmhook::hotspot::safe_klass_methods(klass, methods, count)) { return; }
            const auto& config{jit::config()};
            const auto offset{pn::rt().off("nmethod", "_comp_level")};
            const auto width{pn::rt().enum_value("nmethod", "storage_size__comp_level")};
            for (std::size_t index{}; index < names.size(); ++index)
            {
                for (std::int32_t member{}; member < count; ++member)
                {
                    if (methods[member] && methods[member]->get_name() == names[index])
                    { value.methods[index] = methods[member]; break; }
                }
                if (!value.methods[index]) { return; }
                value.code[index] = pn::load<void*>(value.methods[index], config.method_code_offset);
                if (value.code[index])
                {
                    value.levels[index] = width == 1 ? pn::load<std::uint8_t>(value.code[index], offset)
                        : pn::load<std::uint32_t>(value.code[index], offset);
                }
            }
            value.visible_inline = jit::metadata_contains(value.code[1], value.methods[0]);
            value.folded_metadata = jit::metadata_contains(value.code[4], value.methods[3]);
            value.excluded = jit::method_is_excluded(value.methods[0]);
            value.okay = true;
        }
        catch (...) {}
    }
    auto sample() -> snapshot
    {
        snapshot result{};
        if (!jit::at_safepoint(&capture, &result) || !result.okay)
        { throw std::runtime_error{"Compiled method snapshot unavailable"}; }
        return result;
    }
    auto warm_compiled(const bool initial) -> snapshot
    {
        for (unsigned attempt{}; attempt < 80; ++attempt)
        {
            if (initial) { fixture::warm_up(); }
            // An ordinary private JavaCalls invocation cannot be folded into
            // this native driver. It gives the independent method its own
            // invocation profile without JVM compiler flags or WhiteBox.
            fixture::warm_named("unrelated");
            const auto result{sample()};
            if (result.levels[2] == 4 && (!initial
                || (result.code[0] && result.levels[1] == 4 && result.levels[4] == 4)))
            { return result; }
            std::this_thread::sleep_for(std::chrono::milliseconds{25});
        }
        const auto final{sample()};
        throw std::runtime_error{std::format("Required compiled methods unavailable: target={}, caller={}, unrelated={}, folded={}",
            final.levels[0], final.levels[1], final.levels[2], final.levels[4])};
    }
    auto worker(void*) -> DWORD
    {
        const auto* destination{std::getenv("VMHOOK_COALESCED_JIT_REPORT")};
        std::ofstream output{destination ? destination : "coalesced-jit-report.txt"};
        output.setf(std::ios::unitbuf);
        bool valid{true};
        const auto check = [&](const std::string_view name, const bool result)
        { output << std::format("{}={}\n", name, result ? "pass" : "fail"); valid = valid && result; };
        try
        {
            vmhook::native_thread_context context{"VMHook-coalesced-JIT"};
            check("context", context.valid());
            if (!context.valid()) { return 2; }
            check("registration", vmhook::register_class<fixture>("CoalescedJitFixture"));
            const auto before{warm_compiled(true)};
            check("compiled_baseline", before.code[0] && before.levels[1] == 4
                && before.levels[2] == 4 && before.levels[4] == 4 && !before.excluded);
            check("visible_inline_metadata", before.visible_inline);
            // A missing Method here is an actual optimized C2 caller, not a
            // timing estimate. It disproves complete metadata-only coverage.
            output << std::format("folded_leaf_method_metadata={}\n", before.folded_metadata ? "present" : "absent");
            unsigned first_calls{}, second_calls{};
            const auto first{vmhook::add_hook<fixture>("target", "(I)I",
                [&](vmhook::return_value& result, const std::int32_t value)
                { ++first_calls; (void)result.set_arg(0, value + 10); }, {.priority = 10})};
            check("first_hook", first != 0);
            const auto first_snapshot{sample()};
            check("first_global_invalidation", first_snapshot.excluded && !first_snapshot.code[0]
                && first_snapshot.code[2] != before.code[2]);
            check("direct_first_hook", fixture::invoke("target", 7) == 18);
            check("inlined_first_hook", fixture::invoke("caller", 7) == 54);
            check("indirect_first_hook", fixture::invoke("handle_caller", 7) == 18
                && fixture::invoke("interface_caller", 7) == 18 && fixture::invoke("lambda_caller", 7) == 18);
            const auto warmed{warm_compiled(false)};
            check("unrelated_recompiled", warmed.levels[2] == 4 && warmed.code[2]);
            snapshot compiled_caller{};
            for (unsigned attempt{}; attempt < 80; ++attempt)
            {
                fixture::warm_named("caller"); compiled_caller = sample();
                if (compiled_caller.levels[1] == 4) { break; }
                std::this_thread::sleep_for(std::chrono::milliseconds{25});
            }
            check("caller_recompiled_under_policy", compiled_caller.levels[1] == 4
                && compiled_caller.excluded && fixture::invoke("caller", 7) == 54);
            const auto unrelated_code{compiled_caller.code[2]};
            const auto caller_code{compiled_caller.code[1]};
            const auto second{vmhook::add_hook<fixture>("target", "(I)I",
                [&](vmhook::return_value& result, const std::int32_t value)
                { ++second_calls; result.set(value + 100); }, {.priority = 20})};
            check("second_hook", second != 0);
            const auto second_snapshot{sample()};
            check("coalesced_code_retention", second_snapshot.code[2] == unrelated_code
                && second_snapshot.levels[2] == 4 && second_snapshot.excluded && !second_snapshot.code[0]);
            check("coalesced_caller_code_retention", second_snapshot.code[1] == caller_code && second_snapshot.levels[1] == 4);
            check("coalesced_direct_semantics", fixture::invoke("target", 7) == 117);
            check("coalesced_inlined_semantics", fixture::invoke("caller", 7) == 351);
            check("coalesced_indirect_semantics", fixture::invoke("handle_caller", 7) == 117
                && fixture::invoke("interface_caller", 7) == 117 && fixture::invoke("lambda_caller", 7) == 117);
            std::shared_ptr<void> rejected_policy{};
            const bool rejected{!jit::install_method_policy(second_snapshot.methods[0],
                [](void*) noexcept { return false; }, nullptr, rejected_policy)};
            const auto after_rejection{sample()};
            check("rejected_addition_keeps_policy", rejected && !rejected_policy && after_rejection.excluded
                && after_rejection.code[2] == unrelated_code && fixture::invoke("target", 7) == 117);
            check("remove_second", vmhook::remove_hook(second) && fixture::invoke("target", 7) == 18);
            check("remove_first", vmhook::remove_hook(first) && fixture::invoke("target", 7) == 8);
            const auto dormant_before{sample()};
            const auto dormant{vmhook::add_hook<fixture>("target", "(I)I",
                [](vmhook::return_value& result, const std::int32_t value) { result.set(value + 200); })};
            const auto dormant_after{sample()};
            check("dormant_policy_reuse", dormant && dormant_before.code[2] == dormant_after.code[2]
                && dormant_after.code[1] == caller_code && dormant_after.excluded && fixture::invoke("caller", 7) == 621
                && vmhook::remove_hook(dormant) && fixture::invoke("caller", 7) == 24);
            // Fault injection models a first transaction that published its
            // reason/exclusion but never completed invalidation. Exact flags
            // and Method::_code == nullptr must not independently authorize
            // another public observer publication.
            struct incomplete_install final
            {
                vmhook::hotspot::method* method{};
                bool changed{}, guard{}, identity_guard{}, field_guard{}, restored_guard{};
            };
            incomplete_install incomplete{sample().methods[0]};
            const bool injected{jit::at_safepoint([](void* const opaque) noexcept
            {
                auto& request{*static_cast<incomplete_install*>(opaque)};
                auto* const policy{jit::find_policy(request.method)};
                if (!policy) { return; }
#if !defined(VMHOOK_TEST_BASELINE)
                jit::method_policy uncommitted{};
                uncommitted.method = policy->method; uncommitted.const_method = policy->const_method;
                uncommitted.excluded = true; uncommitted.reasons.store(jit::method_reason);
                bool published{};
                jit::existing_policy_install denied{&uncommitted, request.method,
                    [](void* const value) noexcept { *static_cast<bool*>(value) = true; return true; }, &published};
                jit::commit_existing_policy(&denied);
                request.guard = !denied.eligible && !denied.okay && !published;
                uncommitted.invalidation_complete = true;
                uncommitted.const_method = nullptr;
                denied.eligible = false; denied.okay = false;
                jit::commit_existing_policy(&denied);
                request.identity_guard = !denied.eligible && !denied.okay && !published;
                uncommitted.const_method = policy->const_method;
                uncommitted.reasons.store(jit::field_reason);
                jit::commit_existing_policy(&denied);
                request.field_guard = !denied.eligible && !denied.okay && !published;
                // This detached probe owns no Method bits. Restoring it only
                // clears its local provenance and zero owned masks.
                jit::restore(uncommitted);
                request.restored_guard = !uncommitted.invalidation_complete && !uncommitted.excluded;
                policy->invalidation_complete = false;
                request.changed = true;
#endif
            }, &incomplete)};
            check("incomplete_invalidation_guard", injected && incomplete.changed && incomplete.guard);
            check("changed_identity_guard", injected && incomplete.identity_guard);
            check("field_only_policy_guard", injected && incomplete.field_guard);
            check("restoration_clears_invalidation_proof", injected && incomplete.restored_guard);
            const auto recovery_before{sample()};
            const auto recovered{vmhook::add_hook<fixture>("target", "(I)I",
                [](vmhook::return_value& result, const std::int32_t value) { result.set(value + 300); })};
            const auto recovery_after{sample()};
            check("public_incomplete_policy_recovery", recovered && recovery_after.code[2] != recovery_before.code[2]
                && recovery_after.code[1] != recovery_before.code[1] && recovery_after.excluded
                && fixture::invoke("target", 7) == 307 && fixture::invoke("caller", 7) == 921
                && vmhook::remove_hook(recovered) && fixture::invoke("target", 7) == 8);
            // This new method's first transaction must still invalidate globally:
            // a fully folded leaf cannot be admitted by metadata-only scans.
            const auto folded{vmhook::add_hook<fixture>("folded_target", "()I",
                [](vmhook::return_value& result) { result.set(std::int32_t{99}); })};
            check("folded_leaf_fallback", folded && fixture::invoke("folded_caller", 7) == 106
                && vmhook::remove_hook(folded) && fixture::invoke("folded_caller", 7) == 44);
            check("observer_lifecycle", first_calls >= 11 && second_calls >= 6
                && vmhook::reclaim_hook_callbacks() && vmhook::hook_callbacks_idle()
                && vmhook::hook_resources().active_observers == 0);
            vmhook::shutdown_hooks();
            const auto restored{sample()};
            check("shutdown_restores_policy", !restored.excluded
                && fixture::invoke("target", 7) == 8 && fixture::invoke("caller", 7) == 24);
            const auto recompiled{warm_compiled(true)};
            check("shutdown_recompilation", recompiled.code[0] && recompiled.levels[1] == 4
                && recompiled.levels[2] == 4 && fixture::invoke("folded_caller", 7) == 44);
            check("coalesced_jit_proof", valid);
            return valid ? 0 : 3;
        }
        catch (const std::exception& error)
        { output << std::format("exception={}\ncoalesced_jit_proof=fail\n", error.what()); return 4; }
    }
}
extern "C" BOOL __attribute__((ms_abi)) DllMain(HINSTANCE self, DWORD reason, LPVOID)
{
    if (reason == 1)
    {
        ::DisableThreadLibraryCalls(self);
        if (HANDLE thread{::CreateThread(nullptr, 0, worker, nullptr, 0, nullptr)}) { ::CloseHandle(thread); }
    }
    return true;
}
