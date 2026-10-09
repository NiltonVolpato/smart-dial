// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

// A small `{}` formatter: std::format's syntax at a fraction of its size.
// std::format links every formatter it might need, floating point and locale
// included: ~324 KB of flash on the ESP32. This one formats strings and
// integers, plus types that format themselves, and checks the placeholder
// count at compile time.
//
// Syntax: `{}` is the next argument; `{{` and `}}` are literal braces.

#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

namespace coprocessor {

/// What `\n` in a format string becomes. Only the format string's own text is
/// converted, never an argument's. Crlf lets HTTP heads be raw string
/// literals; write either `\n` or `\r\n` in them, not both.
enum class Newlines : uint8_t { Keep, Crlf };

class Writer;

/// A type that formats itself: `void format_to(Writer&) const`.
template <class T>
concept SelfFormatting =
    requires(const T& value, Writer& out) { value.format_to(out); };

/// Integers print in decimal. char and bool are excluded: as text or as
/// numbers would both surprise someone.
template <class T>
concept FormattableInteger =
    std::integral<T> && !std::same_as<T, bool> && !std::same_as<T, char>;

template <class T>
concept Formattable = std::convertible_to<const T&, std::string_view> ||
                      FormattableInteger<T> || SelfFormatting<T>;

namespace detail {

// Not constexpr: calling it from a consteval function is a compile error,
// and its name tells what went wrong.
void invalid_format_string_or_argument_count();

consteval std::size_t count_placeholders(std::string_view text) {
  std::size_t count = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '{') {
      if (i + 1 < text.size() && text[i + 1] == '{') {
        ++i;  // Literal '{'.
      } else if (i + 1 < text.size() && text[i + 1] == '}') {
        ++i;
        ++count;
      } else {
        invalid_format_string_or_argument_count();
      }
    } else if (text[i] == '}') {
      if (i + 1 < text.size() && text[i + 1] == '}') {
        ++i;  // Literal '}'.
      } else {
        invalid_format_string_or_argument_count();
      }
    }
  }
  return count;
}

}  // namespace detail

/// A format string, checked at compile time against its arguments' types.
template <Formattable... Args>
class FormatString {
 public:
  // Implicit, like std::format_string, so literals can be passed directly.
  template <class S>
    requires std::convertible_to<const S&, std::string_view>
  consteval FormatString(const S& text)  // NOLINT(google-explicit-constructor)
      : text_(text) {
    if (detail::count_placeholders(text_) != sizeof...(Args)) {
      detail::invalid_format_string_or_argument_count();
    }
  }

  [[nodiscard]] constexpr std::string_view text() const { return text_; }

 private:
  std::string_view text_;
};

/// Writes into a fixed buffer. Past its end it keeps counting without
/// writing, so size() is what the whole output needs: overflow is
/// size() > capacity, and a Writer over an empty span measures.
class Writer {
 public:
  explicit Writer(std::span<char> buffer, Newlines newlines = Newlines::Keep)
      : buffer_(buffer), newlines_(newlines) {}

  /// Bytes the output needs; more than the buffer holds if it overflowed.
  [[nodiscard]] std::size_t size() const { return size_; }

  void write(std::string_view text) {
    if (size_ < buffer_.size()) {
      std::size_t n = std::min(text.size(), buffer_.size() - size_);
      std::copy_n(text.data(), n, buffer_.data() + size_);
    }
    size_ += text.size();
  }

  template <class... Args>
  void print(FormatString<std::type_identity_t<Args>...> fmt,
             const Args&... args) {
    std::string_view rest = fmt.text();
    // Literal text up to each placeholder, then its argument, in order.
    ((write_literal_until_placeholder(rest), put(args)), ...);
    write_literal_until_placeholder(rest);  // The tail: no placeholder left.
  }

 private:
  // Writes format-string text up to the next `{}`, which it consumes,
  // unescaping braces and converting newlines. FormatString validated the
  // text, so a brace is always followed by its pair.
  void write_literal_until_placeholder(std::string_view& rest) {
    std::size_t start = 0;  // First character not written yet.
    for (std::size_t i = 0; i < rest.size(); ++i) {
      const char c = rest[i];
      if (c == '{' || c == '}') {
        write(rest.substr(start, i - start));
        if (c == '{' && rest[i + 1] == '}') {
          rest.remove_prefix(i + 2);  // A placeholder: stop after it.
          return;
        }
        write(rest.substr(i, 1));  // `{{` or `}}`: one brace.
        ++i;
        start = i + 1;
      } else if (c == '\n' && newlines_ == Newlines::Crlf) {
        write(rest.substr(start, i - start));
        write("\r\n");
        start = i + 1;
      }
    }
    write(rest.substr(start));
    rest = {};
  }

  template <class T>
  void put(const T& value) {
    if constexpr (SelfFormatting<T>) {
      value.format_to(*this);
    } else if constexpr (FormattableInteger<T>) {
      std::array<char, 24> digits;  // Fits any 64-bit integer.
      auto result = std::to_chars(digits.begin(), digits.end(), value);
      write({digits.data(), result.ptr});
    } else {
      write(std::string_view(value));
    }
  }

  std::span<char> buffer_;
  Newlines newlines_;
  std::size_t size_ = 0;
};

}  // namespace coprocessor
