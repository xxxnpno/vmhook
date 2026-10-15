#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
import vmhook.windows;
#else
#include <format>
#include <iostream>
#include <vmhook/vmhook.hpp>
#endif

namespace
{
    std::atomic<bool> fail_next_allocation{};
    struct deliberately_long_type_name_for_allocation_failure {};
}

[[gnu::noinline]] void* operator new(const std::size_t size)
{
    if (fail_next_allocation.exchange(false)) { throw std::bad_alloc{}; }
    if (void* result = std::malloc(size == 0 ? 1 : size)) { return result; }
    throw std::bad_alloc{};
}
[[gnu::noinline]] void* operator new[](const std::size_t size) { return ::operator new(size); }
[[gnu::noinline]] void operator delete(void* pointer) noexcept { std::free(pointer); }
[[gnu::noinline]] void operator delete[](void* pointer) noexcept { std::free(pointer); }
[[gnu::noinline]] void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
[[gnu::noinline]] void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

int main()
{
    int failures{};
    const auto check = [&](bool condition, const char* description)
    {
        if (!condition) { std::cerr << std::format("FAIL: {}\n", description); ++failures; }
    };
    const auto decode = [](std::string_view bytes)
    {
        const auto decoded = vmhook::detail::utf8_to_utf16(bytes);
        std::u16string result;
        for (const auto unit : decoded) { result.push_back(static_cast<char16_t>(unit)); }
        return result;
    };
    constexpr char scalar_bytes[]{
        "\x00\x7f\xc2\x80\xdf\xbf\xe0\xa0\x80\xed\x9f\xbf\xee\x80\x80"
        "\xef\xbf\xbf\xf0\x90\x80\x80\xf4\x8f\xbf\xbf"};
    const std::string scalars{scalar_bytes, sizeof(scalar_bytes) - 1};
    const std::u16string scalar_units{
        char16_t{0}, char16_t{0x7f}, char16_t{0x80}, char16_t{0x7ff},
        char16_t{0x800}, char16_t{0xd7ff}, char16_t{0xe000}, char16_t{0xffff},
        char16_t{0xd800}, char16_t{0xdc00}, char16_t{0xdbff}, char16_t{0xdfff}};
    check(decode(scalars) == scalar_units, "UTF8 scalar boundaries decode exactly, including NUL");
    check(vmhook::detail::utf16_to_utf8(scalar_units) == scalars,
          "UTF16 scalar boundaries round trip to standard UTF8");
    check(decode("\xc0\xaf") == u"\ufffd\ufffd", "overlong two-byte encoding is rejected");
    check(decode("\xe0\x80\xaf") == u"\ufffd\ufffd\ufffd", "overlong three-byte encoding is rejected");
    check(decode("\xf0\x80\x80\xaf") == u"\ufffd\ufffd\ufffd\ufffd", "overlong four-byte encoding is rejected");
    check(decode("\xed\xa0\x80") == u"\ufffd\ufffd\ufffd", "UTF8 encoded surrogate is rejected");
    check(decode("\xf4\x90\x80\x80") == u"\ufffd\ufffd\ufffd\ufffd", "scalar above U+10FFFF is rejected");
    check(decode("\xf5\x80\x80\x80") == u"\ufffd\ufffd\ufffd\ufffd", "invalid starter is rejected");
    check(decode("\x80\xbf") == u"\ufffd\ufffd", "isolated continuation bytes are rejected");
    check(decode("\xe2\x82") == u"\ufffd\ufffd", "truncated sequence is rejected without an overread");
    check(decode("\xe2" "A\xa1") == u"\ufffdA\ufffd", "invalid continuation preserves the next ASCII scalar");
    const std::u16string isolated{char16_t{0xd800}, u'A', char16_t{0xdc00}};
    check(vmhook::detail::utf16_to_utf8(isolated) == "\xef\xbf\xbd" "A\xef\xbf\xbd",
          "isolated UTF16 surrogates are replaced with valid UTF8");
    check(decode({}).empty() && vmhook::detail::utf16_to_utf8({}).empty(),
          "empty strings remain successful empty conversions");
    const std::string metadata{"A\xc0\x80\xed\xa0\x81\xed\xb0\x80Z"};
    const std::u16string metadata_units{u'A', char16_t{}, char16_t{0xd801}, char16_t{0xdc00}, u'Z'};
    check(vmhook::detail::modified_utf8_to_utf16(metadata) == metadata_units,
          "metadata preserves modified UTF8 NUL and supplementary UTF16 units");
    check(vmhook::detail::modified_utf8_to_utf16("\xed\xa0\x80")
          == std::u16string{char16_t{0xd800}}, "metadata preserves isolated UTF16 units");
    check(vmhook::detail::modified_utf8_to_utf16(
              "\x7f\xc2\x80\xdf\xbf\xe0\xa0\x80\xed\x9f\xbf\xee\x80\x80\xef\xbf\xbf")
          == std::u16string{char16_t{0x7f}, char16_t{0x80}, char16_t{0x7ff},
                           char16_t{0x800}, char16_t{0xd7ff}, char16_t{0xe000}, char16_t{0xffff}},
          "modified UTF8 metadata preserves every one/two/three-byte boundary");
    for (const std::string_view malformed : std::array<std::string_view, 9>{std::string_view{"\0", 1}, "\xc0\x81", "\xc1\xbf",
            "\xe0\x80\x80", "\xc2", "\xe2\x82", "\xe2" "A\xa1", "\x80", "\xf0\x90\x90\x80"})
    {
        bool refused_metadata{};
        try { (void)vmhook::detail::modified_utf8_to_utf16(malformed); }
        catch (const vmhook::exception&) { refused_metadata = true; }
        check(refused_metadata, "malformed modified UTF8 metadata is refused without replacement");
    }
    bool null_refused{};
    try { (void)vmhook::read_java_string_utf16(nullptr); }
    catch (const vmhook::exception&) { null_refused = true; }
    check(null_refused, "Java null String is distinct from an empty C++ string");
    const auto refused = vmhook::method_proxy::value_t{};
    check(refused.status() == vmhook::error_code::refused && !refused.is_void(),
          "a refused invocation is distinct from successful void");
    bool refused_zero{};
    try { (void)static_cast<std::int32_t>(refused); }
    catch (const vmhook::exception& error) { refused_zero = error.code() == vmhook::error_code::refused; }
    check(refused_zero, "primitive conversion cannot hide refusal as zero");
    check(static_cast<std::int32_t>(vmhook::method_proxy::value_t::completed(std::int32_t{})) == 0,
          "successful primitive zero remains zero");
    check(!static_cast<bool>(vmhook::method_proxy::value_t::completed(false)),
          "successful false remains false");
    check(vmhook::method_proxy::value_t::completed(std::monostate{}).is_void(),
          "successful void is explicit");
    auto thrown = vmhook::method_proxy::value_t::completed(std::int32_t{});
    thrown.exception_thrown = true;
    thrown.exception_class = "java/lang/IllegalStateException";
    check(thrown.status() == vmhook::error_code::java_exception,
          "Java throw status is distinct from refusal");
    bool thrown_zero{};
    try { (void)static_cast<std::int32_t>(thrown); }
    catch (const vmhook::exception& error)
    { thrown_zero = error.code() == vmhook::error_code::java_exception
            && error.exception_class() == "java/lang/IllegalStateException"; }
    check(thrown_zero, "Java exception diagnostics survive checked primitive conversion");
    vmhook::method_proxy invalid_method{nullptr, nullptr, "()V"};
    check(invalid_method.try_call().status() == vmhook::error_code::refused,
          "try_call returns explicit refusal");
    bool checked_void{};
    try { invalid_method.call(); }
    catch (const vmhook::exception& error) { checked_void = error.code() == vmhook::error_code::refused; }
    check(checked_void, "ignored void calls still report failure");
    vmhook::field_proxy invalid_field{nullptr, "I", false};
    check(invalid_field.try_get().status() == vmhook::error_code::refused,
          "try_get reports failure without a fabricated field value");
    bool checked_field{};
    try { (void)invalid_field.get(); }
    catch (const vmhook::exception&) { checked_field = true; }
    check(checked_field, "field get checks failure immediately");
    alignas(8) std::int32_t storage{};
    vmhook::field_proxy valid_field{&storage, "I", false};
    check(valid_field.try_get().status() == vmhook::error_code::none
          && static_cast<std::int32_t>(valid_field.get()) == 0,
          "successful zero field is distinct from unreadable storage");
    valid_field.set(std::int32_t{19});
    check(storage == 19 && valid_field.try_set(std::int32_t{23}) && storage == 23,
          "checked and try setters report successful writes");
    check(!valid_field.try_set(1.0f) && storage == 23,
          "same-width incompatible primitive types cannot reinterpret field bits");
    check(!valid_field.try_set(std::int64_t{1}) && storage == 23,
          "a wrong-width write is an explicit refusal and leaves the field unchanged");
    vmhook::field_proxy unmapped_field{reinterpret_cast<void*>(0x10000), "I", false};
    check(unmapped_field.try_get().status() == vmhook::error_code::refused
          && !unmapped_field.try_set(std::int32_t{1}),
          "unmapped primitive reads and writes are explicit failures");
    bool utf_allocation{};
    fail_next_allocation.store(true);
    try { (void)vmhook::detail::utf8_to_utf16("allocate"); }
    catch (const std::bad_alloc&) { utf_allocation = true; }
    check(utf_allocation, "Unicode allocation failure propagates std::bad_alloc");
    bool metadata_allocation{};
    fail_next_allocation.store(true);
    try { (void)vmhook::detail::modified_utf8_to_utf16(std::string_view{scalar_bytes, sizeof(scalar_bytes) - 1}); }
    catch (const std::bad_alloc&) { metadata_allocation = true; }
    fail_next_allocation.store(false);
    check(metadata_allocation, "metadata conversion preserves the original C++ allocation failure");
    // Initialize Windows emulated TLS/destructor registration before faulting
    // the operation's own C++ allocation, rather than a runtime TLS allocation.
    (void)vmhook::detail::call_handle_pool();
    (void)vmhook::detail::call_handle_depth;
    bool handle_allocation{};
    fail_next_allocation.store(true);
    try { vmhook::detail::call_handles_guard handles; }
    catch (const std::bad_alloc&) { handle_allocation = true; }
    check(handle_allocation && vmhook::detail::call_handle_depth == 0,
          "call-handle allocation failure propagates and preserves nesting state");
    const auto copied = vmhook::method_proxy::value_t::completed(std::string(512, 'x'));
    bool copy_allocation{};
    fail_next_allocation.store(true);
    try { (void)copied.as_string(); }
    catch (const std::bad_alloc&) { copy_allocation = true; }
    check(copy_allocation, "String-copy allocation failure propagates without terminate or Java null");
    bool descriptor_allocation{};
    fail_next_allocation.store(true);
    try { (void)vmhook::detail::jvm_descriptor_for_arg<std::string>(); }
    catch (const std::bad_alloc&) { descriptor_allocation = true; }
    check(descriptor_allocation, "descriptor allocation failure remains a C++ allocation failure");
    bool diagnostic_allocation{};
    fail_next_allocation.store(true);
    try { (void)vmhook::detail::type_name<deliberately_long_type_name_for_allocation_failure>(); }
    catch (const std::bad_alloc&) { diagnostic_allocation = true; }
    check(diagnostic_allocation, "type-name diagnostics do not terminate on C++ allocation failure");
    bool field_diagnostic_allocation{};
    fail_next_allocation.store(true);
    try { (void)vmhook::get_field<std::int32_t>(nullptr, nullptr, "allocation_missing_field"); }
    catch (const std::bad_alloc&) { field_diagnostic_allocation = true; }
    check(field_diagnostic_allocation, "raw field helpers do not hide diagnostic allocation failure as zero");
    const auto field_failure{ vmhook::field_proxy::value_t::failed(
        vmhook::exception{"stored field failure", vmhook::error_code::invalid_memory}) };
    vmhook::method_proxy::value_t method_failure{};
    method_failure.invoked = true;
    method_failure.exception_thrown = true;
    method_failure.exception_class = "java/lang/IllegalStateException";
    const auto preserves_field_error = [&](auto convert)
    {
        try { convert(); }
        catch (const vmhook::exception& error)
        { return error.code() == vmhook::error_code::invalid_memory && std::string_view{error.what()} == "stored field failure"; }
        return false;
    };
    const auto preserves_java_error = [&](auto convert)
    {
        try { convert(); }
        catch (const vmhook::exception& error)
        { return error.code() == vmhook::error_code::java_exception && error.exception_class() == "java/lang/IllegalStateException"; }
        return false;
    };
    check(preserves_field_error([&] { (void)field_failure.to_vector(); })
          && preserves_field_error([&] { (void)field_failure.to_entries<vmhook::any_object, vmhook::any_object>(); }),
          "lazy and typed collection conversions preserve the stored field failure");
    check(preserves_java_error([&] { (void)method_failure.to_entries(); })
          && preserves_java_error([&] { (void)method_failure.to_vector<vmhook::any_object>(); }),
          "lazy and typed collection conversions preserve the original Java exception");
    return failures ? 1 : 0;
}
