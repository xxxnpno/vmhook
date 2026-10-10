#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
#else
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vmhook/vmhook.hpp>
#endif

class entity : public vmhook::object<entity>
{
public:
    using vmhook::object<entity>::object;

    static auto count() -> std::int32_t
    {
        return static_members::get_method("count")->call();
    }

    static auto count(std::int32_t limit) -> std::int32_t
    {
        return static_members::get_method("count", "(I)I")->call(limit);
    }

    static auto total() -> std::int32_t
    {
        return static_members::get_field("total")->get();
    }

    static auto set_total(std::int32_t value) -> void
    {
        static_members::get_field("total")->set(value);
    }

    auto kill() const -> void
    {
        get_method("kill")->call();
    }

    auto damage(std::int32_t amount) const -> void
    {
        get_method("damage", "(I)V")->call(amount);
    }

    auto health() const -> float
    {
        return get_field("health")->get();
    }

    auto set_health(float value) -> void
    {
        get_field("health")->set(value);
    }

    auto name() const -> std::string
    {
        return get_field("name")->get();
    }

    auto say_hello() const -> void
    {
        get_method("say")->call("hello");
    }

    auto ride(const vmhook::borrowed<entity>& passenger) const -> void
    {
        get_method("ride")->call(passenger);
    }

    auto ride(const vmhook::ref<entity>& passenger) const -> void
    {
        get_method("ride")->call(passenger);
    }

    auto dismount() const -> void
    {
        get_method("ride")->call(nullptr);
    }

    auto replace_arrays() const -> void
    {
        get_field("numbers")->set(std::vector<std::int32_t>{1, 2, 3});
        get_field("flags")->set(std::vector<bool>{true, false});
        get_field("names")->set(std::vector<std::string>{"first", "second"});
        const auto name_field = get_field("name");
        vmhook::set_str_field(*name_field, "new name");
    }

    // A bound instance can read a Java static using the same accessor.
    auto total_from_instance() const -> std::int32_t
    {
        return get_field("total")->get();
    }
};

class other : public vmhook::object<other>
{
public:
    using vmhook::object<other>::object;
};

template<class wrapper>
concept implicit_receiver_lookup = requires(const wrapper& object)
{
    object.get_method("kill");
    object.get_method("damage", "(I)V");
    object.get_field("health");
};

template<class wrapper>
concept static_instance_method_lookup = requires { wrapper::get_method("kill"); };

template<class wrapper>
concept static_instance_pinned_method_lookup = requires {
    wrapper::get_method("damage", "(I)V");
};

template<class wrapper>
concept static_instance_field_lookup = requires { wrapper::get_field("health"); };

template<class owner, class receiver>
concept method_receiver_argument = requires(const owner& object, const receiver& receiver_object)
{
    object.get_method("kill", receiver_object);
};

template<class owner, class receiver>
concept pinned_method_receiver_argument = requires(const owner& object, const receiver& receiver_object)
{
    object.get_method("damage", "(I)V", receiver_object);
};

template<class owner, class receiver>
concept field_receiver_argument = requires(const owner& object, const receiver& receiver_object)
{
    object.get_field("health", receiver_object);
};

static_assert(implicit_receiver_lookup<entity>);
static_assert(implicit_receiver_lookup<other>);
static_assert(!implicit_receiver_lookup<std::string>);
static_assert(!static_instance_method_lookup<entity>);
static_assert(!static_instance_pinned_method_lookup<entity>);
static_assert(!static_instance_field_lookup<entity>);
static_assert(!method_receiver_argument<entity, entity>);
static_assert(!method_receiver_argument<entity, other>);
static_assert(!pinned_method_receiver_argument<entity, entity>);
static_assert(!pinned_method_receiver_argument<entity, other>);
static_assert(!field_receiver_argument<entity, entity>);
static_assert(!field_receiver_argument<entity, other>);
static_assert(std::is_same_v<decltype(entity::static_members::get_method("count")),
                             vmhook::member_lookup<vmhook::method_proxy>>);
static_assert(std::is_same_v<decltype(entity::static_members::get_method("count", "()I")),
                             vmhook::member_lookup<vmhook::method_proxy>>);
static_assert(std::is_same_v<decltype(entity::static_members::get_field("total")),
                             vmhook::member_lookup<vmhook::field_proxy>>);
static_assert(std::is_same_v<decltype(std::declval<const entity&>().get_method("kill")),
                             vmhook::member_lookup<vmhook::method_proxy>>);
static_assert(std::is_same_v<decltype(std::declval<const entity&>().get_method("damage", "(I)V")),
                             vmhook::member_lookup<vmhook::method_proxy>>);
static_assert(std::is_same_v<decltype(std::declval<const entity&>().get_field("health")),
                             vmhook::member_lookup<vmhook::field_proxy>>);

// This test runs without a JVM. The accessor bodies above are compiled but
// never invoked; the runtime checks exercise only the checked lookup result.
auto other_translation_unit() -> bool;

auto main() -> int
{
    if (!other_translation_unit()) { return 10; }
    vmhook::member_lookup<vmhook::method_proxy> absent{
        std::optional<vmhook::method_proxy>{} };
    if (absent || absent.has_value())
    {
        return 1;
    }
    try
    {
        (void)absent.operator->();
        return 2;
    }
    catch (const vmhook::exception& error)
    {
        if (std::string{ error.what() }.find("lookup failed") == std::string::npos)
        {
            return 3;
        }
    }

    const vmhook::member_lookup<vmhook::field_proxy> absent_field{
        std::optional<vmhook::field_proxy>{} };
    try
    {
        (void)absent_field.value();
        return 4;
    }
    catch (const vmhook::exception&)
    {
    }

    vmhook::member_lookup<vmhook::method_proxy> present{
        std::optional<vmhook::method_proxy>{ std::in_place, nullptr, nullptr, std::string{ "()V" } } };
    if (!present || !present.has_value() || present->signature() != "()V")
    {
        return 5;
    }
    const auto& readonly{ present };
    if (readonly->signature() != "()V" || (*readonly).signature() != "()V")
    {
        return 6;
    }
    return 0;
}
