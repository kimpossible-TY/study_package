// Lightweight RFC 1928 SOCKS5 proxy for GCP Windows VM testing.
// Listens on 127.0.0.1:1055 and forwards VNC target port 5900 traffic
// to 127.0.0.1:15900 (SSH reverse tunnel to Mac 127.0.0.1:5900).

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

bool RecvExact(SOCKET s, uint8_t* buf, int len) {
  int got = 0;
  while (got < len) {
    int n = recv(s, reinterpret_cast<char*>(buf + got), len - got, 0);
    if (n <= 0) return false;
    got += n;
  }
  return true;
}

bool SendAll(SOCKET s, const uint8_t* buf, int len) {
  int sent = 0;
  while (sent < len) {
    int n = send(s, reinterpret_cast<const char*>(buf + sent), len - sent, 0);
    if (n <= 0) return false;
    sent += n;
  }
  return true;
}

void TuneSocket(SOCKET s) {
  BOOL nodelay = TRUE;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
  int bufsize = 1024 * 1024;
  setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bufsize), sizeof(bufsize));
  setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bufsize), sizeof(bufsize));
}

SOCKET ConnectTcp(const std::string& host, uint16_t port) {
  struct addrinfo hints {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  char port_str[16];
  std::snprintf(port_str, sizeof(port_str), "%u", static_cast<unsigned>(port));

  struct addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0 || !res) {
    return INVALID_SOCKET;
  }

  SOCKET out = INVALID_SOCKET;
  for (auto* p = res; p != nullptr; p = p->ai_next) {
    SOCKET s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (s == INVALID_SOCKET) continue;
    TuneSocket(s);
    if (connect(s, p->ai_addr, static_cast<int>(p->ai_addrlen)) == 0) {
      out = s;
      break;
    }
    closesocket(s);
  }
  freeaddrinfo(res);
  return out;
}

void PumpHalf(SOCKET src, SOCKET dst) {
  std::vector<uint8_t> buf(65536);
  for (;;) {
    int n = recv(src, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0);
    if (n <= 0) break;
    if (!SendAll(dst, buf.data(), n)) break;
  }
  shutdown(dst, SD_SEND);
}

void HandleClient(SOCKET client, uint16_t tunnel_port) {
  TuneSocket(client);

  uint8_t hdr[2];
  if (!RecvExact(client, hdr, 2) || hdr[0] != 0x05) {
    closesocket(client);
    return;
  }
  uint8_t nmethods = hdr[1];
  std::vector<uint8_t> methods(nmethods);
  if (nmethods > 0 && !RecvExact(client, methods.data(), nmethods)) {
    closesocket(client);
    return;
  }
  const uint8_t auth_reply[2] = {0x05, 0x00};
  if (!SendAll(client, auth_reply, 2)) {
    closesocket(client);
    return;
  }

  uint8_t req[4];
  if (!RecvExact(client, req, 4) || req[0] != 0x05 || req[1] != 0x01) {
    const uint8_t err_reply[10] = {0x05, 0x07, 0x00, 0x01, 0, 0, 0, 0, 0, 0};
    SendAll(client, err_reply, 10);
    closesocket(client);
    return;
  }

  std::string target_host;
  uint8_t atyp = req[3];
  if (atyp == 0x01) {
    uint8_t ipv4[4];
    if (!RecvExact(client, ipv4, 4)) {
      closesocket(client);
      return;
    }
    char ip_buf[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, ipv4, ip_buf, sizeof(ip_buf));
    target_host = ip_buf;
  } else if (atyp == 0x03) {
    uint8_t dlen = 0;
    if (!RecvExact(client, &dlen, 1) || dlen == 0) {
      closesocket(client);
      return;
    }
    std::vector<char> dom(dlen);
    if (!RecvExact(client, reinterpret_cast<uint8_t*>(dom.data()), dlen)) {
      closesocket(client);
      return;
    }
    target_host.assign(dom.data(), dlen);
  } else if (atyp == 0x04) {
    uint8_t ipv6[16];
    if (!RecvExact(client, ipv6, 16)) {
      closesocket(client);
      return;
    }
    char ip_buf[INET6_ADDRSTRLEN] = {};
    inet_ntop(AF_INET6, ipv6, ip_buf, sizeof(ip_buf));
    target_host = ip_buf;
  } else {
    const uint8_t err_reply[10] = {0x05, 0x08, 0x00, 0x01, 0, 0, 0, 0, 0, 0};
    SendAll(client, err_reply, 10);
    closesocket(client);
    return;
  }

  uint8_t port_bytes[2];
  if (!RecvExact(client, port_bytes, 2)) {
    closesocket(client);
    return;
  }
  uint16_t target_port = static_cast<uint16_t>((port_bytes[0] << 8) | port_bytes[1]);

  std::string connect_host = target_host;
  uint16_t connect_port = target_port;
  if (target_port == 5900) {
    connect_host = "127.0.0.1";
    connect_port = tunnel_port;
  }

  SOCKET upstream = ConnectTcp(connect_host, connect_port);
  if (upstream == INVALID_SOCKET) {
    const uint8_t err_reply[10] = {0x05, 0x05, 0x00, 0x01, 0, 0, 0, 0, 0, 0};
    SendAll(client, err_reply, 10);
    closesocket(client);
    return;
  }

  const uint8_t ok_reply[10] = {0x05, 0x00, 0x00, 0x01, 127, 0, 0, 1, 0, 0};
  if (!SendAll(client, ok_reply, 10)) {
    closesocket(upstream);
    closesocket(client);
    return;
  }

  std::thread t(PumpHalf, client, upstream);
  PumpHalf(upstream, client);
  if (t.joinable()) t.join();

  closesocket(upstream);
  closesocket(client);
}

}  // namespace

int main(int argc, char** argv) {
  uint16_t listen_port = 1055;
  uint16_t tunnel_port = 15900;
  if (argc >= 2) listen_port = static_cast<uint16_t>(std::atoi(argv[1]));
  if (argc >= 3) tunnel_port = static_cast<uint16_t>(std::atoi(argv[2]));

  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;

  SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == INVALID_SOCKET) return 1;

  BOOL reuse = TRUE;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

  sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(listen_port);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

  if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    closesocket(listener);
    return 1;
  }
  if (listen(listener, SOMAXCONN) != 0) {
    closesocket(listener);
    return 1;
  }

  std::printf("SOCKS5 bridge listening on 127.0.0.1:%u -> port 5900 mapped to 127.0.0.1:%u\n",
              static_cast<unsigned>(listen_port), static_cast<unsigned>(tunnel_port));
  std::fflush(stdout);

  for (;;) {
    SOCKET client = accept(listener, nullptr, nullptr);
    if (client == INVALID_SOCKET) continue;
    std::thread(HandleClient, client, tunnel_port).detach();
  }
}
