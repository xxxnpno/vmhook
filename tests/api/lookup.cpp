#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
#else
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
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

    auto ride(const std::unique_ptr<entity>& passenger) const -> void
    {
        get_method("ride")->call(passenger);
    }

    auto dismount() const -> void
    {
        const auto absent = std::make_unique<entity>(nullptr);
        get_method("ride")->call(absent);
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

class player : public vmhook::object<player>
{
public:
    using vmhook::object<player>::object;
};

using player_vector = std::vector<std::unique_ptr<player>>;
using player_entries =
    std::vector<std::pair<std::unique_ptr<entity>, std::unique_ptr<player>>>;

// The wrapper is still named a: its caller writes a::get_total().
// static_members is the inherited class lookup helper inside the body.
class a : public vmhook::object<a>
{
public:
    using vmhook::object<a>::object;

    static auto get_total() -> std::int32_t
    {
        return static_members::get_field("total")->get();
    }

    auto player_entities() const -> player_vector
    {
        return get_field("playerEntities")->get().to_vector();
    }

    auto owner() const -> std::unique_ptr<player>
    {
        return get_field("owner")->get();
    }

    auto owner(const std::unique_ptr<player>& next) const -> void
    {
        get_field("owner")->set(next);
    }

    auto find_player(const std::unique_ptr<player>& target) const -> std::unique_ptr<player>
    {
        return get_method("findPlayer")->call(target);
    }

    auto players_by_entity() const -> player_entries
    {
        return get_field("playersByEntity")->get().to_vector();
    }

    auto player_entries_by_entity() const -> player_entries
    {
        return get_field("playersByEntity")->get().to_entries();
    }

    auto player_entities_eager() const -> player_vector
    {
        auto result = get_field("playerEntities")->get().to_vector<player>();
        return result;
    }
};

using inferred_field_vector =
    decltype(std::declval<const vmhook::field_proxy::value_t&>().to_vector());
using inferred_collection_vector =
    decltype(std::declval<const vmhook::collection&>().to_vector());
using inferred_map_vector =
    decltype(std::declval<const vmhook::map&>().to_vector());
using inferred_map_entries =
    decltype(std::declval<const vmhook::map&>().to_entries());
using inferred_field_entries =
    decltype(std::declval<const vmhook::field_proxy::value_t&>().to_entries());
using inferred_method_vector =
    decltype(std::declval<const vmhook::method_proxy::value_t&>().to_vector());

template<class wrapper_type>
concept collection_tag =
    std::is_base_of_v<vmhook::collection, wrapper_type>
    && std::is_constructible_v<wrapper_type, vmhook::oop_t>
    && std::is_constructible_v<wrapper_type, std::nullptr_t>
    && std::is_constructible_v<wrapper_type, const vmhook::borrowed<player>&>;

template<class wrapper_type>
concept map_tag =
    std::is_base_of_v<vmhook::map, wrapper_type>
    && std::is_constructible_v<wrapper_type, vmhook::oop_t>
    && std::is_constructible_v<wrapper_type, std::nullptr_t>
    && std::is_constructible_v<wrapper_type, const vmhook::borrowed<player>&>;

static_assert(collection_tag<vmhook::collection>);
static_assert(collection_tag<vmhook::list>);
static_assert(collection_tag<vmhook::array_list>);
static_assert(collection_tag<vmhook::linked_list>);
static_assert(collection_tag<vmhook::set>);
static_assert(collection_tag<vmhook::hash_set>);
static_assert(collection_tag<vmhook::linked_hash_set>);
static_assert(collection_tag<vmhook::tree_set>);
static_assert(collection_tag<vmhook::queue>);
static_assert(collection_tag<vmhook::deque>);
static_assert(collection_tag<vmhook::array_deque>);
static_assert(map_tag<vmhook::map>);
static_assert(map_tag<vmhook::hash_map>);
static_assert(map_tag<vmhook::linked_hash_map>);
static_assert(map_tag<vmhook::tree_map>);
static_assert(map_tag<vmhook::concurrent_hash_map>);

static_assert(std::is_same_v<decltype(a::get_total()), std::int32_t>);
static_assert(std::is_same_v<decltype(std::declval<const a&>().player_entities()),
                             player_vector>);
static_assert(std::is_convertible_v<inferred_field_vector, player_vector>);
static_assert(std::is_convertible_v<const inferred_field_vector&, player_vector>);
static_assert(std::is_convertible_v<inferred_field_vector, player_entries>);
static_assert(std::is_convertible_v<inferred_collection_vector, player_vector>);
static_assert(std::is_convertible_v<inferred_map_vector, player_entries>);
static_assert(std::is_convertible_v<inferred_map_entries, player_entries>);
static_assert(std::is_convertible_v<inferred_field_entries, player_entries>);
static_assert(std::is_convertible_v<inferred_method_vector, player_vector>);
static_assert(std::is_convertible_v<inferred_method_vector, player_entries>);
static_assert(!std::is_convertible_v<inferred_collection_vector, player_entries>);
static_assert(!std::is_convertible_v<inferred_map_vector, player_vector>);
static_assert(!std::is_convertible_v<inferred_field_entries, player_vector>);
static_assert(!std::is_convertible_v<inferred_field_vector, std::vector<int>>);
static_assert(!std::is_convertible_v<inferred_field_vector, std::vector<std::string>>);
static_assert(!std::is_convertible_v<inferred_field_vector,
                                   std::vector<std::unique_ptr<int>>>);
static_assert(!std::is_convertible_v<inferred_field_vector,
                                   std::vector<std::unique_ptr<const player>>>);
static_assert(!std::is_convertible_v<inferred_field_vector, std::unique_ptr<player>>);
static_assert(!std::is_convertible_v<inferred_field_vector, std::string>);
static_assert(!std::is_convertible_v<inferred_field_vector, int>);
static_assert(!std::is_convertible_v<inferred_field_vector, bool>);
static_assert(std::is_copy_constructible_v<inferred_field_vector>);
static_assert(std::is_move_constructible_v<inferred_field_vector>);
static_assert(std::is_same_v<decltype(std::declval<const vmhook::field_proxy::value_t&>()
                                        .to_vector<player>()), player_vector>);
static_assert(std::is_same_v<decltype(std::declval<const vmhook::collection&>()
                                        .to_vector<player>()), player_vector>);
static_assert(std::is_same_v<decltype(std::declval<const vmhook::map&>()
                                        .to_vector<entity, player>()), player_entries>);
static_assert(std::is_same_v<decltype(std::declval<const vmhook::map&>()
                                        .to_entries<entity, player>()), player_entries>);
static_assert(std::is_same_v<decltype(std::declval<const vmhook::method_proxy::value_t&>()
                                        .to_vector<player>()), player_vector>);
static_assert(std::is_same_v<decltype(std::declval<const vmhook::field_proxy::value_t&>()
                                        .to_vector<entity, player>()), player_entries>);

// Java object ownership is explicit at every public method/field/hook boundary.
// Primitive values and string copies remain ordinary C++ values.
template<class argument_type>
concept java_method_argument = requires(const vmhook::method_proxy& method, argument_type&& argument)
{
    method.call(std::forward<argument_type>(argument));
};

template<class argument_type>
concept java_constructor_argument = requires(argument_type&& argument)
{
    vmhook::make_unique<player>(std::forward<argument_type>(argument));
};

template<class argument_type>
concept java_field_argument = requires(const vmhook::field_proxy& field, argument_type&& argument)
{
    field.set(std::forward<argument_type>(argument));
};

template<class argument_type>
concept java_hook_result = requires(vmhook::return_value& result, argument_type&& argument)
{
    result.set(std::forward<argument_type>(argument));
};

template<class argument_type>
concept java_hook_argument_write = requires(vmhook::return_value& result, argument_type&& argument)
{
    result.set_arg(0, std::forward<argument_type>(argument));
};

template<class argument_type>
concept java_hook_callback = requires
{
    vmhook::hook<player>("target", std::declval<void(*)(vmhook::return_value&, argument_type)>());
};

template<class argument_type>
concept public_java_object_input = java_method_argument<argument_type>
    && java_constructor_argument<argument_type> && java_field_argument<argument_type>
    && java_hook_result<argument_type> && java_hook_argument_write<argument_type>;

template<class argument_type>
concept rejected_java_object_input = !java_method_argument<argument_type>
    && !java_constructor_argument<argument_type> && !java_field_argument<argument_type>
    && !java_hook_result<argument_type> && !java_hook_argument_write<argument_type>
    && !java_hook_callback<argument_type>;

static_assert(public_java_object_input<std::unique_ptr<player>>);
static_assert(public_java_object_input<const std::unique_ptr<player>&>);
static_assert(java_hook_callback<std::unique_ptr<player>>);
static_assert(java_hook_callback<const std::unique_ptr<player>&>);
static_assert(!java_hook_callback<std::unique_ptr<player>&>);
static_assert(!java_hook_callback<std::unique_ptr<player>&&>);
static_assert(rejected_java_object_input<player>);
static_assert(rejected_java_object_input<const player&>);
static_assert(rejected_java_object_input<vmhook::borrowed<player>>);
static_assert(rejected_java_object_input<const vmhook::borrowed<player>&>);
static_assert(rejected_java_object_input<vmhook::ref<player>>);
static_assert(rejected_java_object_input<std::nullptr_t>);
static_assert(rejected_java_object_input<void*>);
static_assert(rejected_java_object_input<std::unique_ptr<int>>);
static_assert(rejected_java_object_input<std::unique_ptr<const player>>);
static_assert(java_method_argument<std::int32_t>);
static_assert(java_method_argument<const std::string&>);
static_assert(java_field_argument<std::vector<std::int32_t>>);
static_assert(std::is_same_v<decltype(std::declval<const a&>().owner()), std::unique_ptr<player>>);
static_assert(std::is_same_v<decltype(std::declval<const a&>().find_player(
    std::declval<const std::unique_ptr<player>&>())), std::unique_ptr<player>>);

template<class source_type>
concept java_object_result = std::is_convertible_v<source_type, std::unique_ptr<player>>
    && !std::is_convertible_v<source_type, player>
    && !std::is_convertible_v<source_type, vmhook::borrowed<player>>
    && !std::is_convertible_v<source_type, vmhook::ref<player>>
    && !std::is_convertible_v<source_type, void*>
    && !std::is_convertible_v<source_type, std::unique_ptr<int>>
    && !std::is_convertible_v<source_type, std::unique_ptr<const player>>;

static_assert(java_object_result<vmhook::field_proxy::value_t>);
static_assert(java_object_result<vmhook::method_proxy::value_t>);
static_assert(std::is_same_v<decltype(std::declval<const vmhook::field_proxy::value_t&>().operator->()),
                             std::unique_ptr<vmhook::any_object>>);
static_assert(std::is_same_v<decltype(std::declval<const vmhook::method_proxy::value_t&>().operator->()),
                             std::unique_ptr<vmhook::any_object>>);
static_assert(std::is_same_v<decltype(std::declval<const vmhook::root<player>&>().get()),
                             std::unique_ptr<player>>);

template<class argument_type>
concept frame_java_argument = requires(const vmhook::hotspot::frame& frame)
{
    frame.template get_arguments<argument_type>();
};

static_assert(frame_java_argument<std::unique_ptr<player>>);
static_assert(!frame_java_argument<player>);
static_assert(!frame_java_argument<vmhook::borrowed<player>>);
static_assert(!frame_java_argument<vmhook::ref<player>>);

template<class wrapper_type>
concept owning_creation_helpers = requires
{
    vmhook::new_object<wrapper_type>(nullptr, std::size_t{});
    vmhook::new_array<wrapper_type>("[I", std::int32_t{}, sizeof(std::int32_t));
    vmhook::new_string<wrapper_type>("value");
};

static_assert(owning_creation_helpers<player>);
static_assert(!owning_creation_helpers<int>);
static_assert(!owning_creation_helpers<const player>);
static_assert(!owning_creation_helpers<volatile player>);
static_assert(std::is_same_v<decltype(vmhook::new_object(nullptr, std::size_t{})),
                             std::unique_ptr<vmhook::any_object>>);
static_assert(std::is_same_v<decltype(vmhook::new_array("[I", std::int32_t{}, sizeof(std::int32_t))),
                             std::unique_ptr<vmhook::any_object>>);
static_assert(std::is_same_v<decltype(vmhook::new_string("value")), std::unique_ptr<vmhook::any_object>>);
static_assert(std::is_same_v<decltype(vmhook::new_object<player>(nullptr, std::size_t{})),
                             std::unique_ptr<player>>);
static_assert(std::is_same_v<decltype(vmhook::new_array<player>("[I", std::int32_t{}, sizeof(std::int32_t))),
                             std::unique_ptr<player>>);
static_assert(std::is_same_v<decltype(vmhook::new_string<player>("value")), std::unique_ptr<player>>);
static_assert(std::is_same_v<decltype(std::declval<const player&>().self()), std::unique_ptr<player>>);

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

// These checks run without a JVM. Real wrapper accessors above are compiled
// but never invoked. Null sources exercise deferred conversion ownership,
// while moved-from adapters and primitive/void sources must fail explicitly.
auto check_container_adapters() -> bool
{
    auto deferred = vmhook::array_list{nullptr}.to_vector();
    const auto copied = deferred;
    auto moved = std::move(deferred);
    player_vector from_copy = copied;
    player_vector from_move = moved;
    player_vector assigned;
    assigned = copied;
    player_entries entries = vmhook::map{nullptr}.to_vector();
    player_entries explicit_entries = vmhook::map{nullptr}.to_entries<entity, player>();
    player_vector field_null = vmhook::field_proxy::value_t{
        std::uint32_t{}, "Ljava/util/List;"}.to_vector();
    player_vector method_null =
        vmhook::method_proxy::value_t::completed_reference({}).to_vector();
    if (!from_copy.empty() || !from_move.empty() || !assigned.empty()
        || !entries.empty() || !explicit_entries.empty() || !field_null.empty()
        || !method_null.empty())
    {
        return false;
    }
    try
    {
        player_vector invalid = deferred;
        (void)invalid;
        return false;
    }
    catch (const vmhook::exception&)
    {
    }
    try
    {
        player_vector primitive = vmhook::field_proxy::value_t{
            std::int32_t{}, "I"}.to_vector();
        (void)primitive;
        return false;
    }
    catch (const vmhook::exception&)
    {
    }
    try
    {
        player_vector void_result = vmhook::method_proxy::value_t{}.to_vector();
        (void)void_result;
        return false;
    }
    catch (const vmhook::exception&)
    {
    }
    return true;
}

auto check_native_reference_handoff() -> bool
{
    using native_result = vmhook::detail::native_backend::native_call_result;
    const auto actual_null = vmhook::method_proxy::value_t::completed_native_reference(
        native_result{.invoked = true});
    player_vector null_vector = actual_null.to_vector();
    const std::unique_ptr<player> null_object = actual_null;
    if (!actual_null.was_invoked() || !actual_null.reference_result
        || actual_null.result_failed || !null_vector.empty()
        || !null_object || null_object->get_instance())
    {
        return false;
    }

    // A lost non-null ticket and a failed promotion must remain failures,
    // rather than becoming a successful Java null when the public value copies.
    const std::array<native_result, 4> failures{
        native_result{.invoked = true, .bits = 1},
        native_result{.invoked = true, .error = "bookkeeping failed"},
        native_result{},
        native_result{.invoked = true, .threw = true}
    };
    for (const auto& failed : failures)
    {
        const auto result =
            vmhook::method_proxy::value_t::completed_native_reference(failed);
        if (result.was_invoked() != failed.invoked || result.threw() != failed.threw
            || !result.reference_result)
        {
            return false;
        }
        if ((failed.bits != 0 || !failed.error.empty()) && !result.result_failed)
        {
            return false;
        }
        try
        {
            player_vector invalid = result.to_vector();
            (void)invalid;
            return false;
        }
        catch (const vmhook::exception&)
        {
        }
        try
        {
            const std::unique_ptr<player> invalid = result;
            (void)invalid;
            return false;
        }
        catch (const vmhook::exception&)
        {
        }
    }
    return true;
}

auto check_null_object_values() -> bool
{
    const vmhook::field_proxy::value_t null_field{std::uint32_t{}, "Ltest/Player;"};
    const std::unique_ptr<player> first = null_field;
    const std::unique_ptr<player> second = null_field;
    const auto null_arrow = null_field.operator->();
    if (!first || first->get_instance() || !second || second->get_instance()
        || first.get() == second.get() || !null_arrow || null_arrow->get_instance())
    { return false; }

    // Zero primitive bits, a malformed reference alternative, an uninvoked
    // result, and a lost non-null reference must never become Java null.
    const std::array<vmhook::field_proxy::value_t, 3> bad_fields{
        vmhook::field_proxy::value_t{std::int32_t{}, "I"},
        vmhook::field_proxy::value_t{std::int32_t{}, "Ltest/Player;"},
        vmhook::field_proxy::value_t{std::uint32_t{1}, "Ltest/Player;"}
    };
    for (const auto& field : bad_fields)
    {
        try { const std::unique_ptr<player> invalid = field; (void)invalid; return false; }
        catch (const vmhook::exception&) {}
    }
    try
    {
        const std::unique_ptr<player> invalid = vmhook::method_proxy::value_t::completed(std::int32_t{});
        (void)invalid;
        return false;
    }
    catch (const vmhook::exception&) {}
    return true;
}

auto check_creation_context_refusal() -> bool
{
    try { (void)vmhook::new_object(nullptr, std::size_t{}); return false; }
    catch (const vmhook::exception&) {}
    try { (void)vmhook::new_array("[I", std::int32_t{1}, sizeof(std::int32_t)); return false; }
    catch (const vmhook::exception&) {}
    try { (void)vmhook::new_string("value"); return false; }
    catch (const vmhook::exception&) {}
    return true;
}

auto other_translation_unit() -> bool;

auto main() -> int
{
    if (!other_translation_unit()) { return 10; }
    if (!check_container_adapters()) { return 11; }
    if (!check_native_reference_handoff()) { return 12; }
    if (!check_null_object_values()) { return 13; }
    if (!check_creation_context_refusal()) { return 14; }
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
