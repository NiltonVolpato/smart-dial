// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#include <netinet/in.h>

#include <gtest/gtest.h>

#include <optional>

#include "net/ipv4.h"

namespace coprocessor {
namespace {

TEST(Ipv4Test, ParsesDottedQuadAsANumber) {
  // a.b.c.d is (a << 24) | (b << 16) | (c << 8) | d, as the schemas say.
  EXPECT_EQ(Ipv4::parse("192.168.1.10"), Ipv4{0xC0A8010A});
  EXPECT_EQ(Ipv4::parse("0.0.0.1"), Ipv4{1});
  EXPECT_EQ(Ipv4::parse("255.255.255.255"), Ipv4{0xFFFFFFFF});
}

TEST(Ipv4Test, RejectsWhatIsNotAnAddress) {
  EXPECT_EQ(Ipv4::parse(""), std::nullopt);
  EXPECT_EQ(Ipv4::parse("192.168.1"), std::nullopt);
  EXPECT_EQ(Ipv4::parse("192.168.1.256"), std::nullopt);
  EXPECT_EQ(Ipv4::parse("speaker.local"), std::nullopt);
  EXPECT_EQ(Ipv4::parse("1234567890.1234567890"), std::nullopt);  // Too long.
}

TEST(Ipv4Test, FormatsAsDottedQuad) {
  EXPECT_EQ(Ipv4{0xC0A8010A}.to_string(), "192.168.1.10");
  EXPECT_EQ(Ipv4{0xFFFFFFFF}.to_string(), "255.255.255.255");
  EXPECT_EQ(Ipv4{}.to_string(), "0.0.0.0");
}

TEST(Ipv4Test, ConvertsToNetworkOrder) {
  EXPECT_EQ(Ipv4{INADDR_LOOPBACK}.to_in_addr().s_addr, htonl(INADDR_LOOPBACK));
}

}  // namespace
}  // namespace coprocessor
