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
        static auto method(std::string_view name) -> vmhook::member_lookup<vmhook::method_proxy>
        { return static_members::get_method(name); }
        static auto collect() -> void { (void)method("collect")->call(); }
    };
    class player : public vmhook::object<player> { public: using object::object;
        auto id() const -> std::int32_t { return get_field("id")->get(); } };
    class unknown : public vmhook::object<unknown> { public: using object::object; };
    template<int index> class result : public vmhook::object<result<index>>
    {
    public:
        using vmhook::object<result<index>>::object;
        auto marker() const -> std::int32_t { return this->get_field("marker")->get(); }
    };
    auto require(bool condition, std::string_view message) -> void
    { if (!condition) { throw std::runtime_error{std::string{message}}; } }
    template<int index, typename argument>
    auto marker(const argument& value, std::int32_t expected) -> void
    { const auto created{vmhook::make_unique<result<index>>(value)};
      require(created && created->get_instance() && created->marker() == expected, "inferred constructor overload"); }
    auto worker(void*) -> DWORD
    {
        char destination[4096]{};
        if (!::GetEnvironmentVariableA("VMHOOK_TEST_RESULT", destination, sizeof destination)) { return 1; }
        std::ofstream output{destination};
        const auto check = [&](std::string_view label) { output << std::format("{}=pass\n", label); output.flush(); };
        try
        {
            vmhook::native_thread_context context{"VMHook-descriptors"};
            require(context.valid(), context.error());
            require(vmhook::register_class<fixture>("ArgumentDescriptorFixture")
                && vmhook::register_class<player>("ArgumentDescriptorFixture$Player"), "descriptor fixture bindings");
            const std::array names{"StringExact", "StringBroad", "StringObject", "ArrayExact", "ArrayBroad",
                "ArrayObject", "Nested", "CollectionExact", "CollectionBroad", "Invalid", "Ambiguous"};
            const auto register_results = [&]<std::size_t... index>(std::index_sequence<index...>)
            { return (vmhook::register_class<result<static_cast<int>(index)>>(
                std::format("ArgumentDescriptorFixture${}", names[index])) && ...); };
            require(register_results(std::make_index_sequence<11>{}), "constructor class bindings");
            const auto text{vmhook::new_string<vmhook::java_string>("descriptor")};
            const auto text_null{std::make_unique<vmhook::java_string>(nullptr)};
            require(vmhook::detail::jvm_descriptor_for_arg<decltype(text)>() == "Ljava/lang/String;"
                && vmhook::detail::jvm_descriptor_for_arg<std::unique_ptr<vmhook::array<player>>>()
                    == "[LArgumentDescriptorFixture$Player;"
                && vmhook::detail::jvm_descriptor_for_arg<std::unique_ptr<vmhook::array<vmhook::array<std::int32_t>>>>()
                    == "[[I"
                && vmhook::detail::jvm_descriptor_for_arg<std::unique_ptr<vmhook::collection>>()
                    == "Ljava/util/Collection;", "exact inferred descriptors");
            check("exact_descriptors");
            marker<0>(text, 2); marker<1>(text, 3); marker<2>(text, 4);
            marker<0>(text_null, 2); marker<1>(text_null, 3); marker<2>(text_null, 4);
            marker<1>(std::string{"native-string"}, 3);
            check("string_constructor_upcasts");
            const auto players{vmhook::make_array<player>(1)};
            const auto first{vmhook::make_unique<player>(std::int32_t{11})}; players->set(0, first);
            const auto array_null{std::make_unique<vmhook::array<player>>(nullptr)};
            marker<3>(players, 6); marker<4>(players, 5); marker<5>(players, 4);
            marker<3>(array_null, 6); marker<4>(array_null, 5); marker<5>(array_null, 4);
            check("array_constructor_upcasts");
            const auto nested{vmhook::make_array<vmhook::array<std::int32_t>>(1)};
            const auto row{vmhook::make_array<std::int32_t>(1)}; row->set(0, 42); nested->set(0, row);
            marker<6>(nested, 7);
            check("nested_constructor");
            const auto list{vmhook::make_unique<vmhook::array_list>()}; list->add(first);
            const std::unique_ptr<vmhook::collection> collection{fixture::method("get_collection")->call()};
            collection->add(first);
            marker<7>(collection, 8); marker<8>(list, 9);
            check("collection_constructor_upcasts");
            bool wrong{}, ambiguous{}, unknown_refused{};
            try { (void)vmhook::make_unique<result<9>>(text_null); } catch (const vmhook::exception&) { wrong = true; }
            try { (void)vmhook::make_unique<result<10>>(text); } catch (const vmhook::exception&) { ambiguous = true; }
            try { (void)vmhook::detail::jvm_descriptor_for_arg<std::unique_ptr<vmhook::array<unknown>>>(); }
                catch (const vmhook::exception&) { unknown_refused = true; }
            require(wrong && ambiguous && unknown_refused, "incompatible, ambiguous or unknown type accepted");
            require(vmhook::detail::jvm_descriptor_for_arg<std::unique_ptr<vmhook::any_object>>() == "Ljava/lang/Object;"
                && vmhook::detail::jvm_descriptor_for_arg<vmhook::borrowed<void>>() == "Ljava/lang/Object;",
                "opaque Object descriptors");
            {
                vmhook::detail::native_backend::operation access{};
                auto* const holder{vmhook::detail::registered_class_klass<fixture>()};
                require(vmhook::detail::wrapper_descriptor_assignable<vmhook::array<player>>(
                    "[LArgumentDescriptorFixture$Player;", holder, true)
                    && !vmhook::detail::wrapper_descriptor_assignable<vmhook::array<std::int32_t>>(
                        "[LArgumentDescriptorFixture$Player;", holder, true)
                    && vmhook::detail::wrapper_descriptor_assignable<vmhook::collection>(
                        "Ljava/util/ArrayList;", holder, true)
                    && !vmhook::detail::wrapper_descriptor_assignable<vmhook::array_list>(
                        "Ljava/util/Collection;", holder, true), "incoming callback assignability direction");
            }
            check("descriptor_refusals");
            unsigned array_calls{}, nested_calls{}, collection_calls{};
            const auto a = vmhook::add_hook<fixture>("array_target",
                [&](vmhook::return_value& result, const std::unique_ptr<vmhook::array<player>>& values)
                { fixture::collect(); require(static_cast<bool>(values), "empty Java-null callback owner");
                  result.set(std::int32_t{values->get_instance() ? values->get(0)->id() + 100 : -100}); ++array_calls; });
            const auto n = vmhook::add_hook<fixture>("nested_target",
                [&](vmhook::return_value& result, const std::unique_ptr<vmhook::array<vmhook::array<std::int32_t>>>& values)
                { fixture::collect(); result.set(std::int32_t{values->get(0)->get(0) + 100}); ++nested_calls; });
            const auto c = vmhook::add_hook<fixture>("collection_target",
                [&](vmhook::return_value& result, const std::unique_ptr<vmhook::collection>& values)
                { fixture::collect(); result.set(std::int32_t{values->size() + 100}); ++collection_calls; });
            require(a && n && c, "inferred typed hook registration");
            for (unsigned cycle{}; cycle < 20; ++cycle)
            {
                require(static_cast<std::int32_t>(fixture::method("array_target")->call(players)) == 111
                    && static_cast<std::int32_t>(fixture::method("array_target")->call(array_null)) == -100
                    && static_cast<std::int32_t>(fixture::method("nested_target")->call(nested)) == 142
                    && static_cast<std::int32_t>(fixture::method("collection_target")->call(collection)) == 101,
                    "typed method hook arguments after nested GC");
            }
            require(array_calls == 40 && nested_calls == 20 && collection_calls == 20, "callback counts");
            require(vmhook::remove_hook(a) && vmhook::remove_hook(n) && vmhook::remove_hook(c), "hook removal");
            check("inferred_hook_arguments");
            check("descriptor_complete"); return 0;
        }
        catch (const std::exception& error) { output << std::format("exception={}\n", error.what()); output.flush(); return 2; }
    }
}
extern "C" BOOL __attribute__((ms_abi)) DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == 1) { ::DisableThreadLibraryCalls(instance);
        HANDLE thread = ::CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
        if (thread) { ::CloseHandle(thread); } }
    return true;
}
