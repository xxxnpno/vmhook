#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
import vmhook.windows;
#else
#include <vmhook/vmhook.hpp>
#include <format>
#include <fstream>
#endif

namespace
{
    namespace pn = vmhook::detail::private_native;

    class jit_fixture : public vmhook::object<jit_fixture>
    {
    public:
        using vmhook::object<jit_fixture>::object;
        static auto count(std::string_view name) -> std::int64_t
        {
            const auto field = static_members::get_field(name);
            return field ? static_cast<std::int64_t>(field->get()) : -1;
        }
        static auto start_future() -> void { static_members::get_field("futureRequested")->set(true); }
    };

    class field_fixture : public vmhook::object<field_fixture>
    {
    public:
        using vmhook::object<field_fixture>::object;
        static auto gc_epoch() -> std::int32_t { return static_members::get_field("gcEpoch")->get(); }
    };

    auto find_method(vmhook::hotspot::klass* klass, std::string_view name,
                     std::string_view signature) -> vmhook::hotspot::method*
    {
        if (!klass) { return nullptr; }
        for (int index = 0; index < klass->get_methods_count(); ++index)
        {
            auto* method = klass->get_methods_ptr()[index];
            if (method && method->get_name() == name && method->get_signature() == signature)
            {
                return method;
            }
        }
        return nullptr;
    }

    struct report
    {
        std::ofstream output;
        bool valid{true};
        explicit report(const char* path) : output{path} { output.setf(std::ios::unitbuf); }
        auto check(std::string_view name, bool condition) -> bool
        {
            output << std::format("{}={}\n", name, (condition ? "pass" : "fail"));
            valid = valid && condition;
            return condition;
        }
    };

    auto mark(const std::string& path) -> void { std::ofstream{path} << '1'; }
    auto await(const std::string& path) -> bool
    {
        for (unsigned attempt = 0; attempt < 3000; ++attempt)
        {
            if (::GetFileAttributesA(path.c_str()) != static_cast<DWORD>(-1)) { return true; }
            ::Sleep(20);
        }
        return false;
    }

    template<typename predicate>
    auto eventually(predicate condition) -> bool
    {
        for (unsigned attempt = 0; attempt < 1500; ++attempt)
        {
            if (condition()) { return true; }
            ::Sleep(20);
        }
        return false;
    }

    std::atomic<std::uint64_t> method_calls{};
    std::array<std::atomic<unsigned>, 5> typed_calls{};
    std::atomic_bool typed_arguments_valid{true};

    struct jit_snapshot
    {
        vmhook::hotspot::method* target{};
        vmhook::hotspot::method* caller{};
        vmhook::hotspot::method* unrelated{};
        vmhook::hotspot::method* future{};
        std::array<vmhook::hotspot::method*, 5> typed{};
        bool target_compiled{};
        bool caller_compiled{};
        bool unrelated_compiled{};
        bool future_compiled{};
        bool inlined{};
        bool excluded{};
        bool use_compiler{};
        bool typed_compiled{};
        std::array<bool, 5> typed_excluded{};
    };

    auto snapshot(void* context) noexcept -> void
    {
        auto& state = *static_cast<jit_snapshot*>(context);
        try
        {
            state.target_compiled = state.target->get_code() != nullptr;
            state.caller_compiled = state.caller->get_code() != nullptr;
            state.unrelated_compiled = state.unrelated->get_code() != nullptr;
            state.future_compiled = state.future->get_code() != nullptr;
            state.inlined = vmhook::detail::jit_backend::metadata_contains(
                state.caller->get_code(), state.target);
            state.excluded = vmhook::detail::jit_backend::method_is_excluded(state.target);
            state.use_compiler = *static_cast<bool*>(pn::rt().function("use_compiler"));
            state.typed_compiled = true;
            for (std::size_t index = 0; index < state.typed.size(); ++index)
            {
                state.typed_compiled = state.typed_compiled && state.typed[index]->get_code() != nullptr;
                state.typed_excluded[index] = vmhook::detail::jit_backend::method_is_excluded(state.typed[index]);
            }
        }
        catch (...) { state.target_compiled = state.caller_compiled = false; }
    }

    auto take_snapshot(jit_snapshot& state) -> bool
    {
        return vmhook::detail::jit_backend::at_safepoint(&snapshot, &state);
    }

