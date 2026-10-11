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
    class fixture : public vmhook::object<fixture>
    {
    public:
        using object::object;
        static auto collect() -> void { (void)static_members::get_method("collect")->call(); }
        static auto first_player() -> std::unique_ptr<vmhook::any_object>
        { return static_members::get_method("first_player")->call(); }
        static auto wrong_player() -> std::unique_ptr<vmhook::any_object>
        { return static_members::get_method("wrong_player")->call(); }
        static auto wrong_holder() -> std::unique_ptr<vmhook::any_object>
        { return static_members::get_method("wrong_holder")->call(); }
        static auto nested_id() -> std::int32_t { return static_members::get_method("nested_id")->call(); }
    };
    class wrong_player : public vmhook::object<wrong_player> { public: using object::object; };
    class wrong_holder : public vmhook::object<wrong_holder> { public: using object::object; };
    class player : public vmhook::object<player>
    {
    public:
        using object::object;
        auto id() const -> std::int32_t { return get_field("id")->get(); }
    };
    struct allocation_failure_callback final
    {
        unsigned moves{};
        allocation_failure_callback() = default;
        allocation_failure_callback(const allocation_failure_callback&) = delete;
        allocation_failure_callback(allocation_failure_callback&& other) : moves{other.moves + 1}
        { if (moves == 2) { throw std::bad_alloc{}; } }
        auto operator()(vmhook::field_access<std::int32_t>&) const -> void {}
    };
    static_assert(std::is_same_v<decltype(std::declval<const vmhook::field_access<std::unique_ptr<player>>&>().value()),
                                 const std::unique_ptr<player>&>);
    auto worker(void*) -> DWORD
    {
        char destination[4096]{}, control[4096]{};
        if (!::GetEnvironmentVariableA("VMHOOK_TEST_RESULT", destination, sizeof destination)
            || !::GetEnvironmentVariableA("VMHOOK_REFERENCE_CONTROL", control, sizeof control)) { return 1; }
        std::ofstream output{destination};
        try
        {
            vmhook::native_thread_context context{"VMHook-reference-install"};
            if (!context) { throw std::runtime_error{context.error()}; }
            if (!vmhook::register_class<fixture>("ReferenceFieldFixture"))
            { throw std::runtime_error{"reference field owner metadata"}; }
            const auto first = fixture::first_player();
            if (!vmhook::register_class<player>("ReferenceFieldFixture$Player", first))
            { throw std::runtime_error{"reference field value metadata"}; }
            bool allocation_propagated{};
            try { (void)vmhook::hook_field<fixture, std::int32_t>("sink", allocation_failure_callback{}); }
            catch (const std::bad_alloc&) { allocation_propagated = true; }
            if (!allocation_propagated) { throw std::runtime_error{"field hook swallowed allocation failure"}; }
            output << "reference_install_allocation_failure=pass\n"; output.flush();
            const auto second = fixture::wrong_player();
            const auto holder = fixture::wrong_holder();
            if (!vmhook::register_class<wrong_player>("ReferenceFieldFixture$Player", second)
                || !vmhook::register_class<wrong_holder>("ReferenceFieldFixture$Holder", holder))
            { throw std::runtime_error{"second-loader field metadata"}; }
            if (vmhook::hook_field<fixture, std::unique_ptr<wrong_player>>("reference", [](auto&) {})
                || vmhook::hook_field<fixture, std::unique_ptr<vmhook::array<wrong_player>>>("references", [](auto&) {}))
            { throw std::runtime_error{"wrong-loader field hook was accepted"}; }
            output << "reference_loader_refusal=pass\n"; output.flush();
            const bool installed = vmhook::hook_field<fixture, std::unique_ptr<player>>("reference",
                [](vmhook::field_access<std::unique_ptr<player>>& event)
                {
                    if (!event.value()) { throw std::runtime_error{"empty reference field owner"}; }
                    fixture::collect();
                    if (event.is_read())
                    {
                        if (fixture::nested_id() != 202) { throw std::runtime_error{"nested field binding scope"}; }
                        event.set(vmhook::make_unique<player>(std::int32_t{101}));
                    }
                    else if (event.value()->get_instance())
                    {
                        const auto id = event.value()->id();
                        if (id == 33) { event.prevent(); }
                        else if (id == 21) { event.set(vmhook::make_unique<player>(std::int32_t{121})); }
                    }
                    fixture::collect();
                });
            const bool arrays = vmhook::hook_field<fixture, std::unique_ptr<vmhook::array<player>>>("references",
                [](vmhook::field_access<std::unique_ptr<vmhook::array<player>>>& event)
                {
                    fixture::collect();
                    if (event.is_write() && !event.value()->get_instance()) { return; }
                    if (event.is_write() && event.value()->get(0)->id() == 33) { event.prevent(); return; }
                    const auto value = vmhook::make_array<player>(1);
                    value->set(0, vmhook::make_unique<player>(std::int32_t{event.is_read() ? 101 : 121}));
                    event.set(value); fixture::collect();
                });
            const bool prevented = vmhook::hook_field<fixture, std::unique_ptr<player>>("prevented",
                [](vmhook::field_access<std::unique_ptr<player>>& event)
                { if (event.is_read()) { event.prevent(); } });
            if (!installed || !arrays || !prevented) { throw std::runtime_error{"reference field installation"}; }
            if (!vmhook::register_class<player>("ReferenceFieldFixture$Player", second))
            { throw std::runtime_error{"reference value wrapper rebind"}; }
            const bool nested = vmhook::hook_field<wrong_holder, std::unique_ptr<player>>("nested",
                [](vmhook::field_access<std::unique_ptr<player>>& event)
                { if (event.is_read()) { event.set(vmhook::make_unique<player>(std::int32_t{202})); fixture::collect(); } });
            if (!nested) { throw std::runtime_error{"nested second-loader field hook installation"}; }
            {
                vmhook::detail::native_backend::operation access{};
                const auto fresh = vmhook::make_unique<player>(std::int32_t{313});
                if (vmhook::klass_from_oop(fresh->get_instance()) != vmhook::detail::registered_class_klass<wrong_player>())
                { throw std::runtime_error{"callback snapshot changed the global wrapper binding"}; }
            }
            output << "reference_binding_rebind=pass\n"; output.flush();
            output << "reference_install=pass\n"; output.flush();
            std::ofstream{control}.put('1');
            return 0;
        }
        catch (const std::exception& error) { output << std::format("exception={}\n", error.what()); return 2; }
    }
}
extern "C" BOOL __attribute__((ms_abi)) DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == 1)
    {
        ::DisableThreadLibraryCalls(instance);
        HANDLE thread = ::CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
        if (thread) { ::CloseHandle(thread); }
    }
    return true;
}
