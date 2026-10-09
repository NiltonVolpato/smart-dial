// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "format.h"

namespace coprocessor {

/// An IPv4 address as the schemas carry it: a number where a.b.c.d is
/// (a << 24) | (b << 16) | (c << 8) | d. 0 means none.
struct Ipv4 {
  uint32_t value = 0;

  /// Parses dotted-quad text, e.g. from a UPnP Location URL.
  static std::optional<Ipv4> parse(std::string_view dotted) {
    std::array<char, INET_ADDRSTRLEN> text{};  // inet_pton wants a C string.
    if (dotted.size() >= text.size()) {
      return std::nullopt;
    }
    std::ranges::copy(dotted, text.begin());
    in_addr addr{};
    if (inet_pton(AF_INET, text.data(), &addr) != 1) {
      return std::nullopt;
    }
    return Ipv4{ntohl(addr.s_addr)};
  }

  [[nodiscard]] in_addr to_in_addr() const { return {.s_addr = htonl(value)}; }

  /// Dotted-quad text. Prefer formatting it in place: `out.print("{}", ip)`.
  [[nodiscard]] std::string to_string() const {
    std::array<char, INET_ADDRSTRLEN> text{};
    Writer out(text);
    format_to(out);
    return {text.data(), std::min(out.size(), text.size())};
  }

  void format_to(Writer& out) const {
    out.print("{}.{}.{}.{}", value >> 24, (value >> 16) & 0xFF,
              (value >> 8) & 0xFF, value & 0xFF);
  }

  friend bool operator==(Ipv4, Ipv4) = default;
};

}  // namespace coprocessor
