// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

// UPnP SOAP requests, formatted in place into fixed buffers.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "format.h"

#include "net/http_client.h"
#include "net/http_request.h"

namespace coprocessor {

struct SoapService {
  std::string_view urn;
  std::string_view control_path;
};

inline constexpr SoapService kAvTransport{
    .urn = "urn:schemas-upnp-org:service:AVTransport:1",
    .control_path = "/MediaRenderer/AVTransport/Control",
};
inline constexpr SoapService kRenderingControl{
    .urn = "urn:schemas-upnp-org:service:RenderingControl:1",
    .control_path = "/MediaRenderer/RenderingControl/Control",
};

// Arguments of an action: each formats the children of its <u:Action>.

struct InstanceArgs {
  void format_to(Writer& out) const { out.print("<InstanceID>0</InstanceID>"); }
};

struct PlayArgs {
  void format_to(Writer& out) const {
    out.print("<InstanceID>0</InstanceID><Speed>1</Speed>");
  }
};

struct SetVolumeArgs {
  uint8_t volume;  // 0..100
  void format_to(Writer& out) const {
    out.print(
        "<InstanceID>0</InstanceID><Channel>Master</Channel>"
        "<DesiredVolume>{}</DesiredVolume>",
        volume);
  }
};

struct SetRelativeVolumeArgs {
  int16_t adjustment;
  void format_to(Writer& out) const {
    out.print(
        "<InstanceID>0</InstanceID><Channel>Master</Channel>"
        "<Adjustment>{}</Adjustment>",
        adjustment);
  }
};

/// Fits every request below, with room for the longest IPv4 address.
inline constexpr std::size_t kSoapRequestSize = 768;
using SoapRequest = FixedRequest<kSoapRequestSize>;

// Placeholders: action, service URN, arguments, action again.
inline constexpr std::string_view kSoapEnvelope =
    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
    "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
    "<u:{} xmlns:u=\"{}\">{}</u:{}></s:Body></s:Envelope>";

/// POSTs `action` of `service` to `endpoint`. Check fits() before sending.
template <SelfFormatting Args>
SoapRequest soap_request(const Endpoint& endpoint, const SoapService& service,
                         std::string_view action, const Args& args) {
  SoapRequest request;
  request.append(
      R"(POST {} HTTP/1.1
HOST: {}:{}
Content-Type: text/xml; charset="utf-8"
SOAPAction: "{}#{}"
Content-Length: {}
Connection: close

)",
      service.control_path, endpoint.ip, endpoint.port, service.urn, action,
      // Content-Length precedes the body, so measure it first.
      SoapRequest::measure(kSoapEnvelope, action, service.urn, args, action));
  request.append(kSoapEnvelope, action, service.urn, args, action);
  return request;
}

}  // namespace coprocessor
