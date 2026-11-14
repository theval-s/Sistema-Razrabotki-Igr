#pragma once

#include <engine/engine_export.hpp>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace engine::utf {
// Strict Unicode: reject overlong UTF-8, surrogates encoded in UTF-8, values
// above U+10FFFF, and unpaired UTF-16 surrogates. Embedded NUL is ordinary text.
// nullopt means malformed input; an engaged empty string/count means success.
ENGINE_API std::optional<std::size_t> Utf8ScalarCount(std::string_view text);
ENGINE_API bool IsValidUtf8(std::string_view text);
ENGINE_API std::optional<std::u16string> ToUtf16(std::string_view text);
ENGINE_API std::optional<std::string> ToUtf8(std::u16string_view text);

// Native wide strings are interpreted as UTF-16 code units, even on platforms
// with 32-bit wchar_t (units above 0xFFFF are rejected).
ENGINE_API std::optional<std::wstring> ToWide(std::string_view text);
ENGINE_API std::optional<std::string> ToUtf8(std::wstring_view text);

// Host offset mapping is deliberately tolerant: an unpaired surrogate counts
// as one scalar. Offsets clamp to the text end. A unit offset inside a pair
// counts its high surrogate as one (prefix semantics, matching Qt/Win32 hosts).
// These helpers do not validate text; conversions above remain strict.
ENGINE_API std::size_t Utf16ScalarOffset(std::u16string_view text, std::size_t unitOffset);
ENGINE_API std::size_t Utf16UnitOffset(std::u16string_view text, std::size_t scalarOffset);
ENGINE_API std::size_t Utf16ScalarOffset(std::wstring_view text, std::size_t unitOffset);
ENGINE_API std::size_t Utf16UnitOffset(std::wstring_view text, std::size_t scalarOffset);
} // namespace engine::utf
