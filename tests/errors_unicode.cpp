#if defined(VMHOOK_TEST_MODULE)
import std;
import vmhook;
import vmhook.windows;
#else
#include <vmhook/vmhook.hpp>
#endif

int main()
{
    int failures{};
    const auto check = [&](bool condition, const char* description)
    {
        if (!condition) { std::cerr << "FAIL: " << description << '\n'; ++failures; }
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
    bool null_refused{};
    try { (void)vmhook::read_java_string_utf16(nullptr); }
    catch (const vmhook::exception&) { null_refused = true; }
    check(null_refused, "Java null String is distinct from an empty C++ string");
    return failures ? 1 : 0;
}
