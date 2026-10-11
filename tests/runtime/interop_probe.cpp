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
    class interop_fixture : public vmhook::object<interop_fixture>
    {
    public:
        using vmhook::object<interop_fixture>::object;
        static auto method(const std::string_view name) -> vmhook::member_lookup<vmhook::method_proxy>
        { return static_members::get_method(name); }
    };
    class player : public vmhook::object<player>
    {
    public:
        using vmhook::object<player>::object;
        auto id() const -> std::int32_t { return get_field("id")->get(); }
    };
    class wide : public vmhook::object<wide>
    {
    public:
        using vmhook::object<wide>::object;
        auto total() const -> std::int64_t { return get_field("total")->get(); }
    };
    auto require(const bool condition, const std::string_view label) -> void
    { if (!condition) { throw std::runtime_error{std::string{label}}; } }
    auto check(std::ostream& output, const std::string_view label, const bool condition) -> void
    { require(condition, label); output << label << "=pass\n"; output.flush(); }

    template<std::size_t... index>
    auto call_many_references(const std::unique_ptr<player>& value, std::index_sequence<index...>) -> std::int32_t
    { return interop_fixture::method("manyReferences")->call((static_cast<void>(index), value)...); }

    template<typename element_type>
    auto primitive_array(const element_type first, const element_type last) -> bool
    {
        const auto values = vmhook::make_array<element_type>(3);
        require(values && values->get_instance(), "primitive owner");
        values->set(0, first); values->set(2, last);
        interop_fixture::method("collect")->call();
        interop_fixture::method("verifyPrimitive")->call(values,
            std::string{vmhook::detail::primitive_array_descriptor<element_type>}, std::int32_t{3});
        const auto copied = values->to_vector();
        return copied.size() == 3 && copied[0] == first && copied[1] == element_type{} && copied[2] == last;
    }
    auto worker(void*) -> DWORD
    {
        char destination[4096]{};
        if (!::GetEnvironmentVariableA("VMHOOK_TEST_RESULT", destination, sizeof destination)) { return 1; }
        std::ofstream output{destination};
        try
        {
            require(vmhook::register_class<interop_fixture>("InteropFixture")
                && vmhook::register_class<player>("InteropFixture$Player")
                && vmhook::register_class<wide>("InteropFixture$Wide"), "metadata");
            vmhook::native_thread_context context{"VMHook-interop-" + std::to_string(::GetCurrentThreadId())};
            check(output, "context", context.valid());
            const auto first = vmhook::make_unique<player>(std::int32_t{11});
            const auto last = vmhook::make_unique<player>(std::int32_t{22});
            const auto java_null = std::make_unique<player>(nullptr);
            bool maximum_parameters{};
            {
                vmhook::detail::native_backend::operation access{};
                require(static_cast<bool>(access), "maximum parameter VM operation");
                const auto maximum = interop_fixture::method("maximumReferences");
                std::vector<vmhook::method_proxy::java_arg> arguments(255,
                    vmhook::method_proxy::java_arg::of_object(first->get_instance()));
                maximum_parameters = static_cast<std::int32_t>(maximum->call_packed(maximum->raw_method(), arguments)) == 255;
                arguments.push_back(vmhook::method_proxy::java_arg::of_object(first->get_instance()));
                bool overflow_refused{};
                try { overflow_refused = !maximum->call_packed(maximum->raw_method(), arguments).was_invoked(); }
                catch (const vmhook::exception&) { overflow_refused = true; }
                require(overflow_refused, "256 parameter refusal");
            }
            require(maximum_parameters, "255 packed reference arguments");
            require(call_many_references(first, std::make_index_sequence<64>{}) == 64, "64 typed reference arguments");
            check(output, "arity", static_cast<std::int64_t>(interop_fixture::method("mixed")->call(
                std::int32_t{1}, std::int64_t{0x1234567812345678}, 3.25, first,
                std::int32_t{5}, std::int64_t{-9000000000}, -7.5, last, std::string{"ninth"}, std::int32_t{10})) == 1234);
            const auto constructed = vmhook::make_unique_with_descriptor<wide>(
                "(IJDLInteropFixture$Player;IJDLInteropFixture$Player;Ljava/lang/String;I)V", std::int32_t{1}, std::int64_t{0x1234567812345678}, 3.25, first,
                std::int32_t{5}, std::int64_t{-9000000000}, -7.5, last, std::string{"ninth"}, std::int32_t{10});
            check(output, "constructor_arity", constructed->total() == 1234);
            check(output, "primitive_arrays", primitive_array<bool>(true, false)
                && primitive_array<std::int8_t>(-12, 120) && primitive_array<std::uint16_t>(0xd834, 0xffff)
                && primitive_array<std::int16_t>(-1234, 32123) && primitive_array<std::int32_t>(-123456, 7654321)
                && primitive_array<std::int64_t>(-9000000000LL, 0x1234567812345678LL)
                && primitive_array<float>(1.25f, -5.5f) && primitive_array<double>(1.25, -5.5));
            const auto bulk = vmhook::make_array<std::int32_t>(100000);
            std::vector<std::int32_t> contents(100000);
            for (std::size_t index{}; index < contents.size(); ++index) { contents[index] = static_cast<std::int32_t>(index); }
            bulk->copy_from(contents);
            const auto bulk_destination = vmhook::make_array<std::int32_t>(100000);
            bulk_destination->copy_from(bulk, 0, 0, 100000);
            const auto booleans = vmhook::make_array<bool>(3);
            booleans->copy_from(std::vector<bool>{true, false, true});
            check(output, "bulk_copy", bulk->to_vector() == contents && bulk_destination->to_vector() == contents
                && booleans->to_vector() == std::vector<bool>{true, false, true});
            const auto objects = vmhook::make_array<player>(3);
            objects->set(0, first); objects->set(2, last);
            interop_fixture::method("collect")->call();
            const auto elements = objects->to_vector();
            const auto objects_destination = vmhook::make_array<player>(3);
            objects_destination->copy_from(objects, 0, 0, 3);
            interop_fixture::method("collect")->call();
            check(output, "object_arrays", elements.size() == 3 && elements[0]->id() == 11
                && elements[1] && !elements[1]->get_instance() && elements[2]->id() == 22
                && objects_destination->get(0)->id() == 11 && !objects_destination->get(1)->get_instance());
            const std::unique_ptr<vmhook::array<player>> returned = interop_fixture::method("objectArray")->call(first, last);
            check(output, "array_return", returned->get(0)->id() == 11 && returned->get(1) && !returned->get(1)->get_instance());
            const auto rows = vmhook::make_array<vmhook::array<std::int32_t>>(2);
            rows->set(0, bulk);
            const auto player_rows = vmhook::make_array<vmhook::array<player>>(2);
            player_rows->set(0, objects);
            check(output, "multidimensional", rows->get(0)->get(99999) == 99999 && !rows->get(1)->get_instance()
                && player_rows->get(0)->get(0)->id() == 11 && !player_rows->get(1)->get_instance());
            const auto items = vmhook::make_unique<vmhook::array_list>();
            check(output, "list_add", items->add(first) && items->add(java_null) && items->add(last) && items->size() == 3);
            const std::unique_ptr<player> replaced = items->set(0, last);
            const std::unique_ptr<player> removed = items->remove(1);
            items->add(0, first);
            check(output, "list_crud", replaced->id() == 11 && removed && !removed->get_instance()
                && items->index_of(first) == 0 && items->last_index_of(last) == 2 && items->contains(last));
            const std::unique_ptr<vmhook::iterator> cursor = items->iterator();
            const std::unique_ptr<player> head = cursor->next();
            cursor->remove();
            check(output, "iterator", head->id() == 11 && cursor->has_next() && items->size() == 2);
            const auto mapping = vmhook::make_unique<vmhook::hash_map>();
            const std::unique_ptr<player> absent = mapping->put(first, last);
            const std::unique_ptr<player> found = mapping->get(first);
            const std::unique_ptr<player> old = mapping->put(first, java_null);
            const std::unique_ptr<player> null_value = mapping->get(first);
            check(output, "map_crud", absent && !absent->get_instance() && found->id() == 22 && old->id() == 22
                && null_value && !null_value->get_instance() && mapping->contains_key(first) && mapping->contains_value(java_null));
            interop_fixture::method("collect")->call();
            const std::vector<std::pair<std::unique_ptr<player>, std::unique_ptr<player>>> entries = mapping->to_entries();
            check(output, "gc_roots", entries.size() == 1 && entries[0].first->id() == 11
                && entries[0].second && !entries[0].second->get_instance() && first->id() == 11 && last->id() == 22
                && vmhook::make_collection(elements)->size() == 3 && vmhook::make_map(entries)->contains_key(first));
            mapping->clear(); items->clear();
            check(output, "clear", mapping->is_empty() && items->is_empty());
            bool negative_refused{}, bounds_refused{}, owner_refused{};
            try { (void)vmhook::make_array<std::int32_t>(-1); } catch (const vmhook::exception&) { negative_refused = true; }
            try { bulk->set(100000, 1); } catch (const vmhook::exception&) { bounds_refused = true; }
            try { (void)items->add(std::unique_ptr<player>{}); } catch (const vmhook::exception&) { owner_refused = true; }
            check(output, "refusals", negative_refused && bounds_refused && owner_refused);
            const auto text = vmhook::new_string<vmhook::java_string>("hello");
            const auto substring = text->substring(1, 4);
            check(output, "string_wrapper", text->length() == 5 && text->char_at(1) == 'e'
                && text->to_utf16() == u"hello" && substring->to_string() == "ell"
                && text->starts_with("he") && text->ends_with("lo"));
            const std::unique_ptr<vmhook::java_string> surrogate = interop_fixture::method("surrogate")->call();
            check(output, "lossless_utf16", surrogate->to_utf16() == std::u16string{char16_t{0xd800}, u'X'});
            check(output, "boxing", vmhook::java_boolean::value_of(true)->value()
                && vmhook::java_byte::value_of(-12)->value() == -12 && vmhook::java_char::value_of(0xffff)->value() == 0xffff
                && vmhook::java_short::value_of(-1234)->value() == -1234 && vmhook::java_integer::value_of(7654321)->value() == 7654321
                && vmhook::java_long::value_of(-9000000000LL)->value() == -9000000000LL
                && vmhook::java_float::value_of(1.25f)->value() == 1.25f && vmhook::java_double::value_of(-7.5)->value() == -7.5);
            const auto present = vmhook::optional::of_nullable(first);
            const auto missing = vmhook::optional::of_nullable(java_null);
            const std::unique_ptr<player> contained = present->get();
            const std::unique_ptr<player> fallback = missing->or_else(last);
            check(output, "optional", present->is_present() && missing->is_empty() && vmhook::optional::empty()->is_empty()
                && contained->id() == 11 && fallback->id() == 22);
            const auto bootstrap = std::make_unique<vmhook::class_loader>(nullptr);
            const auto string_type = vmhook::class_object::for_name("java.lang.String", bootstrap);
            check(output, "class_wrapper", string_type->name() == "java.lang.String" && !string_type->is_array()
                && !string_type->is_primitive() && !string_type->is_interface() && string_type->loader()
                && !string_type->loader()->get_instance() && string_type->super_class()->name() == "java.lang.Object");
            const std::unique_ptr<vmhook::throwable> error = interop_fixture::method("throwable")->call();
            check(output, "throwable_wrapper", error->message()->to_string() == "interop-error"
                && error->cause() && !error->cause()->get_instance()
                && error->to_string() == "java.lang.IllegalStateException: interop-error");
            const std::array<std::int8_t, 4> bytes{-128, 0, 42, 127};
            for (const auto& buffer : std::array{vmhook::byte_buffer::allocate(4), vmhook::byte_buffer::allocate_direct(4)})
            {
                buffer->put(bytes); buffer->position(0);
                require(buffer->capacity() == 4 && buffer->remaining() == 4 && buffer->to_vector()
                    == std::vector<std::int8_t>(bytes.begin(), bytes.end()) && buffer->position() == 0, "NIO copied bytes");
                buffer->put(1, -42); require(buffer->get(1) == -42, "NIO indexed bytes");
                interop_fixture::method("collect")->call();
                require(buffer->get(2) == 42, "NIO rooted Java ownership");
            }
            check(output, "nio_buffers", true);
            check(output, "interop_complete", vmhook::verify_hooks() == 0);
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
