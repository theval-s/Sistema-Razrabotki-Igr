#include <engine/core/Utf.hpp>

#include <algorithm>
#include <cstdint>

namespace engine::utf {
namespace {
bool High(std::uint32_t c) { return c >= 0xd800 && c <= 0xdbff; }
bool Low(std::uint32_t c) { return c >= 0xdc00 && c <= 0xdfff; }

bool Next(std::string_view text, std::size_t& i, std::uint32_t& value) {
    const auto c = static_cast<unsigned char>(text[i++]);
    if (c < 0x80) { value = c; return true; }
    const unsigned n = c >= 0xf0 && c <= 0xf4 ? 3 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xc2 && c <= 0xdf ? 1 : 0;
    if (!n || n > text.size() - i) return false;
    value = c & ((1u << (6 - n)) - 1);
    for (unsigned j = 0; j < n; ++j) {
        const auto next = static_cast<unsigned char>(text[i++]);
        if ((next & 0xc0) != 0x80) return false;
        value = (value << 6) | (next & 63);
    }
    return value >= (n == 1 ? 0x80u : n == 2 ? 0x800u : 0x10000u) &&
        value <= 0x10ffff && !High(value) && !Low(value);
}

template<class Char>
std::optional<std::basic_string<Char>> Decode(std::string_view text) {
    std::basic_string<Char> result;
    for (std::size_t i = 0; i < text.size();) {
        std::uint32_t value;
        if (!Next(text, i, value)) return std::nullopt;
        if (value <= 0xffff) result.push_back(static_cast<Char>(value));
        else {
            value -= 0x10000;
            result.push_back(static_cast<Char>(0xd800 + (value >> 10)));
            result.push_back(static_cast<Char>(0xdc00 + (value & 0x3ff)));
        }
    }
    return result;
}

template<class Char>
std::optional<std::string> Encode(std::basic_string_view<Char> text) {
    std::string result;
    for (std::size_t i = 0; i < text.size(); ++i) {
        auto value = static_cast<std::uint32_t>(text[i]);
        if (value > 0xffff || Low(value)) return std::nullopt;
        if (High(value)) {
            if (i + 1 == text.size() || !Low(static_cast<std::uint32_t>(text[i + 1]))) return std::nullopt;
            value = 0x10000 + ((value - 0xd800) << 10) + (static_cast<std::uint32_t>(text[++i]) - 0xdc00);
        }
        if (value < 0x80) result.push_back(static_cast<char>(value));
        else {
            if (value < 0x800) result.push_back(static_cast<char>(0xc0 | (value >> 6)));
            else {
                if (value < 0x10000) result.push_back(static_cast<char>(0xe0 | (value >> 12)));
                else {
                    result.push_back(static_cast<char>(0xf0 | (value >> 18)));
                    result.push_back(static_cast<char>(0x80 | ((value >> 12) & 63)));
                }
                result.push_back(static_cast<char>(0x80 | ((value >> 6) & 63)));
            }
            result.push_back(static_cast<char>(0x80 | (value & 63)));
        }
    }
    return result;
}

template<class Char>
std::size_t Scalars(std::basic_string_view<Char> text, std::size_t offset) {
    offset = (std::min)(offset, text.size());
    std::size_t count = 0;
    for (std::size_t i = 0; i < offset; ++i, ++count)
        if (High(text[i]) && i + 1 < offset && Low(text[i + 1])) ++i;
    return count;
}
template<class Char>
std::size_t Units(std::basic_string_view<Char> text, std::size_t offset) {
    std::size_t i = 0;
    for (std::size_t n = 0; n < offset && i < text.size(); ++n, ++i)
        if (High(text[i]) && i + 1 < text.size() && Low(text[i + 1])) ++i;
    return i;
}
} // namespace

std::optional<std::size_t> Utf8ScalarCount(std::string_view text) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < text.size(); ++count) {
        std::uint32_t value;
        if (!Next(text, i, value)) return std::nullopt;
    }
    return count;
}
bool IsValidUtf8(std::string_view text) { return Utf8ScalarCount(text).has_value(); }
std::optional<std::u16string> ToUtf16(std::string_view text) { return Decode<char16_t>(text); }
std::optional<std::wstring> ToWide(std::string_view text) { return Decode<wchar_t>(text); }
std::optional<std::string> ToUtf8(std::u16string_view text) { return Encode(text); }
std::optional<std::string> ToUtf8(std::wstring_view text) { return Encode(text); }
std::size_t Utf16ScalarOffset(std::u16string_view text, std::size_t offset) { return Scalars(text, offset); }
std::size_t Utf16ScalarOffset(std::wstring_view text, std::size_t offset) { return Scalars(text, offset); }
std::size_t Utf16UnitOffset(std::u16string_view text, std::size_t offset) { return Units(text, offset); }
std::size_t Utf16UnitOffset(std::wstring_view text, std::size_t offset) { return Units(text, offset); }
} // namespace engine::utf
