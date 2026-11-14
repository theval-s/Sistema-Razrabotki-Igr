#include <engine/core/Utf.hpp>
#include <gtest/gtest.h>
#include <limits>
#include <vector>

namespace {
using namespace engine::utf;

TEST(Utf, EmptyIsSuccessAndNulIsText) {
    ASSERT_TRUE(ToUtf16(""));
    EXPECT_TRUE(ToUtf16("")->empty());
    ASSERT_TRUE(ToUtf8(u""));
    EXPECT_TRUE(ToUtf8(u"")->empty());
    EXPECT_EQ(Utf8ScalarCount(""), 0u);
    const std::string bytes("a\0b", 3);
    const std::u16string units(u"a\0b", 3);
    EXPECT_TRUE(IsValidUtf8(bytes));
    EXPECT_EQ(Utf8ScalarCount(bytes), 3u);
    EXPECT_EQ(ToUtf16(bytes), units);
    EXPECT_EQ(ToUtf8(units), bytes);
    const auto wide = ToWide(bytes);
    ASSERT_TRUE(wide);
    EXPECT_EQ(ToUtf8(*wide), bytes);
}

TEST(Utf, RoundTripsEncodingBoundariesAndSupplementaryPairs) {
    const std::string bytes = "\x7f\xc2\x80\xdf\xbf\xe0\xa0\x80\xed\x9f\xbf\xee\x80\x80\xef\xbf\xbf\xf0\x90\x80\x80\xf4\x8f\xbf\xbf";
    const std::u16string units = u"\x7f\x80\x7ff\x800\xd7ff\xe000\xffff\xd800\xdc00\xdbff\xdfff";
    EXPECT_EQ(Utf8ScalarCount(bytes), 9u);
    EXPECT_EQ(ToUtf16(bytes), units);
    EXPECT_EQ(ToUtf8(units), bytes);
    const auto wide = ToWide(bytes);
    ASSERT_TRUE(wide);
    EXPECT_EQ(wide->size(), units.size());
    EXPECT_EQ(ToUtf8(*wide), bytes);
}

TEST(Utf, RejectsMalformedUtf8InEveryStrictOperation) {
    const std::vector<std::string> malformed = {
        "\x80", "\xbf", "\xc0\x80", "\xc1\xbf", "\xc2", "\xc2x",
        "\xe0\x80\x80", "\xe2\x82", "\xe2x\xac", "\xed\xa0\x80", "\xed\xbf\xbf",
        "\xf0\x80\x80\x80", "\xf0\x90\x80", "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "\xff"
    };
    for (const auto& bytes : malformed) {
        EXPECT_FALSE(IsValidUtf8(bytes));
        EXPECT_FALSE(Utf8ScalarCount(bytes));
        EXPECT_FALSE(ToUtf16(bytes));
        EXPECT_FALSE(ToWide(bytes));
        EXPECT_FALSE(ToUtf16(std::string("valid-prefix") + bytes));
    }
}

TEST(Utf, RejectsUnpairedUtf16WithoutSubstitution) {
    const std::vector<std::u16string> malformed = {u"\xd800", u"\xdc00", u"\xd800x", u"\xd800\xd800", u"\xdc00\xd800", u"x\xdfff"};
    for (const auto& units : malformed) {
        EXPECT_FALSE(ToUtf8(units));
        const std::wstring wide(units.begin(), units.end());
        EXPECT_FALSE(ToUtf8(wide));
    }
}

TEST(Utf, OffsetMappingClampsAndPreservesPrefixSemantics) {
    const std::u16string text = u"A\xd83d\xde00Z";
    const std::wstring wide(text.begin(), text.end());
    const std::size_t scalarOffsets[] = {0, 1, 2, 2, 3};
    const std::size_t unitOffsets[] = {0, 1, 3, 4, 4};
    for (std::size_t i = 0; i < 5; ++i) {
        EXPECT_EQ(Utf16ScalarOffset(text, i), scalarOffsets[i]);
        EXPECT_EQ(Utf16ScalarOffset(wide, i), scalarOffsets[i]);
        EXPECT_EQ(Utf16UnitOffset(text, i), unitOffsets[i]);
        EXPECT_EQ(Utf16UnitOffset(wide, i), unitOffsets[i]);
    }
    const auto huge = (std::numeric_limits<std::size_t>::max)();
    EXPECT_EQ(Utf16ScalarOffset(text, huge), 3u);
    EXPECT_EQ(Utf16UnitOffset(text, huge), 4u);
    EXPECT_EQ(Utf16ScalarOffset(u"", huge), 0u);
    EXPECT_EQ(Utf16UnitOffset(u"", huge), 0u);
    const std::u16string malformed = u"\xd800x\xdc00";
    EXPECT_EQ(Utf16ScalarOffset(malformed, huge), 3u);
    EXPECT_EQ(Utf16UnitOffset(malformed, 2), 2u);
}
} // namespace