    auto test_jit(report& results) -> void
    {
        vmhook::native_thread_context context{"VMHook-observation-JIT"};
        auto* klass = vmhook::find_class("ObservationJitFixture");
        jit_snapshot state{find_method(klass, "target", "(I)I"),
                           find_method(klass, "caller", "(I)I"),
                           find_method(klass, "unrelated", "(I)I"),
                           find_method(klass, "futureCaller", "(I)I"),
                           {find_method(klass, "targetLong", "(J)J"),
                            find_method(klass, "targetDouble", "(D)D"),
                            find_method(klass, "targetFloat", "(F)F"),
                            find_method(klass, "targetReference", "()Ljava/lang/Object;"),
                            find_method(klass, "targetVoid", "()V")}};
        if (!results.check("metadata", context.valid() && state.target && state.caller
            && state.unrelated && state.future
            && std::all_of(state.typed.begin(), state.typed.end(), [](const auto* method) { return method != nullptr; })
            && vmhook::register_class<jit_fixture>("ObservationJitFixture"))) { return; }
        const bool warmed = eventually([&state]
        {
            return take_snapshot(state) && state.target_compiled && state.caller_compiled
                && state.inlined && state.typed_compiled;
        });
        if (!warmed) { results.check("compiled_callee", false); return; }
        results.check("compiled_callee", state.target_compiled);
        results.check("compiled_caller", state.caller_compiled);
        results.check("inline_metadata", state.inlined);
        results.check("typed_compiled", state.typed_compiled);
        const bool compiler_before = state.use_compiler;
        const bool excluded_before = state.excluded;
        const auto typed_excluded_before = state.typed_excluded;
        if (!results.valid) { return; }
        const bool installed = vmhook::hook<jit_fixture>("target", "(I)I",
            [](vmhook::return_value& result, std::int32_t value)
            {
                method_calls.fetch_add(1, std::memory_order_relaxed);
                result.set<std::int32_t>(value + 100);
            });
        if (!results.check("hook", installed)) { return; }
        bool typed_installed = vmhook::hook<jit_fixture>("targetLong", "(J)J",
            [](vmhook::return_value& result, std::int64_t value)
            {
                if (value != 0x1122334455667788LL) { typed_arguments_valid.store(false); }
                typed_calls[0].fetch_add(1);
                result.set<std::int64_t>(0x2233445566778899LL);
            });
        typed_installed = vmhook::hook<jit_fixture>("targetDouble", "(D)D",
            [](vmhook::return_value& result, double value)
            {
                if (value != 42.25) { typed_arguments_valid.store(false); }
                typed_calls[1].fetch_add(1);
                result.set<double>(142.25);
            }) && typed_installed;
        typed_installed = vmhook::hook<jit_fixture>("targetFloat", "(F)F",
            [](vmhook::return_value& result, float value)
            {
                if (value != 7.5F) { typed_arguments_valid.store(false); }
                typed_calls[2].fetch_add(1);
                result.set<float>(107.5F);
            }) && typed_installed;
        typed_installed = vmhook::hook<jit_fixture>("targetReference", "()Ljava/lang/Object;",
            [](vmhook::return_value& result)
            {
                typed_calls[3].fetch_add(1);
                const auto java_null = std::make_unique<vmhook::any_object>(nullptr);
                result.set(java_null);
            }) && typed_installed;
        typed_installed = vmhook::hook<jit_fixture>("targetVoid", "()V",
            [](vmhook::return_value& result)
            {
                typed_calls[4].fetch_add(1);
                result.cancel();
            }) && typed_installed;
        if (!results.check("typed_hooks", typed_installed)) { vmhook::shutdown_hooks(); return; }
        results.check("callee_deoptimized", take_snapshot(state) && !state.target_compiled && state.excluded);
        results.check("callbacks", eventually([] { return method_calls.load() >= 10000; }));
        results.check("typed_returns", eventually([]
        {
            return typed_arguments_valid.load() && jit_fixture::count("longChanged") >= 100
                && jit_fixture::count("doubleChanged") >= 100 && jit_fixture::count("floatChanged") >= 100
                && jit_fixture::count("referenceChanged") >= 100;
        }));
        const auto void_body = jit_fixture::count("voidBodies");
        ::Sleep(100);
        results.check("void_cancellation", typed_calls[4].load() >= 100 && jit_fixture::count("voidBodies") == void_body);
        const auto before_contention = method_calls.load();
        {
            // Callback delivery must use the published immutable registry even
            // when another native thread owns the administration mutex.
            std::lock_guard lock{vmhook::hotspot::g_hooked_methods_mutex};
            ::Sleep(100);
        }
        results.check("registry_contention", method_calls.load() > before_contention);
        results.check("compiled_caller_observed", jit_fixture::count("callerChanged") > 0);
        results.check("direct_observed", jit_fixture::count("directChanged") > 0);
        jit_fixture::start_future();
        results.check("future_caller_observed", eventually([] { return jit_fixture::count("futureChanged") >= 10000; }));
        const bool compiled_again = eventually([&state]
        {
            return take_snapshot(state) && state.caller_compiled && state.unrelated_compiled && state.future_compiled;
        });
        results.check("callee_stays_interpreted", !state.target_compiled && state.excluded);
        results.check("caller_recompiled", compiled_again && state.caller_compiled);
        results.check("unrelated_recompiled", compiled_again && state.unrelated_compiled);
        results.check("future_caller_compiled", compiled_again && state.future_compiled);
        results.check("compiler_restored", state.use_compiler == compiler_before);
        const auto normal_before = jit_fixture::count("normal");
        vmhook::shutdown_hooks();
        results.check("shutdown_original_body", eventually([normal_before]
        {
            return jit_fixture::count("normal") > normal_before;
        }));
        // Removal disables future entries; an already-entered callback can
        // finish. These bounded callbacks need no user-controlled drain wait.
        ::Sleep(100);
        const auto calls_after_shutdown = method_calls.load();
        std::array<unsigned, 5> typed_after_shutdown{};
        for (std::size_t index = 0; index < typed_calls.size(); ++index) { typed_after_shutdown[index] = typed_calls[index].load(); }
        ::Sleep(200);
        bool no_callbacks = method_calls.load() == calls_after_shutdown;
        for (std::size_t index = 0; index < typed_calls.size(); ++index)
        {
            no_callbacks = no_callbacks && typed_calls[index].load() == typed_after_shutdown[index];
        }
        results.check("shutdown_no_callbacks", no_callbacks);
        results.check("shutdown_flags_restored", take_snapshot(state) && state.excluded == excluded_before
            && state.typed_excluded == typed_excluded_before
            && state.use_compiler == compiler_before && vmhook::verify_hooks() == 0);
        results.check("jit_complete", results.valid);
    }

