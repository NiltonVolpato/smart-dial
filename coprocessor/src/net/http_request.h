// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <type_traits>

#include "format.h"

namespace coprocessor {

/// A request formatted in place into a fixed buffer: no heap. `N` bounds the
/// request: anything longer is a bug, reported by fits().
template <std::size_t N>
class FixedRequest {
 public:
  /// Formats more of the request. `\n` in `fmt` becomes `\r\n`, so heads can
  /// be raw string literals.
  template <class... Args>
  void append(FormatString<std::type_identity_t<Args>...> fmt,
              const Args&... args) {
    Writer out(std::span<char>(buffer_).subspan(std::min(size_, N)),
               Newlines::Crlf);
    out.print(fmt, args...);
    size_ += out.size();
  }

  /// The size append(fmt, args...) would add, e.g. for a Content-Length.
  template <class... Args>
  static std::size_t measure(FormatString<std::type_identity_t<Args>...> fmt,
                             const Args&... args) {
    Writer out({}, Newlines::Crlf);
    out.print(fmt, args...);
    return out.size();
  }

  /// False if the request didn't fit: it must not be sent.
  [[nodiscard]] bool fits() const { return size_ <= N; }

  [[nodiscard]] std::span<const char> bytes() const {
    return {buffer_.data(), std::min(size_, N)};
  }

 private:
  std::array<char, N> buffer_;
  std::size_t size_ = 0;  // May exceed N: the size it needed.
};

}  // namespace coprocessor
