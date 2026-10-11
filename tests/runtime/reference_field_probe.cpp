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
    class fixture : public vmhook::object<fixture>
    {
    public:
        using object::object;
        static auto collect() -> void { (void)static_members::get_method("collect")->call(); }
    };
    class player : public vmhook::object<player>
    {
    public:
        using object::object;
        auto id() const -> std::int32_t { return get_field("id")->get(); }
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
            if (!vmhook::register_class<fixture>("ReferenceFieldFixture")
                || !vmhook::register_class<player>("ReferenceFieldFixture$Player"))
            { throw std::runtime_error{"reference field metadata"}; }
            const bool installed = vmhook::hook_field<fixture, std::unique_ptr<player>>("reference",
                [](vmhook::field_access<std::unique_ptr<player>>& event)
                {
                    if (!event.value()) { throw std::runtime_error{"empty reference field owner"}; }
                    fixture::collect();
                    if (event.is_read()) { event.set(vmhook::make_unique<player>(std::int32_t{101})); }
                    else if (event.value()->get_instance())
                    {
                        const auto id = event.value()->id();
                        if (id == 33) { event.prevent(); }
                        else if (id == 21) { event.set(vmhook::make_unique<player>(std::int32_t{121})); }
                    }
                    fixture::collect();
                });
            const bool prevented = vmhook::hook_field<fixture, std::unique_ptr<player>>("prevented",
                [](vmhook::field_access<std::unique_ptr<player>>& event)
                { if (event.is_read()) { event.prevent(); } });
            if (!installed || !prevented) { throw std::runtime_error{"reference field installation"}; }
            output << "reference_install=pass\n"; output.flush();
            std::ofstream{control}.put('1');
            return 0;
        }
        catch (const std::exception& error) { output << "exception=" << error.what() << '\n'; return 2; }
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
