#pragma once

#if defined(VMHOOK_EXAMPLE_MODULE)
import std;
import vmhook;
import vmhook.windows;
#else
#include <vmhook/vmhook.hpp>
#include <fstream>
#endif

namespace example
{
    class report
    {
        std::ofstream output;

    public:
        explicit report(const char* path) : output{path}
        {
            if (!output) { throw std::runtime_error{"Cannot open the example result file"}; }
            output.setf(std::ios::unitbuf);
        }
        auto check(std::string_view name, bool passed) -> void
        {
            output << name << '=' << (passed ? "pass" : "fail") << '\n';
            if (!passed) { throw std::runtime_error{std::string{name}}; }
        }
        auto value(std::string_view name, std::string_view value) -> void
        { output << name << '=' << value << '\n'; }
        auto number(std::string_view name, std::uint64_t value) -> void
        { output << name << '=' << value << '\n'; }
    };

    class player : public vmhook::object<player>
    {
    public:
        using vmhook::object<player>::object;
        auto id() const -> std::int32_t { return get_field("id")->get(); }
    };

    class fixture : public vmhook::object<fixture>
    {
    public:
        using vmhook::object<fixture>::object;
        auto value() const -> std::int32_t { return get_field("value")->get(); }
        auto add(std::int32_t amount) const -> std::int32_t
        { return get_method("add", "(I)I")->call(amount); }
        static auto get_total() -> std::int32_t
        { return static_members::get_field("total")->get(); }
        static auto thread_name() -> std::string
        { return static_members::get_method("thread_name")->call(); }
        static auto players() -> std::vector<std::unique_ptr<player>>
        { return static_members::get_field("players")->get().to_vector(); }
        static auto echo(const std::unique_ptr<player>& value) -> std::unique_ptr<player>
        { return static_members::get_method("echo")->call(value); }
        static auto plain(std::int32_t value) -> std::int32_t
        { return static_members::get_method("plain", "(I)I")->call(value); }
        static auto read_total() -> std::int32_t
        { return static_members::get_method("read_total")->call(); }
        static auto write_total(std::int32_t value) -> void
        { static_members::get_method("write_total", "(I)V")->call(value); }
        static auto load_later() -> void
        { static_members::get_method("load_later", "()V")->call(); }
    };

    inline auto register_wrappers(report& results) -> void
    {
        results.check("fixture", vmhook::register_class<fixture>("ExampleFixture"));
        results.check("player", vmhook::register_class<player>("ExampleFixture$Player"));
    }
}

auto run_example(example::report&) -> void;

namespace example
{
    inline auto worker(void*) -> DWORD
    {
        char destination[4096]{};
        if (!::GetEnvironmentVariableA("VMHOOK_EXAMPLE_RESULT", destination, sizeof destination)) { return 1; }
        try
        {
            report results{destination};
            try
            {
                run_example(results);
                results.check("complete", true);
                return 0;
            }
            catch (const std::exception& error)
            {
                results.value("exception", error.what());
                return 2;
            }
        }
        catch (...) { return 3; }
    }
}

extern "C" BOOL __attribute__((ms_abi)) DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    constexpr DWORD process_attach{1};
    if (reason == process_attach)
    {
        // Windows starts this worker after DLL initialization releases the
        // loader lock. No Java lookup, context, allocation or hook runs here.
        ::DisableThreadLibraryCalls(instance);
        if (HANDLE thread = ::CreateThread(nullptr, 0, example::worker, nullptr, 0, nullptr))
        { ::CloseHandle(thread); }
    }
    return true;
}