    std::array<std::atomic<unsigned>, 9> field_reads{};
    std::array<std::atomic<unsigned>, 9> field_writes{};
    std::atomic<unsigned> field_calls{};
    std::atomic<unsigned> gc_advances{};
    std::atomic_bool field_events_valid{true};

    template<typename value_type>
    auto install_field(std::string_view name, unsigned index, value_type first_read,
                       value_type replaced_write, value_type canceled_write) -> bool
    {
        return vmhook::hook_field<field_fixture, value_type>(name,
            [index, first_read, replaced_write, canceled_write](vmhook::field_access<value_type>& event)
            {
                const auto calls = field_calls.fetch_add(1);
                const auto before = calls < 41 ? field_fixture::gc_epoch() : 0;
                if (event.is_read())
                {
                    if (event.value() != value_type{} || event.is_write())
                    {
                        field_events_valid.store(false);
                    }
                    const auto previous_reads = field_reads[index].fetch_add(1);
                    if (index == 3) { event.prevent(); }
                    else if (previous_reads == 0) { event.set(first_read); }
                }
                else if (event.is_write())
                {
                    field_writes[index].fetch_add(1);
                    if (event.value() == canceled_write) { event.prevent(); }
                    else if constexpr (std::is_same_v<value_type, std::int32_t>
                        || std::is_same_v<value_type, std::int64_t>
                        || std::is_floating_point_v<value_type>)
                    {
                        event.set(event.value() + replaced_write);
                    }
                    else { event.set(replaced_write); }
                }
                else { field_events_valid.store(false); }
                if (calls < 41)
                {
                    // The callback remains in native state while a separate
                    // Java thread collects and moves the static class mirror.
                    ::Sleep(5);
                    if (field_fixture::gc_epoch() != before) { gc_advances.fetch_add(1); }
                }
            });
    }

