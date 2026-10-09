// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "format.h"

namespace coprocessor {
namespace {

// Formats into a large buffer and returns the text.
template <class... Args>
std::string format(Newlines newlines,
                   FormatString<std::type_identity_t<Args>...> fmt,
                   const Args&... args) {
  std::array<char, 256> buffer{};
  Writer out(buffer, newlines);
  out.print(fmt, args...);
  return {buffer.data(), out.size()};
}

template <class... Args>
std::string format(FormatString<std::type_identity_t<Args>...> fmt,
                   const Args&... args) {
  return format<Args...>(Newlines::Keep, fmt, args...);
}

struct Point {
  int x;
  int y;
  void format_to(Writer& out) const { out.print("({}, {})", x, y); }
};

TEST(FormatTest, Strings) {
  std::string owned = "owned";
  EXPECT_EQ(format("{} {} {}", "literal", std::string_view("view"), owned),
            "literal view owned");
}

TEST(FormatTest, Integers) {
  EXPECT_EQ(format("{} {} {}", 0, -42, 1400U), "0 -42 1400");
  EXPECT_EQ(format("{}", uint8_t{255}), "255");  // A number, not a char.
  EXPECT_EQ(format("{}", std::numeric_limits<int64_t>::min()),
            "-9223372036854775808");
}

TEST(FormatTest, SelfFormattingTypesNest) {
  EXPECT_EQ(format("at {}!", Point{1, -2}), "at (1, -2)!");
}

TEST(FormatTest, EscapedBraces) {
  EXPECT_EQ(format("{{}} {{{}}}", 7), "{} {7}");
}

TEST(FormatTest, PlaceholdersAtEdges) {
  EXPECT_EQ(format("{}", "only"), "only");
  EXPECT_EQ(format("{}{}", 1, 2), "12");
  EXPECT_EQ(format("no placeholders"), "no placeholders");
  EXPECT_EQ(format(""), "");
}

TEST(FormatTest, CrlfConvertsOnlyTheFormatString) {
  EXPECT_EQ(format(Newlines::Crlf, "a\nb: {}\n\n", "x\ny"),
            "a\r\nb: x\ny\r\n\r\n");
  EXPECT_EQ(format(Newlines::Keep, "a\n{}", 1), "a\n1");
}

TEST(FormatTest, OverflowKeepsCounting) {
  std::array<char, 4> buffer{};
  Writer out(buffer);
  out.print("{}-{}", "abc", 12345);
  EXPECT_EQ(out.size(), 9U);  // What the whole output needs.
  EXPECT_EQ(std::string_view(buffer.data(), buffer.size()), "abc-");
}

TEST(FormatTest, EmptyBufferMeasures) {
  Writer out({});
  out.print("{}:{}", "127.0.0.1", 1400);
  EXPECT_EQ(out.size(), 14U);
}

// Compile-time checks: each of these fails to compile, as it should.
//   format("{}");             // Too few arguments.
//   format("{}", 1, 2);       // Too many.
//   format("{", 1);           // Unmatched brace.
//   format("{0}", 1);         // Positional arguments aren't supported.
//   format("{}", 1.5);        // Floating point isn't formattable.
//   format("{}", true);       // Neither is bool.

}  // namespace
}  // namespace coprocessor
