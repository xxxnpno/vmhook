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
        return getMethod("count")->call();
    }

    static auto count(std::int32_t limit) -> std::int32_t
    {
        return getMethod("count", "(I)I")->call(limit);
    }

    static auto total() -> std::int32_t
    {
        return getField("total")->get();
    }

    static auto set_total(std::int32_t value) -> void
    {
        getField("total")->set(value);
    }

    auto kill() const -> void
    {
        getMethod("kill", *this)->call();
    }

    auto damage(std::int32_t amount) const -> void
    {
        getMethod("damage", "(I)V", *this)->call(amount);
    }

    auto health() const -> float
    {
        return getField("health", *this)->get();
    }

    auto set_health(float value) -> void
    {
        getField("health", *this)->set(value);
    }

    auto name() const -> std::string
    {
        return getField("name", *this)->get();
    }

    auto say_hello() const -> void
    {
        getMethod("say", *this)->call("hello");
    }

    auto ride(const vmhook::borrowed<entity>& passenger) const -> void
    {
        getMethod("ride", *this)->call(passenger);
    }

    auto ride(const vmhook::ref<entity>& passenger) const -> void
    {
        getMethod("ride", *this)->call(passenger);
    }

    auto dismount() const -> void
    {
        getMethod("ride", *this)->call(nullptr);
    }

    auto replace_arrays() const -> void
    {
        getField("numbers", *this)->set(std::vector<std::int32_t>{1, 2, 3});
        getField("flags", *this)->set(std::vector<bool>{true, false});
        getField("names", *this)->set(std::vector<std::string>{"first", "second"});
        const auto name_field = getField("name", *this);
        vmhook::set_str_field(*name_field, "new name");
    }

    // A bound instance can read a Java static using the same accessor.
    auto total_from_instance() const -> std::int32_t
    {
        return getField("total", *this)->get();
    }
};

class other : public vmhook::object<other>
{
public:
    using vmhook::object<other>::object;
};

template<class receiver>
concept entity_receiver = requires(const receiver& object)
{
    entity::getMethod("kill", object);
    entity::getMethod("damage", "(I)V", object);
    entity::getField("health", object);
};

static_assert(entity_receiver<entity>);
static_assert(!entity_receiver<other>);
static_assert(!entity_receiver<std::string>);
static_assert(std::is_same_v<decltype(entity::getMethod("count")),
                             vmhook::member_lookup<vmhook::method_proxy>>);
static_assert(std::is_same_v<decltype(entity::getMethod("count", "()I")),
                             vmhook::member_lookup<vmhook::method_proxy>>);
static_assert(std::is_same_v<decltype(entity::getField("total")),
                             vmhook::member_lookup<vmhook::field_proxy>>);
static_assert(std::is_same_v<decltype(entity::getMethod("kill", std::declval<const entity&>())),
                             vmhook::member_lookup<vmhook::method_proxy>>);
static_assert(std::is_same_v<decltype(entity::getField("health", std::declval<const entity&>())),
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