    auto test_fields(report& results, const std::string& control) -> void
    {
        vmhook::native_thread_context context{"VMHook-observation-fields"};
        auto* klass = vmhook::find_class("ObservationFieldFixture");
        if (!results.check("metadata", context.valid() && klass
            && vmhook::register_class<field_fixture>("ObservationFieldFixture"))) { return; }
        auto* accessor = find_method(klass, "warmedAccessor", "()J");
        if (!results.check("compiled_accessor", accessor && accessor->get_code())) { return; }
        bool installed = install_field<std::int32_t>("intValue", 0, 117, 200, 333);
        installed = install_field<std::int64_t>("longValue", 1, 0x2233445566778899LL, 100, 333) && installed;
        installed = install_field<double>("doubleValue", 2, 142.25, 200.0, 333.5) && installed;
        installed = install_field<std::int32_t>("plainValue", 3, 0, 0, 0) && installed;
        installed = install_field<std::int8_t>("byteValue", 4, -9, -30, 33) && installed;
        installed = install_field<std::int16_t>("shortValue", 5, -309, -3000, 33) && installed;
        installed = install_field<std::uint16_t>("charValue", 6, 'Q', 'Z', 'S') && installed;
        installed = install_field<bool>("boolValue", 7, true, false, false) && installed;
        installed = install_field<float>("floatValue", 8, 142.25F, 200.0F, 333.5F) && installed;
        if (!results.check("field_registration", installed)) { vmhook::shutdown_hooks(); return; }
        const bool duplicate_refused = !vmhook::hook_field<field_fixture, std::int32_t>(
            "intValue", [](vmhook::field_access<std::int32_t>&) {});
        const bool width_refused = !vmhook::hook_field<field_fixture, std::int64_t>(
            "plainValue", [](vmhook::field_access<std::int64_t>&) {});
        const bool reference_refused = !vmhook::hook_field<field_fixture, vmhook::oop_t>(
            "referenceValue", [](vmhook::field_access<vmhook::oop_t>&) {});
        results.check("field_refusals", duplicate_refused && width_refused && reference_refused);
        mark(control);
        if (!await(std::format("{}.done", control))) { results.check("field_callbacks", false); return; }
        results.check("field_callbacks", field_events_valid.load() && field_calls.load() == 20044
            && field_reads[0].load() == 20004 && field_writes[0].load() == 4
            && field_reads[3].load() == 1 && field_writes[3].load() == 0);
        results.check("field_gc", gc_advances.load() != 0);
        auto* future = vmhook::find_class("ObservationFieldFuture");
        auto* future_read = find_method(future, "read", "()I");
        auto* future_write = find_method(future, "write", "(I)V");
        results.check("future_field_callbacks", future_read && future_write
            && !future_read->get_code() && !future_write->get_code()
            && vmhook::detail::jit_backend::method_is_excluded(future_read)
            && vmhook::detail::jit_backend::method_is_excluded(future_write));
        const bool removed = vmhook::unhook_field<field_fixture>("intValue");
        results.check("field_removal", removed && !vmhook::unhook_field<field_fixture>("intValue"));
        vmhook::shutdown_hooks();
        const auto after_shutdown = field_calls.load();
        mark(std::format("{}.restore", control));
        results.check("field_shutdown", await(std::format("{}.restore.done", control))
            && field_calls.load() == after_shutdown && vmhook::verify_hooks() == 0);
        results.check("fields_complete", results.valid);
    }

    struct class_record
    {
        std::uintptr_t klass{};
        std::uintptr_t cld{};
        std::uintptr_t loader{};
        int thread_state{};
        std::array<char, 256> name{};
        std::atomic_bool complete{};
    };

    std::array<class_record, 4096> class_records{};
    std::atomic<std::size_t> class_count{};
    std::atomic<unsigned> persistent_after_remove{};
    std::atomic<unsigned> persistent_after_shutdown{};
    std::atomic_bool class_overflow{};

    auto record_class(const vmhook::class_load_event& event) noexcept -> void
    {
        const auto index = class_count.fetch_add(1);
        if (index >= class_records.size()) { class_overflow.store(true); return; }
        auto& record = class_records[index];
        record.klass = reinterpret_cast<std::uintptr_t>(event.klass);
        record.cld = reinterpret_cast<std::uintptr_t>(event.defining_loader_data);
        record.loader = reinterpret_cast<std::uintptr_t>(event.defining_loader_oop);
        auto* thread = pn::rt().current_thread();
        record.thread_state = thread ? pn::thread_state(thread) : 0;
        const auto size = std::min(event.internal_name.size(), record.name.size() - 1);
        std::memcpy(record.name.data(), event.internal_name.data(), size);
        record.complete.store(true, std::memory_order_release);
    }

