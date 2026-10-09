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
    class callback_fixture : public vmhook::object<callback_fixture>
    { public: using vmhook::object<callback_fixture>::object; };
    class callback_node : public vmhook::object<callback_node>
    {
    public:
        using vmhook::object<callback_node>::object;
        auto value() const -> std::int32_t { return getField("value", *this)->get(); }
    };
    std::atomic<unsigned> callback_count{}, nested_count{}, observed_gc{};
    std::atomic<bool> correct{true};
    std::ofstream report;

    auto check(const bool condition) noexcept -> void
    { if (!condition) { correct.store(false, std::memory_order_release); } }

    auto read_gc() -> std::int32_t
    { return callback_fixture::getField("gcEpoch")->get(); }

    auto worker(void*) -> vmhook::windows::DWORD
    {
        char destination[4096]{};
        if (!vmhook::windows::GetEnvironmentVariableA("VMHOOK_CALLBACK_RESULT", destination, sizeof(destination)))
        { return 1; }
        report.open(destination);
        report.setf(std::ios::unitbuf);
        try
        {
            vmhook::native_thread_context context{"VMHook-callback-regression"};
            if (!context) { throw vmhook::exception{context.error()}; }
            if (!vmhook::register_class<callback_fixture>("ObservationCallbackFixture")
                || !vmhook::register_class<callback_node>("ObservationCallbackFixture$Node"))
            { throw vmhook::exception{"Callback fixture classes unavailable"}; }
            const bool nested{vmhook::hook<callback_fixture>("nested", "(LObservationCallbackFixture$Node;)I",
                [](vmhook::return_value&, vmhook::borrowed<callback_node> original)
                {
                    check(vmhook::detail::native_backend::active());
                    check(vmhook::detail::private_native::thread_state(
                        vmhook::hotspot::current_java_thread) == 4);
                    check(original && original->value() > 0);
                    nested_count.fetch_add(1, std::memory_order_release);
                })};
            const bool main{vmhook::hook<callback_fixture>("target",
                "(LObservationCallbackFixture$Node;I)LObservationCallbackFixture$Node;",
                [](vmhook::return_value& result, vmhook::borrowed<callback_node> input, std::int32_t expected)
                {
                    check(vmhook::detail::native_backend::active());
                    check(vmhook::detail::private_native::thread_state(
                        vmhook::hotspot::current_java_thread) == 4);
                    const auto before{read_gc()};
                    vmhook::windows::Sleep(35);
                    if (read_gc() > before) { observed_gc.fetch_add(1, std::memory_order_relaxed); }
                    check(input && input->value() == expected);
                    auto* const parent{vmhook::detail::native_backend::current_context_lifetime()};
                    const auto nested_result{callback_fixture::getMethod("nested", "(LObservationCallbackFixture$Node;)I")->call(input)};
                    check(nested_result.was_invoked() && static_cast<std::int32_t>(nested_result) == expected);
                    check(vmhook::detail::native_backend::current_context_lifetime() == parent);
                    auto allocated{vmhook::make_unique<callback_node>(std::int32_t{expected + 1000})};
                    check(allocated && allocated->get_native_root());
                    vmhook::windows::Sleep(35);
                    check(allocated && allocated->value() == expected + 1000);
                    if (allocated) { result.set(*allocated); }
                    callback_count.fetch_add(1, std::memory_order_release);
                })};
            if (!nested || !main) { throw vmhook::exception{"Callback hooks refused"}; }
            callback_fixture::getField("start")->set(true);
            for (unsigned attempt{}; attempt < 3000 && correct.load(std::memory_order_acquire)
                && static_cast<std::int32_t>(callback_fixture::getField("validReturns")->get()) < 24; ++attempt)
            { vmhook::windows::Sleep(10); }
            const auto returned{static_cast<std::int32_t>(callback_fixture::getField("validReturns")->get())};
            report << "callbacks=" << callback_count.load() << "\nnested=" << nested_count.load()
                   << "\ngc_observed=" << observed_gc.load() << "\nreturns=" << returned << '\n';
            check(callback_count.load() == 24 && nested_count.load() == 24 && observed_gc.load() > 0 && returned == 24);
            report << "callback_proof=" << (correct.load() ? "pass" : "fail") << '\n';
            return correct.load() ? 0 : 2;
        }
        catch (const std::exception& failure)
        { report << "exception=" << failure.what() << "\ncallback_proof=fail\n"; return 3; }
    }
}

extern "C" BOOL __attribute__((ms_abi)) DllMain(HINSTANCE self, DWORD reason, LPVOID)
{
    if (reason == 1)
    {
        ::DisableThreadLibraryCalls(self);
        if (auto thread{::CreateThread(nullptr, 0, &worker, nullptr, 0, nullptr)})
        { vmhook::windows::CloseHandle(thread); }
    }
    return true;
}
