// Included inside native_probe's fixture namespace after wrapper declarations.
auto check_field_memory(const std::unique_ptr<native_fixture>& first,
                        const std::unique_ptr<native_fixture>& second) -> bool
{
    const auto null_object = std::make_unique<native_fixture>(nullptr);
    const auto check_primitive = [&]<typename value_type>(const char* name, value_type value)
    {
        const auto field = second->get_field(name);
        if (!field || !field->is_volatile() || field->is_final()) { return false; }
        field->set(value);
        return static_cast<value_type>(field->get()) == value;
    };
    if (!check_primitive("memory_boolean", true)
        || !check_primitive("memory_byte", std::int8_t{-91})
        || !check_primitive("memory_short", std::int16_t{-23123})
        || !check_primitive("memory_char", std::uint16_t{0xe123})
        || !check_primitive("memory_int", std::int32_t{-123456789})
        || !check_primitive("memory_long", std::int64_t{-0x123456789abcdefLL})
        || !check_primitive("memory_float", -3.25f)
        || !check_primitive("memory_double", -17.125)) { return false; }

    const auto final_value = second->get_field("immutable_value");
    const auto final_child = second->get_field("immutable_child");
    if (!final_value || !final_child || !final_value->is_final() || !final_child->is_final()
        || !conversion_refused([&] { final_value->set(std::int32_t{19}); })
        || !conversion_refused([&] { final_child->set(null_object); })
        || !conversion_refused([&] { (void)final_value->compare_exchange(std::int32_t{31337}, std::int32_t{19}); })
        || static_cast<std::int32_t>(final_value->get()) != 31337) { return false; }

    const auto object_field = second->get_field("memory_object");
    const auto static_field = native_fixture::static_members::get_field("memory_static_object");
    if (!object_field || !static_field || !object_field->is_volatile() || !static_field->is_volatile())
    { return false; }
    object_field->set(first);
    static_field->set(second);
    native_fixture::collect();
    const std::unique_ptr<native_fixture> observed = object_field->get();
    const std::unique_ptr<native_fixture> static_observed = static_field->get();
    if (!observed || observed->value() != 10 || !static_observed || static_observed->value() != 20
        || !object_field->compare_exchange(first, null_object)
        || object_field->compare_exchange(first, second)
        || !object_field->compare_exchange(null_object, second)) { return false; }
    object_field->set(null_object);
    static_field->set(null_object);
    const std::unique_ptr<native_fixture> observed_null = object_field->get();
    const std::unique_ptr<native_fixture> static_null = static_field->get();
    if (!observed_null || observed_null->get_instance() || !static_null || static_null->get_instance())
    { return false; }

    const auto integer = second->get_field("memory_int");
    const auto wide = second->get_field("memory_long");
    if (!integer->compare_exchange(std::int32_t{-123456789}, std::int32_t{61})
        || integer->compare_exchange(std::int32_t{-123456789}, std::int32_t{62})
        || static_cast<std::int32_t>(integer->get()) != 61
        || !wide->compare_exchange(std::int64_t{-0x123456789abcdefLL}, std::int64_t{0x234567890abcdefLL})
        || wide->compare_exchange(std::int64_t{-0x123456789abcdefLL}, std::int64_t{0})
        || static_cast<std::int64_t>(wide->get()) != 0x234567890abcdefLL) { return false; }

    // Publish a young object into an already-collected holder, discard the C++
    // owner, and collect. The Java field must be its remaining reachability edge.
    for (std::int32_t index{}; index < 12; ++index)
    {
        {
            const auto young = vmhook::make_unique<native_fixture>(std::int32_t{1000 + index});
            second->child_object(young);
        }
        native_fixture::collect();
        const auto retained_by_java = second->child_object();
        if (!retained_by_java || retained_by_java->value() != 1000 + index) { return false; }
    }
    second->child_object(first);
    const std::string replacement{"store\0\xf0\x9f\x98\x80", 10};
    second->get_field("text")->set(replacement);
    native_fixture::collect();
    if (second->text() != replacement) { return false; }
    second->get_field("text")->set(unicode_text());

    const auto command = native_fixture::static_members::get_field("memory_command");
    const auto reply = native_fixture::static_members::get_field("memory_reply");
    const auto payload = native_fixture::static_members::get_field("memory_payload");
    payload->set(std::int32_t{0x12345678});
    command->set(std::int32_t{1});
    for (unsigned attempt{}; attempt < 200; ++attempt)
    {
        if (static_cast<std::int32_t>(command->get()) == 2)
        { return static_cast<std::int32_t>(reply->get()) == 1
            && static_cast<std::int32_t>(payload->get()) == 0x23456789; }
        ::Sleep(10);
    }
    return false;
}