    auto test_classes(report& results, const std::string& control) -> void
    {
        const auto observer = vmhook::on_class_loaded(&record_class);
        // Ignoring the numeric ID must keep the registration active.
        vmhook::on_class_loaded([](const vmhook::class_load_event& event)
        {
            if (event.internal_name == "ObservationEventAfterRemove") { persistent_after_remove.fetch_add(1); }
            if (event.internal_name == "ObservationEventAfterShutdown") { persistent_after_shutdown.fetch_add(1); }
        });
        if (!results.check("class_registration", observer != 0)) { return; }
        mark(control);
        if (!await(std::format("{}.done", control))) { results.check("class_identity", false); return; }
        std::array<std::uintptr_t, 6> template_clds{};
        unsigned templates{}, buffers{}, hidden{}, arrays{}, bootstrap{};
        bool valid = !class_overflow.load();
        bool callback_state = true;
        const auto count = class_count.load();
        for (std::size_t index = 0; index < std::min(count, class_records.size()); ++index)
        {
            const auto& record = class_records[index];
            if (!record.complete.load(std::memory_order_acquire)) { valid = false; continue; }
            callback_state = callback_state && record.thread_state == 6 && record.klass && record.cld;
            const std::string_view name{record.name.data()};
            if (name == "ObservationEventTemplate")
            {
                if (templates < template_clds.size()) { template_clds[templates] = record.cld; }
                ++templates;
                valid = valid && record.loader != 0;
            }
            else if (name == "ObservationEventBuffer") { ++buffers; }
            else if (name.starts_with("ObservationEventHidden")) { ++hidden; }
            else if (name == "[LObservationEventTemplate;" || name == "[[LObservationEventTemplate;") { ++arrays; }
            else if (name == "javax/crypto/spec/PBEParameterSpec")
            {
                ++bootstrap;
                valid = valid && record.loader == 0;
            }
        }
        std::sort(template_clds.begin(), template_clds.end());
        results.check("class_identity", valid && templates == 6 && template_clds.front() != 0
            && std::adjacent_find(template_clds.begin(), template_clds.end()) == template_clds.end());
        results.check("class_bytebuffer", buffers == 1);
        results.check("class_hidden_or_anonymous", hidden == 1);
        results.check("class_arrays", arrays == 3);
        results.check("class_bootstrap", bootstrap == 1);
        results.check("class_failed_definition", templates == 6);
        results.check("class_callback_state", callback_state);
        const bool removed = vmhook::remove_class_observer(observer);
        const auto after_remove = class_count.load();
        mark(std::format("{}.remove", control));
        const bool removed_stage = await(std::format("{}.remove.done", control));
        results.check("class_removal", removed && removed_stage && class_count.load() == after_remove
            && !vmhook::remove_class_observer(observer));
        results.check("class_persistent", persistent_after_remove.load() == 1);
        vmhook::shutdown_hooks();
        mark(std::format("{}.shutdown", control));
        results.check("class_shutdown", await(std::format("{}.shutdown.done", control))
            && class_count.load() == after_remove && persistent_after_shutdown.load() == 0);
        results.check("classes_complete", results.valid);
    }

    auto worker(void*) -> DWORD
    {
        char destination[4096]{}, control[4096]{}, mode[32]{};
        if (!::GetEnvironmentVariableA("VMHOOK_OBSERVATION_RESULT", destination, sizeof destination)
            || !::GetEnvironmentVariableA("VMHOOK_OBSERVATION_CONTROL", control, sizeof control)
            || !::GetEnvironmentVariableA("VMHOOK_OBSERVATION_MODE", mode, sizeof mode)) { return 1; }
        report results{destination};
        try
        {
            if (std::string_view{mode} == "jit") { test_jit(results); }
            else if (std::string_view{mode} == "fields") { test_fields(results, control); }
            else if (std::string_view{mode} == "classes") { test_classes(results, control); }
            else { results.check("mode", false); }
        }
        catch (const std::exception& error) { results.output << std::format("exception={}\n", error.what()); return 2; }
        return results.valid ? 0 : 3;
    }
}

extern "C" BOOL __attribute__((ms_abi)) DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == 1)
    {
        ::DisableThreadLibraryCalls(instance);
        if (HANDLE thread = ::CreateThread(nullptr, 0, worker, nullptr, 0, nullptr)) { ::CloseHandle(thread); }
    }
    return true;
}
