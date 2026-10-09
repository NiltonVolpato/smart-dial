// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

#include "sonos/soap.h"

namespace coprocessor {
namespace {

std::string_view text(const SoapRequest& request) {
  return {request.bytes().data(), request.bytes().size()};
}

std::size_t content_length(std::string_view request) {
  constexpr std::string_view kHeader = "Content-Length: ";
  std::size_t at = request.find(kHeader) + kHeader.size();
  return std::stoul(std::string(request.substr(at, request.find('\r', at))));
}

std::string_view body(std::string_view request) {
  return request.substr(request.find("\r\n\r\n") + 4);
}

TEST(SoapTest, SetVolumeRequest) {
  SoapRequest request = soap_request({.ip = *Ipv4::parse("192.168.1.10"), .port = kSonosPort},
                                     kRenderingControl, "SetVolume",
                                     SetVolumeArgs{.volume = 42});
  ASSERT_TRUE(request.fits());
  EXPECT_EQ(
      text(request),
      "POST /MediaRenderer/RenderingControl/Control HTTP/1.1\r\n"
      "HOST: 192.168.1.10:1400\r\n"
      "Content-Type: text/xml; charset=\"utf-8\"\r\n"
      "SOAPAction: \"urn:schemas-upnp-org:service:RenderingControl:1#"
      "SetVolume\"\r\n"
      "Content-Length: 323\r\n"
      "Connection: close\r\n\r\n"
      "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
      "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
      "<u:SetVolume xmlns:u=\"urn:schemas-upnp-org:service:RenderingControl:"
      "1\"><InstanceID>0</InstanceID><Channel>Master</Channel>"
      "<DesiredVolume>42</DesiredVolume></u:SetVolume></s:Body></s:Envelope>");
}

TEST(SoapTest, ContentLengthMatchesBody) {
  SoapRequest request = soap_request({.ip = *Ipv4::parse("10.0.0.1"), .port = kSonosPort},
                                     kAvTransport, "Play", PlayArgs{});
  ASSERT_TRUE(request.fits());
  EXPECT_EQ(content_length(text(request)), body(text(request)).size());
}

TEST(SoapTest, LongestRequestFits) {
  SoapRequest request =
      soap_request({.ip = Ipv4{0xFFFFFFFF}, .port = 65535}, kRenderingControl,
                   "SetRelativeVolume", SetRelativeVolumeArgs{-32768});
  EXPECT_TRUE(request.fits()) << text(request).size();
}

}  // namespace
}  // namespace coprocessor
