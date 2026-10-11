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
    class native_fixture : public vmhook::object<native_fixture>
    {
    public:
        using vmhook::object<native_fixture>::object;
        static auto thread_name() -> std::string
        { return static_members::get_method("threadName", "()Ljava/lang/String;")->call(); }
        static auto plain(std::int32_t input) -> vmhook::method_proxy::value_t
        { return static_members::get_method("plain", "(I)I")->call(input); }
        auto add(std::int32_t amount) const -> std::int32_t
        { return get_method("add", "(I)I")->call(amount); }
        auto value() const -> std::int32_t { return get_field("value")->get(); }
        auto text() const -> std::string { return get_field("text")->get(); }
        static auto collect() -> vmhook::method_proxy::value_t
        { return static_members::get_method("collect", "()V")->call(); }
    };

    class lazy_fixture : public vmhook::object<lazy_fixture>
    {
    public:
        using vmhook::object<lazy_fixture>::object;
        static auto initialized_on() -> std::string
        { return static_members::get_method("initializedOn", "()Ljava/lang/String;")->call(); }
    };

    class collection_player : public vmhook::object<collection_player>
    {
    public:
        using vmhook::object<collection_player>::object;
        auto id() const -> std::int32_t { return get_field("id")->get(); }
    };

    class collection_player_view : public vmhook::object<collection_player_view>
    {
    public:
        using vmhook::object<collection_player_view>::object;
        auto id() const -> std::int32_t { return get_method("id", "()I")->call(); }
    };

    class collection_key : public vmhook::object<collection_key>
    {
    public:
        using vmhook::object<collection_key>::object;
        auto id() const -> std::int32_t { return get_field("id")->get(); }
    };

    class unregistered_player : public vmhook::object<unregistered_player>
    {
    public:
        using vmhook::object<unregistered_player>::object;
    };

    using players = std::vector<std::unique_ptr<collection_player>>;
    using player_entries = std::vector<std::pair<std::unique_ptr<collection_key>,
                                                std::unique_ptr<collection_player>>>;
    using collection_conversion = decltype(std::declval<vmhook::field_proxy::value_t>().to_vector());

    class collection_cases : public vmhook::object<collection_cases>
    {
    public:
        using vmhook::object<collection_cases>::object;
        auto inferred_players() const -> players
        { return get_field("arrayList")->get().to_vector(); }
        auto inferred_entries() const -> player_entries
        { return get_field("linkedHashMap")->get().to_vector(); }
        auto inferred_entries_named() const -> player_entries
        { return get_field("linkedHashMap")->get().to_entries(); }
    };

    vmhook::root<native_fixture> static_holder{"NativeFixture", "holder"};

    auto unicode_text() -> const std::string&
    {
        static const std::string text{"A\0\xf0\x9f\x98\x80\xc3\xa9", 8};
        return text;
    }

    auto thread_name() -> std::string
    { return "VMHook-native-" + std::to_string(::GetCurrentThreadId()); }

    auto passed(std::ostream& output, const std::string_view label, const bool correct) -> bool
    {
        output << label << '=' << (correct ? "pass" : "fail") << '\n';
        output.flush();
        return correct;
    }

    template<typename function_type>
    auto conversion_refused(function_type&& function) -> bool
    {
        try { function(); }
        catch (const vmhook::exception&) { return true; }
        return false;
    }

    template<typename player_type>
    auto player_ids(const std::vector<std::unique_ptr<player_type>>& values)
        -> std::vector<std::int32_t>
    {
        std::vector<std::int32_t> result;
        result.reserve(values.size());
        for (const auto& value : values)
        {
            if (value && !value->get_instance()) { return {}; }
            result.push_back(value ? value->id() : -1);
        }
        return result;
    }

    auto entry_ids(const player_entries& values)
        -> std::vector<std::pair<std::int32_t, std::int32_t>>
    {
        std::vector<std::pair<std::int32_t, std::int32_t>> result;
        result.reserve(values.size());
        for (const auto& [key, value] : values)
        {
            if ((key && !key->get_instance()) || (value && !value->get_instance())) { return {}; }
            result.emplace_back(key ? key->id() : -1, value ? value->id() : -1);
        }
        return result;
    }

    template<typename wrapper_type>
    auto tagged_players(const collection_cases& cases, const std::string_view name) -> players
    {
        const std::unique_ptr<wrapper_type> wrapper = cases.get_field(name)->get();
        if (!wrapper) { throw vmhook::exception{"Missing collection fixture"}; }
        return wrapper->template to_vector<collection_player>();
    }

    template<typename wrapper_type>
    auto tagged_entries(const collection_cases& cases, const std::string_view name) -> player_entries
    {
        const std::unique_ptr<wrapper_type> wrapper = cases.get_field(name)->get();
        if (!wrapper) { throw vmhook::exception{"Missing map fixture"}; }
        return wrapper->template to_vector<collection_key, collection_player>();
    }

    auto check_collections(std::ostream& output,
                           std::optional<collection_conversion>& escaped_conversion,
                           players& escaped_players) -> bool
    {
        const std::unique_ptr<collection_cases> cases =
            native_fixture::static_members::get_field("collectionCases")->get();
        if (!cases || !cases->get_instance()) { return false; }
        const auto expect = [&](const std::string_view name, const bool correct)
        {
            if (!correct) { output << "collection_case=" << name << '\n'; output.flush(); }
            return correct;
        };
        const std::vector<std::int32_t> array_order{1, -1, 2, 3};
        const std::vector<std::int32_t> deque_order{3, 1, 2};
        const std::vector<std::pair<std::int32_t, std::int32_t>> map_order{{2, 20}, {-1, 30}, {1, -1}};
        if (!expect("inferred_field", player_ids(cases->inferred_players()) == array_order
            && entry_ids(cases->inferred_entries()) == map_order
            && entry_ids(cases->inferred_entries_named()) == map_order)) { return false; }
        const std::unique_ptr<vmhook::array_list> inferred_list = cases->get_field("arrayList")->get();
        const players inferred_wrapper = inferred_list->to_vector();
        if (!expect("inferred_collection_wrapper", player_ids(inferred_wrapper) == array_order)) { return false; }
        if (!expect("array_list", player_ids(tagged_players<vmhook::array_list>(*cases, "arrayList")) == array_order)
            || !expect("linked_list", player_ids(tagged_players<vmhook::linked_list>(*cases, "linkedList"))
                == std::vector<std::int32_t>{3, 1, -1})
            || !expect("linked_hash_set", player_ids(tagged_players<vmhook::linked_hash_set>(*cases, "linkedHashSet"))
                == std::vector<std::int32_t>{2, 1, -1})
            || !expect("tree_set", player_ids(tagged_players<vmhook::tree_set>(*cases, "treeSet"))
                == std::vector<std::int32_t>{1, 2, 3})
            || !expect("array_deque", player_ids(tagged_players<vmhook::array_deque>(*cases, "arrayDeque")) == deque_order)
            || !expect("queue", player_ids(tagged_players<vmhook::queue>(*cases, "arrayDeque")) == deque_order)
            || !expect("deque", player_ids(tagged_players<vmhook::deque>(*cases, "arrayDeque")) == deque_order))
        { return false; }
        auto set_ids = player_ids(tagged_players<vmhook::hash_set>(*cases, "hashSet"));
        std::ranges::sort(set_ids);
        if (!expect("hash_set", set_ids == std::vector<std::int32_t>{-1, 1, 2})) { return false; }

        for (const auto name : {"unmodifiableCollection", "unmodifiableList"})
        {
            const players values = cases->get_field(name)->get().to_vector();
            if (!expect(name, player_ids(values) == array_order)) { return false; }
        }
        const players custom = cases->get_field("customCollection")->get().to_vector<collection_player>();
        const players array = cases->get_field("objectArray")->get().to_vector();
        const players array_as_object = cases->get_field("objectArrayAsObject")->get()
            .to_vector<collection_player>();
        const players returned_array_as_object = native_fixture::static_members::get_method(
            "temporaryArrayAsObject", "()Ljava/lang/Object;")->call().to_vector<collection_player>();
        const std::vector<std::unique_ptr<collection_player_view>> interfaces =
            cases->get_field("arrayList")->get().to_vector();
        if (!expect("custom_collection", player_ids(custom) == std::vector<std::int32_t>{2, -1, 1}
                && static_cast<std::int32_t>(cases->get_field("customArrayCalls")->get()) > 0)
            || !expect("object_array", player_ids(array) == std::vector<std::int32_t>{3, -1, 1})
            || !expect("array_object_descriptor", player_ids(array_as_object) == std::vector<std::int32_t>{3, -1, 1}
                && player_ids(returned_array_as_object) == std::vector<std::int32_t>{83, -1, 84})
            || !expect("interface_and_subclass", player_ids(interfaces) == array_order)) { return false; }

        const std::unique_ptr<vmhook::linked_hash_map> ordered = cases->get_field("linkedHashMap")->get();
        const player_entries inferred_map = ordered->to_vector();
        const player_entries inferred_named = ordered->to_entries();
        const player_entries explicit_named = ordered->to_entries<collection_key, collection_player>();
        const player_entries explicit_field = cases->get_field("linkedHashMap")->get()
            .to_vector<collection_key, collection_player>();
        const player_entries explicit_field_named = cases->get_field("linkedHashMap")->get()
            .to_entries<collection_key, collection_player>();
        if (!expect("linked_hash_map", entry_ids(tagged_entries<vmhook::linked_hash_map>(*cases, "linkedHashMap")) == map_order
            && entry_ids(inferred_map) == map_order && entry_ids(inferred_named) == map_order
            && entry_ids(explicit_named) == map_order && entry_ids(explicit_field) == map_order
            && entry_ids(explicit_field_named) == map_order)
            || !expect("tree_map", entry_ids(tagged_entries<vmhook::tree_map>(*cases, "treeMap"))
                == std::vector<std::pair<std::int32_t, std::int32_t>>{{1, -1}, {2, 20}})) { return false; }
        auto hash_ids = entry_ids(tagged_entries<vmhook::hash_map>(*cases, "hashMap"));
        auto concurrent_ids = entry_ids(tagged_entries<vmhook::concurrent_hash_map>(*cases, "concurrentHashMap"));
        auto identity_ids = entry_ids(tagged_entries<vmhook::map>(*cases, "identityHashMap"));
        std::ranges::sort(hash_ids);
        std::ranges::sort(concurrent_ids);
        std::ranges::sort(identity_ids);
        if (!expect("hash_map", hash_ids == std::vector<std::pair<std::int32_t, std::int32_t>>{{-1, 30}, {1, -1}, {2, 20}})
            || !expect("concurrent_hash_map", concurrent_ids
                == std::vector<std::pair<std::int32_t, std::int32_t>>{{8, 80}, {9, 90}})
            || !expect("identity_hash_map", identity_ids
                == std::vector<std::pair<std::int32_t, std::int32_t>>{{7, 70}, {7, 71}})
            || !expect("unmodifiable_map", entry_ids(tagged_entries<vmhook::map>(*cases, "unmodifiableMap")) == map_order))
        { return false; }

        const players null_values = cases->get_field("nullCollection")->get().to_vector();
        const players empty_values = cases->get_field("emptyCollection")->get().to_vector();
        const player_entries null_entries = cases->get_field("nullMap")->get().to_entries();
        const player_entries empty_entries = cases->get_field("emptyMap")->get().to_vector();
        const players null_return = native_fixture::static_members::get_method("nullCollection",
            "()Ljava/util/Collection;")->call().to_vector();
        if (!expect("null_and_empty", null_values.empty() && empty_values.empty()
            && null_entries.empty() && empty_entries.empty() && null_return.empty())) { return false; }

        for (const auto name : {"primitive", "primitiveArray", "nestedArray", "plainObject", "mixedCollection", "linkedHashMap"})
        {
            if (!expect(name, conversion_refused([&]
                { const players values = cases->get_field(name)->get().to_vector(); (void)values; }))) { return false; }
        }
        for (const auto name : {"wrongKeyMap", "wrongValueMap", "arrayList"})
        {
            if (!expect(name, conversion_refused([&]
                { const player_entries values = cases->get_field(name)->get().to_entries(); (void)values; }))) { return false; }
        }
        if (!expect("unregistered_wrapper", conversion_refused([&]
                { (void)cases->get_field("arrayList")->get().to_vector<unregistered_player>(); }))
            || !expect("registered_wrong_wrapper", conversion_refused([&]
                { (void)cases->get_field("arrayList")->get().to_vector<collection_key>(); }))
            || !expect("void_result", conversion_refused([]
                { const players values = native_fixture::collect().to_vector(); (void)values; }))
            || !expect("primitive_result", conversion_refused([]
                { const players values = native_fixture::plain(40).to_vector(); (void)values; }))
            || !expect("java_to_array_exception", conversion_refused([&]
                { const players values = cases->get_field("throwingCollection")->get().to_vector(); (void)values; })))
        { return false; }
        const auto after_exception = native_fixture::plain(40);
        if (!expect("exception_consumed", after_exception.was_invoked() && !after_exception.threw()
            && static_cast<std::int32_t>(after_exception) == 41)) { return false; }

        auto deferred = native_fixture::static_members::get_method("temporaryCollection",
            "()Ljava/util/Collection;")->call().to_vector();
        auto deferred_map = native_fixture::static_members::get_method("temporaryMap",
            "()Ljava/util/Map;")->call().to_entries();
        native_fixture::collect();
        escaped_players = deferred;
        const player_entries temporary_entries = deferred_map;
        native_fixture::collect();
        if (!expect("deferred_gc_roots", player_ids(escaped_players) == std::vector<std::int32_t>{81, -1, 82}
            && entry_ids(temporary_entries) == std::vector<std::pair<std::int32_t, std::int32_t>>{{91, 910}, {-1, -1}}))
        { return false; }
        std::atomic<bool> foreign_safe{};
        std::thread foreign([&]
        {
            vmhook::native_thread_context other{thread_name()};
            foreign_safe = other.valid()
                && conversion_refused([&] { const players values = deferred; (void)values; })
                && conversion_refused([&] { const player_entries values = deferred_map; (void)values; });
        });
        foreign.join();
        if (!expect("foreign_adapter", foreign_safe.load())) { return false; }
        escaped_conversion.emplace(cases->get_field("customCollection")->get().to_vector());
        return true;
    }

    auto base_method(const native_fixture& receiver, const std::string_view name,
                     const std::string_view declaring_class = "NativeBase")
        -> std::optional<vmhook::method_proxy>
    {
        vmhook::detail::native_backend::operation operation{};
        auto* const klass = operation ? vmhook::find_class(declaring_class) : nullptr;
        if (!klass) { return {}; }
        auto** const methods = klass->get_methods_ptr();
        const auto count = klass->get_methods_count();
        if (!methods || count <= 0 || count > 65535) { return {}; }
        for (std::int32_t index = 0; index < count; ++index)
        {
            auto* const method = methods[index];
            if (method && method->get_name() == name && method->get_signature() == "()I")
            {
                return vmhook::method_proxy{receiver.get_instance(), method, "()I", true,
                    nullptr, receiver.get_native_root()};
            }
        }
        return {};
    }

    auto root_frontier() -> std::pair<std::uintptr_t, std::size_t>
    {
        vmhook::detail::native_backend::operation operation{};
        const auto context = vmhook::detail::native_backend::active_context_lifetime();
        if (!operation || !context) { return {}; }
        const auto& runtime = vmhook::detail::private_native::rt();
        void* const area = vmhook::detail::private_native::load<void*>(context->thread,
            runtime.off("Thread", "_handle_area"));
        return {vmhook::detail::private_native::load<std::uintptr_t>(area, runtime.off("Arena", "_hwm")),
                runtime.has_field("Arena", "_size_in_bytes")
                    ? vmhook::detail::private_native::load<std::size_t>(area, runtime.off("Arena", "_size_in_bytes"))
                    : std::size_t{}};
    }

    auto check_root_growth() -> bool
    {
        const auto echo = native_fixture::static_members::get_method("echo", "(Ljava/lang/String;)Ljava/lang/String;");
        const auto invoke = [&]
        {
            const auto value = echo->call(unicode_text());
            return value.was_invoked() && !value.threw() && value.as_string() == unicode_text();
        };
        for (int iteration = 0; iteration < 32; ++iteration) { if (!invoke()) { return false; } }
        const auto before = root_frontier();
        for (int iteration = 0; iteration < 512; ++iteration) { if (!invoke()) { return false; } }
        const auto after = root_frontier();
        return before.first != 0 && before == after;
    }

    auto check_receiver_binding(const native_fixture& first,
                                const native_fixture& second) -> bool
    {
        // Keep both instances' proxies alive together. The second lookup must
        // never redirect the first field or method to a different Java object.
        const auto first_field = first.get_field("value");
        const auto first_method = first.get_method("add");
        const auto second_field = second.get_field("value");
        const auto second_method = second.get_method("add", "(I)I");
        const auto static_from_instance = first.get_method("plain", "(I)I");
        return first_field && first_method && second_field && second_method
            && static_from_instance
            && static_cast<std::int32_t>(first_method->call(std::int32_t{})) == 10
            && static_cast<std::int32_t>(second_method->call(std::int32_t{})) == 20
            && static_cast<std::int32_t>(first_field->get()) == 10
            && static_cast<std::int32_t>(second_field->get()) == 20
            && static_cast<std::int32_t>(static_from_instance->call(std::int32_t{40})) == 41
            && !native_fixture::static_members::get_field("value")
            && !native_fixture::static_members::get_method("add")
            && !native_fixture::static_members::get_method("add", "(I)I")
            && !first.get_field("missing_field")
            && !first.get_method("missing_method");
    }

    auto check_context(std::ostream& output) -> bool
    {
        std::unique_ptr<native_fixture> escaped;
        vmhook::borrowed<native_fixture> escaped_borrow;
        vmhook::ref<native_fixture> escaped_ref;
        std::optional<vmhook::field_proxy> escaped_field;
        std::optional<vmhook::method_proxy> escaped_method;
        vmhook::method_proxy::value_t escaped_value;
        std::optional<collection_conversion> escaped_collection;
        players escaped_players;
        {
            const auto physical_thread = ::GetCurrentThreadId();
            const std::string name{thread_name()};
            vmhook::native_thread_context context{name};
            if (!passed(output, "native_attach", context.valid()))
            {
                output << "attach_error=" << context.error() << '\n';
                return false;
            }
            if (!passed(output, "same_thread", native_fixture::thread_name() == name
                && ::GetCurrentThreadId() == physical_thread)) { return false; }
            if (!passed(output, "class_initialization", lazy_fixture::initialized_on() == name))
            { return false; }

            auto first = vmhook::make_unique<native_fixture>(std::int32_t{10}, unicode_text(), nullptr);
            auto second = vmhook::make_unique<native_fixture>(std::int32_t{20}, unicode_text(), first);
            if (!passed(output, "construction", first && first->get_instance() && second
                && second->get_instance() && first->value() == 10 && second->value() == 20
                && second->text() == unicode_text()
                && check_receiver_binding(*first, *second))) { return false; }
            auto missing_constructor = vmhook::make_unique<native_fixture>(true);
            if (!passed(output, "constructor_refusal", missing_constructor
                && !missing_constructor->get_instance())) { return false; }

            const auto virtual_call = base_method(*first, "virtualValue");
            const auto private_call = base_method(*first, "privateValue");
            const auto static_call = base_method(*first, "staticValue");
            const auto default_call = first->get_method("defaultValue", "()I");
            const auto inherited_default = base_method(*first, "defaultValue", "NativeDefault");
            if (!passed(output, "dispatch_semantics", virtual_call && private_call && static_call
                && inherited_default
                && static_cast<std::int32_t>(virtual_call->call()) == 99
                && static_cast<std::int32_t>(private_call->call()) == 17
                && static_cast<std::int32_t>(static_call->call()) == 17
                && static_cast<std::int32_t>(default_call->call()) == 85
                && static_cast<std::int32_t>(inherited_default->call()) == 85)) { return false; }

            const auto field = first->get_field("value");
            const auto child = second->get_field("child");
            const auto child_value = child ? child->get() : vmhook::field_proxy::value_t{};
            auto touch = native_fixture::static_members::get_method("touch", "(LNativeFixture;Ljava/lang/String;LNativeFixture;)LNativeFixture;");
            const auto result = touch->call(first, unicode_text(), first);
            const auto echo = native_fixture::static_members::get_method("echo", "(Ljava/lang/String;)Ljava/lang/String;")->call(unicode_text());
            const auto wide = native_fixture::static_members::get_method("wide", "(JDI)J")->call(
                std::int64_t{0x1122334455667788LL}, 42.25, std::int32_t{-7});
            const auto floating = native_fixture::static_members::get_method("floating", "(FD)D")->call(1.25f, 2.5);
            const bool collections = check_collections(output, escaped_collection, escaped_players);
            passed(output, "collections", collections);
            if (!passed(output, "invocation", result.was_invoked() && !result.threw()
                && collections
                && first->add(5) == 16 && wide.was_invoked() && !wide.threw()
                && static_cast<std::int64_t>(wide) == 0x1122334455667788LL
                && floating.was_invoked() && !floating.threw()
                && static_cast<double>(floating) == 3.75)) { return false; }
            if (!passed(output, "strings", echo.was_invoked() && !echo.threw()
                && echo.as_string() == unicode_text() && first->text() == unicode_text()))
            { return false; }
            if (!passed(output, "root_growth", check_root_growth())) { return false; }

            const auto thrown = native_fixture::static_members::get_method("fail", "()V")->call();
            const auto after_exception = native_fixture::plain(40);
            if (!passed(output, "java_exception", thrown.was_invoked() && thrown.threw()
                && thrown.exception_class == "java/lang/IllegalStateException"
                && after_exception.was_invoked() && !after_exception.threw()
                && static_cast<std::int32_t>(after_exception) == 41)) { return false; }

            // No VM guard spans this native sleep. The fixture's independent
            // Java thread allocates and collects while persistent roots survive.
            const auto gc_epoch = native_fixture::static_members::get_field("gcEpoch");
            const auto gc_before = gc_epoch ? static_cast<std::int32_t>(gc_epoch->get()) : -1;
            bool collected{};
            for (int attempt = 0; gc_epoch && attempt < 10 && !collected; ++attempt)
            {
                ::Sleep(300);
                collected = static_cast<std::int32_t>(gc_epoch->get()) > gc_before;
            }
            const std::unique_ptr<native_fixture> converted = result;
            const std::unique_ptr<native_fixture> converted_child = child_value;
            const vmhook::borrowed<native_fixture> borrowed = result;
            const vmhook::ref<native_fixture> reference = result;
            native_fixture copy{*first};
            const auto child_path = borrowed.pin().template field<native_fixture>("child");
            const auto holder = static_holder.get();
            bool stable_identity = true;
            for (int attempt = 0; attempt < 32 && stable_identity; ++attempt)
            {
                stable_identity = reference.class_name() == "NativeFixture"
                    && reference.same_object_as(reference);
            }
            if (!passed(output, "idle_gc", collected && field && static_cast<std::int32_t>(field->get()) == 16
                && converted && converted->get_instance() && converted->value() == 16
                && converted_child && converted_child->get_instance() && converted_child->value() == 16
                && borrowed.resolve() && reference.resolve() && child_path.resolve()
                && copy.value() == 16 && holder.resolve() && stable_identity)) { return false; }

            std::atomic<bool> foreign_safe{};
            std::thread foreign([&]
            {
                vmhook::native_thread_context other{thread_name()};
                const auto method = first->get_method("add", "(I)I");
                const bool refused = !method || !method->call(std::int32_t{1}).was_invoked();
                foreign_safe = other.valid() && first->get_instance() == nullptr
                    && borrowed.resolve() == nullptr && reference.resolve() == nullptr
                    && reference.template field<native_fixture>("holder").resolve() == nullptr
                    && !reference->get_field("holder")
                    && virtual_call->raw_method() == nullptr
                    && virtual_call->get_compressed_oop() == 0
                    && !static_call->is_static()
                    && refused && (!field || static_cast<std::int32_t>(field->get()) == 0)
                    && static_holder.get().resolve() != nullptr;
            });
            foreign.join();
            if (!passed(output, "wrong_thread", foreign_safe.load())) { return false; }

            // Warm the unmodified method until HotSpot compiles it, then continue
            // calling that same Method without installing a hook or disabling JIT.
            const auto warm = native_fixture::static_members::get_method("plain", "(I)I");
            bool correct = true;
            for (std::int32_t input = 0; input < 30000 && correct; ++input)
            {
                const auto value = warm->call(input);
                correct = value.was_invoked() && !value.threw()
                    && static_cast<std::int32_t>(value) == input + 1;
            }
            bool compiled = false;
            for (int attempt = 0; attempt < 100 && correct && !compiled; ++attempt)
            {
                {
                    vmhook::detail::native_backend::operation operation{};
                    compiled = operation && warm->raw_method()->get_code() != nullptr;
                }
                if (!compiled) { ::Sleep(20); }
            }
            const auto after_compilation = warm->call(std::int32_t{123});
            if (!passed(output, "compiled_invocation", correct && compiled
                && after_compilation.was_invoked() && !after_compilation.threw()
                && static_cast<std::int32_t>(after_compilation) == 124)) { return false; }

            escaped = std::move(first);
            escaped_borrow = borrowed;
            escaped_ref = reference;
            escaped_field = field ? std::optional<vmhook::field_proxy>{*field} : std::nullopt;
            escaped_method = virtual_call;
            escaped_value = result;
            if (!context.close())
            {
                passed(output, "context_closed", false);
                return false;
            }
        }
        vmhook::native_thread_context replacement{thread_name()};
        return passed(output, "context_closed", replacement.valid() && static_holder.get().resolve()
            && escaped && escaped->get_instance() == nullptr
            && escaped_borrow.resolve() == nullptr && escaped_ref.resolve() == nullptr
            && escaped_ref.template field<native_fixture>("holder").resolve() == nullptr
            && !escaped_ref->get_field("holder")
            && escaped_method && escaped_method->raw_method() == nullptr
            && escaped_method->get_compressed_oop() == 0
            && escaped_value.template to_borrowed<native_fixture>().resolve() == nullptr
            && escaped_collection && conversion_refused([&]
                { const players values = *escaped_collection; (void)values; })
            && escaped_players.size() == 3 && escaped_players[0] && !escaped_players[1]
            && escaped_players[2] && escaped_players[0]->get_instance() == nullptr
            && escaped_players[2]->get_instance() == nullptr
            && (!escaped_field || static_cast<std::int32_t>(escaped_field->get()) == 0));
    }

    auto check_repeated_attachment() -> bool
    {
        for (int iteration = 0; iteration < 8; ++iteration)
        {
            vmhook::native_thread_context context{thread_name()};
            if (!context.valid() || native_fixture::thread_name() != thread_name()) { return false; }
            auto created = vmhook::make_unique<native_fixture>(std::int32_t{iteration}, unicode_text(), nullptr);
            if (!created || !created->get_instance() || created->add(1) != iteration + 1
                || !static_holder.get().resolve()) { return false; }
        }
        return true;
    }

    auto check_concurrent_attachment() -> bool
    {
        std::atomic<bool> correct{true};
        std::array<std::thread, 4> threads;
        for (auto& thread : threads)
        {
            thread = std::thread([&]
            {
                for (int iteration = 0; iteration < 3; ++iteration)
                {
                    vmhook::native_thread_context context{thread_name()};
                    auto created = context.valid()
                        ? vmhook::make_unique<native_fixture>(std::int32_t{iteration}, unicode_text(), nullptr)
                        : nullptr;
                    if (!context.valid() || native_fixture::thread_name() != thread_name()
                        || !created || !created->get_instance() || created->add(1) != iteration + 1)
                    { correct.store(false); return; }
                    ::Sleep(40);
                    if (created->value() != iteration + 1) { correct.store(false); return; }
                }
            });
        }
        for (auto& thread : threads) { thread.join(); }
        return correct.load();
    }

    auto worker(void*) -> DWORD
    {
        char destination[4096]{};
        if (!::GetEnvironmentVariableA("VMHOOK_TEST_RESULT", destination, sizeof destination)) { return 1; }
        std::ofstream output{destination};
        try
        {
            const bool metadata = vmhook::register_class<native_fixture>("NativeFixture")
                && vmhook::register_class<lazy_fixture>("NativeFixture$Lazy")
                && vmhook::register_class<collection_cases>("NativeFixture$CollectionCases")
                && vmhook::register_class<collection_player>("NativeFixture$Player")
                && vmhook::register_class<collection_player_view>("NativeFixture$PlayerView")
                && vmhook::register_class<collection_key>("NativeFixture$Key");
            if (!passed(output, "metadata", metadata)) { return 2; }
            const auto refused = native_fixture::plain(1);
            if (!passed(output, "native_refusal", !refused.was_invoked())) { return 3; }
            if (!check_context(output)) { return 4; }
            if (!passed(output, "repeated_attach", check_repeated_attachment())) { return 5; }
            if (!passed(output, "concurrent_attach", check_concurrent_attachment())) { return 6; }
            if (!passed(output, "no_hooks", vmhook::verify_hooks() == 0)) { return 7; }
            passed(output, "native_complete", true);
            return 0;
        }
        catch (const std::exception& error)
        {
            output << "exception=" << error.what() << '\n';
            return 8;
        }
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
