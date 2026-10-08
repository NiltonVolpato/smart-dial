// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#include <stddef.h>
#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "sonos_http.h"
#include "sonos_parser.h"

namespace coprocessor {
namespace {

std::string load_fixture(const std::string& filename) {
  std::string path = std::string(SONOS_FIXTURES_DIR) + "/" + filename;
  std::ifstream file(path);
  if (!file.is_open()) {
    ADD_FAILURE() << "Failed to open fixture: " << path;
    return "";
  }
  std::stringstream ss;
  ss << file.rdbuf();
  return ss.str();
}

// Lightweight TCP mock server for testing localhost socket communication
class MockSonosServer {
 public:
  MockSonosServer() {
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  // let OS assign an ephemeral port
    bind(server_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    listen(server_fd_, 1);

    socklen_t len = sizeof(addr);
    getsockname(server_fd_, reinterpret_cast<struct sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
  }

  ~MockSonosServer() {
    stop();
    if (server_fd_ >= 0) {
      close(server_fd_);
    }
  }

  uint16_t port() const { return port_; }

  void handle_one_request(
      const std::function<std::string(const std::string&)>& handler) {
    worker_ = std::thread([this, handler]() {
      struct sockaddr_in client_addr = {};
      socklen_t client_len = sizeof(client_addr);
      int client_fd = accept(server_fd_,
                             reinterpret_cast<struct sockaddr*>(&client_addr),
                             &client_len);
      if (client_fd < 0) {
        return;
      }

      std::string req;
      std::array<char, 1024> buf = {};
      while (req.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = recv(client_fd, buf.data(), buf.size(), 0);
        if (n <= 0) {
          break;
        }
        req.append(buf.data(), static_cast<size_t>(n));
      }

      std::string response = handler(req);
      send(client_fd, response.data(), response.size(), 0);
      close(client_fd);
    });
  }

  void stop() {
    if (worker_.joinable()) {
      worker_.join();
    }
  }

 private:
  int server_fd_ = -1;
  uint16_t port_ = 0;
  std::thread worker_;
};

TEST(SonosSocketTest, SoapRequestSuccess) {
  MockSonosServer server;
  std::string fixture = load_fixture("get_position_info_playing.xml");
  ASSERT_FALSE(fixture.empty());

  std::atomic<bool> saw_soap_action = false;
  server.handle_one_request([&](const std::string& req) -> std::string {
    if (req.find("SOAPAction: \"urn:schemas-upnp-org:service:AVTransport:1#GetPositionInfo\"") !=
        std::string::npos) {
      saw_soap_action = true;
    }
    std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: text/xml\r\nContent-Length: ";
    resp += std::to_string(fixture.size());
    resp += "\r\nConnection: close\r\n\r\n";
    resp += fixture;
    return resp;
  });

  std::string body;
  int status_code = 0;
  bool ok = soap_request_tcp(
      "127.0.0.1", server.port(), "/MediaRenderer/AVTransport/Control",
      "urn:schemas-upnp-org:service:AVTransport:1", "GetPositionInfo",
      "<InstanceID>0</InstanceID>",
      [&body](const char* chunk, size_t len) {
        body.append(chunk, len);
        return true;
      },
      &status_code);

  server.stop();

  EXPECT_TRUE(ok);
  EXPECT_EQ(status_code, 200);
  EXPECT_TRUE(saw_soap_action);
  EXPECT_EQ(body, fixture);

  SonosTrackMetadata meta;
  EXPECT_TRUE(parse_position_info(body, &meta));
  EXPECT_EQ(meta.title, "The Rope (feat. POLIÇA)");
}

TEST(SonosSocketTest, SoapRequest500Error) {
  MockSonosServer server;
  server.handle_one_request([](const std::string&) -> std::string {
    return "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n";
  });

  std::string body;
  int status_code = 0;
  bool ok = soap_request_tcp(
      "127.0.0.1", server.port(), "/MediaRenderer/AVTransport/Control",
      "urn:schemas-upnp-org:service:AVTransport:1", "GetPositionInfo",
      "<InstanceID>0</InstanceID>",
      [&body](const char* chunk, size_t len) {
        body.append(chunk, len);
        return true;
      },
      &status_code);

  server.stop();

  EXPECT_FALSE(ok);
  EXPECT_EQ(status_code, 500);
}

TEST(SonosSocketTest, GenaSubscribeSuccess) {
  MockSonosServer server;
  server.handle_one_request([](const std::string& req) -> std::string {
    EXPECT_NE(req.find("SUBSCRIBE /MediaRenderer/AVTransport/Event"), std::string::npos);
    EXPECT_NE(req.find("NT: upnp:event"), std::string::npos);
    return "HTTP/1.1 200 OK\r\n"
           "SID: uuid:RINCON_00000000000101400_sub0000000001\r\n"
           "TIMEOUT: Second-1800\r\n\r\n";
  });

  std::string req =
      "SUBSCRIBE /MediaRenderer/AVTransport/Event HTTP/1.1\r\n"
      "HOST: 127.0.0.1\r\n"
      "CALLBACK: <http://127.0.0.1:80/notify>\r\n"
      "NT: upnp:event\r\n"
      "TIMEOUT: Second-1800\r\n\r\n";

  GenaResponse resp;
  bool ok = gena_request_tcp("127.0.0.1", server.port(), req, &resp);
  server.stop();

  EXPECT_TRUE(ok);
  EXPECT_EQ(resp.status_code, 200);
  EXPECT_EQ(resp.sid, "uuid:RINCON_00000000000101400_sub0000000001");
  EXPECT_EQ(resp.timeout_seconds, 1800u);
}

TEST(SonosSocketTest, Gena412PreconditionFailed) {
  MockSonosServer server;
  server.handle_one_request([](const std::string&) -> std::string {
    return "HTTP/1.1 412 Precondition Failed\r\n\r\n";
  });

  std::string req =
      "SUBSCRIBE /MediaRenderer/AVTransport/Event HTTP/1.1\r\n"
      "HOST: 127.0.0.1\r\n"
      "SID: uuid:stale_sid\r\n"
      "TIMEOUT: Second-1800\r\n\r\n";

  GenaResponse resp;
  bool ok = gena_request_tcp("127.0.0.1", server.port(), req, &resp);
  server.stop();

  EXPECT_TRUE(ok);
  EXPECT_EQ(resp.status_code, 412);
}

TEST(SonosSocketTest, ConnectFailure) {
  // Connect to an unused port (port 1 is usually rejected immediately)
  std::string body;
  int status_code = 0;
  bool ok = soap_request_tcp(
      "127.0.0.1", 1, "/test", "urn:test", "test", "",
      [](const char*, size_t) { return true; }, &status_code);

  EXPECT_FALSE(ok);
  EXPECT_EQ(status_code, 0);
}

TEST(SonosSocketTest, StreamingServerReceive18KB) {
  // Simulate web server receiving an 18,911-byte NOTIFY request over a socket
  int fds[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

  std::string fixture = load_fixture("notify_zone_group_topology.xml");
  ASSERT_EQ(fixture.size(), 18911u);

  std::thread sender([fds, &fixture]() {
    std::string req = "NOTIFY /media/notify HTTP/1.1\r\n";
    req += "Host: 127.0.0.1:80\r\n";
    req += "Content-Type: text/xml\r\n";
    req += "Content-Length: " + std::to_string(fixture.size()) + "\r\n\r\n";
    req += fixture;

    size_t sent = 0;
    while (sent < req.size()) {
      ssize_t n = send(fds[0], req.data() + sent, req.size() - sent, 0);
      if (n <= 0) break;
      sent += n;
    }
    close(fds[0]);
  });

  // Receiver simulates web_server.cpp reading headers and streaming body chunks
  std::string headers;
  std::array<char, 512> buf = {};
  while (headers.find("\r\n\r\n") == std::string::npos) {
    ssize_t n = recv(fds[1], buf.data(), buf.size(), 0);
    ASSERT_GT(n, 0);
    headers.append(buf.data(), n);
  }

  size_t header_end = headers.find("\r\n\r\n");
  ASSERT_NE(header_end, std::string::npos);

  StreamingNotifyParser parser;
  size_t body_start = header_end + 4;
  size_t total_received = 0;
  if (body_start < headers.size()) {
    size_t initial = headers.size() - body_start;
    total_received += initial;
    ASSERT_TRUE(parser.feed_chunk(headers.data() + body_start, initial,
                                  total_received >= fixture.size()));
  }

  while (total_received < fixture.size()) {
    ssize_t n = recv(fds[1], buf.data(), buf.size(), 0);
    if (n <= 0) break;
    total_received += n;
    ASSERT_TRUE(parser.feed_chunk(buf.data(), n,
                                  total_received >= fixture.size()));
  }
  close(fds[1]);
  sender.join();

  EXPECT_EQ(total_received, fixture.size());

  NotifyResult result;
  ASSERT_TRUE(parser.finish(&result));
  EXPECT_TRUE(result.zone_group_state);
}

}  // namespace
}  // namespace coprocessor
