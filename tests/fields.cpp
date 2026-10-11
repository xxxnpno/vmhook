#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
import vmhook.windows;
#else
#include <vmhook/vmhook.hpp>
#endif

int main()
{
    struct alignas(8) object_data
    {
        std::int32_t value{17};
        std::uint32_t adjacent{0xabcddcba};
    } object;
    alignas(8) std::array<std::byte, 64> klass_storage{};
    auto* klass = reinterpret_cast<vmhook::hotspot::klass*>(klass_storage.data());
    {
        std::lock_guard lock{vmhook::g_field_cache_mutex};
        vmhook::g_field_cache[klass].emplace("value",
            vmhook::hotspot::field_entry_t{0, false, "I", klass});
        vmhook::g_field_cache[klass].emplace("reference",
            vmhook::hotspot::field_entry_t{0, false, "Ljava/lang/Object;", klass});
    }

    int failures{};
    const auto check = [&](bool condition, const char* description)
    {
        if (!condition)
        {
            std::cerr << "FAIL: " << description << '\n';
            ++failures;
        }
    };

    check(vmhook::get_field<std::int32_t>(&object, klass, "value") == 17,
          "read a matching Java int width");
    vmhook::set_field<std::int32_t>(&object, klass, "value", 29);
    check(object.value == 29 && object.adjacent == 0xabcddcba,
          "matching int write preserves its neighbor");
    check(vmhook::get_field<std::int64_t>(&object, klass, "value") == 0,
          "a wide read cannot consume the adjacent field");
    vmhook::set_field<std::int64_t>(&object, klass, "value", 0x1122334455667788LL);
    check(object.value == 29 && object.adjacent == 0xabcddcba,
          "a wide write cannot corrupt the adjacent field");
    check(vmhook::get_field<std::int32_t>(reinterpret_cast<void*>(0x10000), klass, "value") == 0,
          "an unmapped object address fails safely");
    vmhook::set_field<std::int32_t>(reinterpret_cast<void*>(0x10000), klass, "value", 99);
    vmhook::set_field<std::uint32_t>(&object, klass, "reference", 0x778899aa);
    check(object.value == 29 && object.adjacent == 0xabcddcba,
          "reference writes refuse an unverified collector");

    vmhook::field_proxy reference_proxy{&object.value, "Ljava/lang/Object;", false};
    check(reference_proxy.get_compressed_oop() == 0,
          "a typed reference read refuses an unknown compression layout");
    check(reference_proxy.try_get().status() == vmhook::error_code::refused,
          "a refused reference read is distinct from a successful primitive zero");
    check(vmhook::detail::reference_slot_width() == 0
          && vmhook::detail::read_reference_slot(&object, 0) == nullptr,
          "an anchor read refuses an unknown reference width");

    vmhook::field_proxy stale_static{&object.value, "I", true, klass, 0};
    check(stale_static.try_get().status() == vmhook::error_code::refused,
          "a missing class mirror cannot fall back to a cached static address");
    check(!stale_static.try_set(std::int32_t{73}), "a checked static write reports a missing mirror");
    check(object.value == 29, "a missing mirror refuses a static write");
    vmhook::field_proxy stale_character{&object.value, "C", true, klass, 0};
    check(!stale_character.try_set('X'), "character widening reports a missing mirror");
    check(object.value == 29, "character widening also refuses a missing mirror");
    vmhook::field_proxy unmapped_character{reinterpret_cast<void*>(0x10000), "C", false};
    check(!unmapped_character.try_set('X'), "an unmapped character write reports refusal");

    // Identity hash, age and lock transitions must preserve a compact Klass.
    const vmhook::detail::compact_header_constants_t compact{42, 3, 0, 1, 2, 3};
    constexpr std::uint32_t narrow_klass{0x234567};
    constexpr std::uint64_t klass_mark{std::uint64_t{narrow_klass} << 42};
    const auto noisy_mark{klass_mark | (std::uint64_t{0x11223344} << 11) | 0x58u};
    check(vmhook::detail::compact_klass_bits(noisy_mark | 1u, compact, true) == narrow_klass,
          "compact unlocked headers preserve Klass across hash and age bits");
    check(vmhook::detail::compact_klass_bits(noisy_mark, compact, true) == narrow_klass,
          "compact lightweight locks preserve Klass bits");
    check(vmhook::detail::compact_klass_bits(noisy_mark | 2u, compact, true) == narrow_klass,
          "compact inflated monitors preserve Klass with a verified monitor table");
    check(!vmhook::detail::compact_klass_bits(noisy_mark | 2u, compact, false),
          "an unverified monitor pointer is never decoded as compact Klass bits");
    check(!vmhook::detail::compact_klass_bits(klass_mark | 3u, compact, true),
          "a GC forwarded or marked object does not expose a Klass from the forwarding pointer");
    check(!vmhook::detail::compact_klass_bits(1u, compact, true),
          "a compact header with no Klass is rejected");
    auto invalid_compact{compact};
    invalid_compact.klass_shift = 64;
    check(!vmhook::detail::compact_klass_bits(klass_mark | 1u, invalid_compact, true),
          "an invalid exported shift cannot trigger undefined arithmetic");
    invalid_compact = compact;
    invalid_compact.monitor_value = invalid_compact.neutral_value;
    check(!vmhook::detail::compact_klass_bits(klass_mark | 1u, invalid_compact, true),
          "ambiguous exported mark states refuse decoding");

    // Registering another wrapper can rehash the shared registry while a
    // detour resolves a descriptor. Lookups must own their copied strings.
    struct registry_tag {};
    const auto registry_type{std::type_index{typeid(registry_tag)}};
    const std::string first_name(512, 'A');
    const std::string second_name(512, 'B');
    {
        std::lock_guard lock{vmhook::registration_mutex};
        vmhook::type_to_class_map.insert_or_assign(registry_type, first_name);
    }
    const auto retained_name{vmhook::detail::registered_class_name<registry_tag>()};
    std::atomic<bool> registry_valid{true};
    std::array<std::thread, 3> registry_threads;
    registry_threads[0] = std::thread([&]
    {
        for (std::size_t iteration{}; iteration < 2000; ++iteration)
        {
            std::lock_guard lock{vmhook::registration_mutex};
            vmhook::type_to_class_map.insert_or_assign(registry_type,
                iteration % 2 ? first_name : second_name);
            vmhook::type_to_class_map.rehash(1 + (iteration % 32) * 16);
        }
    });
    for (std::size_t reader{1}; reader < registry_threads.size(); ++reader)
    {
        registry_threads[reader] = std::thread([&, reader]
        {
            for (std::size_t iteration{}; iteration < 2000; ++iteration)
            {
                const auto name{reader == 1
                    ? vmhook::detail::registered_class_name<registry_tag>()
                    : vmhook::detail::registered_class_name(registry_type)};
                if (name != first_name && name != second_name)
                {
                    registry_valid.store(false, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& thread : registry_threads) { thread.join(); }
    check(registry_valid.load(std::memory_order_relaxed) && retained_name == first_name,
          "registered names remain coherent copies while writers update and rehash");
    {
        std::lock_guard lock{vmhook::registration_mutex};
        vmhook::type_to_class_map.erase(registry_type);
    }
    check(vmhook::detail::registered_class_name<registry_tag>().empty(),
          "removed registration returns an empty copied name");

    {
        std::lock_guard lock{vmhook::g_field_cache_mutex};
        vmhook::g_field_cache.erase(klass);
    }
    return failures ? 1 : 0;
}
