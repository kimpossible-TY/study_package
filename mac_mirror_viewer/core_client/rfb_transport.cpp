#include "rfb_transport.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace mac_mirror {

namespace {

constexpr size_t kRxBufferSize = 65536;
constexpr int kAbortSliceMs = 50;
constexpr int kTargetRecvBufBytes = 1024 * 1024;  // 1 MB SO_RCVBUF

#ifdef _WIN32
bool EnsureWinsockInitialized(std::string* error_out) {
    static const int kInitResult = []() -> int {
        WSADATA wsa{};
        return WSAStartup(MAKEWORD(2, 2), &wsa);
    }();
    if (kInitResult != 0 && error_out) {
        *error_out = "WSAStartup failed with code " + std::to_string(kInitResult);
    }
    return kInitResult == 0;
}

void CloseRawSocket(SOCKET s) noexcept {
    if (s != INVALID_SOCKET) {
        ::closesocket(s);
    }
}

bool SetNonBlocking(SOCKET s, bool non_blocking) noexcept {
    u_long mode = non_blocking ? 1UL : 0UL;
    return ::ioctlsocket(s, FIONBIO, &mode) == 0;
}
#else
bool EnsureWinsockInitialized(std::string*) { return true; }

void CloseRawSocket(int fd) noexcept {
    if (fd >= 0) {
        ::close(fd);
    }
}

bool SetNonBlocking(int fd, bool non_blocking) noexcept {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    int updated = non_blocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return ::fcntl(fd, F_SETFL, updated) == 0;
}
#endif

bool ParseUint16Port(const std::string& text, uint16_t* out_port) {
    if (text.empty() || text.size() > 5) {
        return false;
    }
    uint32_t value = 0;
    for (char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        value = value * 10u + static_cast<uint32_t>(ch - '0');
        if (value > 65535u) {
            return false;
        }
    }
    if (value == 0u) {
        return false;
    }
    *out_port = static_cast<uint16_t>(value);
    return true;
}

std::string StripIpv6Brackets(const std::string& host) {
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        return host.substr(1, host.size() - 2);
    }
    return host;
}

}  // namespace

bool ParseHostPortSpec(const std::string& spec,
                       uint16_t default_port,
                       Endpoint* out_endpoint,
                       std::string* error_out) {
    if (!out_endpoint) {
        if (error_out) *error_out = "Null output endpoint";
        return false;
    }
    // Trim leading/trailing ASCII whitespace
    size_t first = spec.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        if (error_out) *error_out = "Empty endpoint specification";
        return false;
    }
    size_t last = spec.find_last_not_of(" \t\r\n");
    std::string trimmed = spec.substr(first, last - first + 1);

    std::string host;
    uint16_t port = default_port;

    if (trimmed.front() == '[') {
        size_t close_bracket = trimmed.find(']');
        if (close_bracket == std::string::npos || close_bracket == 1) {
            if (error_out) *error_out = "Malformed bracketed IPv6 endpoint: " + trimmed;
            return false;
        }
        host = trimmed.substr(1, close_bracket - 1);
        if (close_bracket + 1 < trimmed.size()) {
            if (trimmed[close_bracket + 1] != ':') {
                if (error_out) *error_out = "Invalid suffix after IPv6 address: " + trimmed;
                return false;
            }
            if (!ParseUint16Port(trimmed.substr(close_bracket + 2), &port)) {
                if (error_out) *error_out = "Invalid port in endpoint: " + trimmed;
                return false;
            }
        } else if (port == 0) {
            if (error_out) *error_out = "Missing port in endpoint: " + trimmed;
            return false;
        }
    } else {
        size_t first_colon = trimmed.find(':');
        size_t last_colon = trimmed.rfind(':');
        if (first_colon != std::string::npos && first_colon == last_colon) {
            host = trimmed.substr(0, first_colon);
            if (!ParseUint16Port(trimmed.substr(first_colon + 1), &port)) {
                if (error_out) *error_out = "Invalid port in endpoint: " + trimmed;
                return false;
            }
        } else {
            // Bare host or unbracketed IPv6 literal with default_port
            host = trimmed;
            if (port == 0) {
                if (error_out) *error_out = "Missing port in endpoint: " + trimmed;
                return false;
            }
        }
    }

    if (host.empty()) {
        if (error_out) *error_out = "Empty host in endpoint: " + trimmed;
        return false;
    }

    out_endpoint->host = std::move(host);
    out_endpoint->port = port;
    return true;
}

bool BuildSocks5ConnectRequest(const std::string& target_host,
                               uint16_t target_port,
                               std::vector<uint8_t>* out_packet,
                               std::string* error_out) {
    if (!out_packet) {
        if (error_out) *error_out = "Null output buffer for SOCKS5 packet";
        return false;
    }
    if (target_port == 0) {
        if (error_out) *error_out = "Invalid target port 0 for SOCKS5";
        return false;
    }

    std::string clean_host = StripIpv6Brackets(target_host);
    if (clean_host.empty()) {
        if (error_out) *error_out = "Empty target host for SOCKS5";
        return false;
    }

    if (!EnsureWinsockInitialized(error_out)) {
        return false;
    }

    out_packet->clear();
    out_packet->reserve(32);
    out_packet->push_back(0x05);  // VER = 5
    out_packet->push_back(0x01);  // CMD = CONNECT
    out_packet->push_back(0x00);  // RSV = 0

    in_addr ipv4{};
    in6_addr ipv6{};
    if (::inet_pton(AF_INET, clean_host.c_str(), &ipv4) == 1) {
        out_packet->push_back(0x01);  // ATYP = IPv4
        const auto* raw = reinterpret_cast<const uint8_t*>(&ipv4);
        out_packet->insert(out_packet->end(), raw, raw + 4);
    } else if (::inet_pton(AF_INET6, clean_host.c_str(), &ipv6) == 1) {
        out_packet->push_back(0x04);  // ATYP = IPv6
        const auto* raw = reinterpret_cast<const uint8_t*>(&ipv6);
        out_packet->insert(out_packet->end(), raw, raw + 16);
    } else {
        if (clean_host.size() > 255) {
            if (error_out) *error_out = "SOCKS5 domain name exceeds 255 bytes";
            return false;
        }
        out_packet->push_back(0x03);  // ATYP = DOMAINNAME
        out_packet->push_back(static_cast<uint8_t>(clean_host.size()));
        out_packet->insert(out_packet->end(), clean_host.begin(), clean_host.end());
    }

    out_packet->push_back(static_cast<uint8_t>((target_port >> 8) & 0xFF));
    out_packet->push_back(static_cast<uint8_t>(target_port & 0xFF));
    return true;
}

std::string Socks5ReplyErrorString(uint8_t reply_code) {
    switch (reply_code) {
        case 0x00: return "succeeded";
        case 0x01: return "general SOCKS server failure";
        case 0x02: return "connection not allowed by ruleset";
        case 0x03: return "network unreachable";
        case 0x04: return "host unreachable";
        case 0x05: return "connection refused";
        case 0x06: return "TTL expired";
        case 0x07: return "command not supported";
        case 0x08: return "address type not supported";
        default:   return "unknown SOCKS5 reply code " + std::to_string(reply_code);
    }
}

RfbTransport::RfbTransport() : rx_buf_(kRxBufferSize, 0) {}

RfbTransport::~RfbTransport() {
    Close();
}

bool RfbTransport::IsConnected() const noexcept {
    std::lock_guard<std::mutex> lock(sock_mutex_);
    return sock_ != kInvalidSocket && !aborted_.load(std::memory_order_acquire);
}

void RfbTransport::Abort() noexcept {
    aborted_.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lock(sock_mutex_);
    if (sock_ != kInvalidSocket) {
#ifdef _WIN32
        SOCKET s = static_cast<SOCKET>(sock_);
        ::shutdown(s, SD_BOTH);
        ::closesocket(s);
        sock_ = kInvalidSocket;
#else
        int fd = static_cast<int>(sock_);
        ::shutdown(fd, SHUT_RDWR);
#endif
    }
}

void RfbTransport::Close() noexcept {
    aborted_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(sock_mutex_);
        if (sock_ != kInvalidSocket) {
#ifdef _WIN32
            SOCKET s = static_cast<SOCKET>(sock_);
            ::shutdown(s, SD_BOTH);
            ::closesocket(s);
#else
            int fd = static_cast<int>(sock_);
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
#endif
            sock_ = kInvalidSocket;
        }
    }
    rx_head_ = 0;
    rx_tail_ = 0;
}

void RfbTransport::ApplyLowLatencySocketOptions(NativeSocket sock) noexcept {
    if (sock == kInvalidSocket) return;
#ifdef _WIN32
    SOCKET s = static_cast<SOCKET>(sock);
    BOOL nodelay = TRUE;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
    int rcvbuf = kTargetRecvBufBytes;
    ::setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));
    BOOL keepalive = TRUE;
    ::setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&keepalive), sizeof(keepalive));
#else
    int fd = static_cast<int>(sock);
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    int rcvbuf = kTargetRecvBufBytes;
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) != 0) {
        int fallback = 512 * 1024;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &fallback, sizeof(fallback));
    }
    ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
#endif
}

int RfbTransport::PollSocket(NativeSocket sock,
                             bool for_write,
                             int timeout_ms,
                             std::string* error_out) {
    if (aborted_.load(std::memory_order_acquire) || sock == kInvalidSocket) {
        if (error_out) *error_out = "Connection aborted";
        return -1;
    }

#ifdef _WIN32
    SOCKET s = static_cast<SOCKET>(sock);
    fd_set rfds;
    fd_set wfds;
    fd_set efds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_ZERO(&efds);
    if (for_write) {
        FD_SET(s, &wfds);
    } else {
        FD_SET(s, &rfds);
    }
    FD_SET(s, &efds);

    timeval tv{};
    if (timeout_ms > 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
    }
    int rc = ::select(0, for_write ? nullptr : &rfds, for_write ? &wfds : nullptr, &efds,
                      timeout_ms < 0 ? nullptr : &tv);
    if (rc < 0) {
        if (error_out) {
            *error_out = "Socket select failed (" + std::to_string(WSAGetLastError()) + ")";
        }
        return -1;
    }
    if (rc == 0) {
        return 0;
    }
    if (FD_ISSET(s, &efds)) {
        int so_err = 0;
        int optlen = sizeof(so_err);
        ::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_err), &optlen);
        if (error_out) {
            *error_out = "Socket error (" + std::to_string(so_err) + ")";
        }
        return -1;
    }
    return 1;
#else
    pollfd pfd{};
    pfd.fd = static_cast<int>(sock);
    pfd.events = for_write ? POLLOUT : POLLIN;
    int rc = ::poll(&pfd, 1, timeout_ms);
    if (rc < 0) {
        if (errno == EINTR) {
            return 0;
        }
        if (error_out) {
            *error_out = std::string("poll failed: ") + std::strerror(errno);
        }
        return -1;
    }
    if (rc == 0) {
        return 0;
    }
    if ((pfd.revents & (POLLERR | POLLNVAL)) != 0) {
        int so_err = 0;
        socklen_t optlen = sizeof(so_err);
        ::getsockopt(pfd.fd, SOL_SOCKET, SO_ERROR, &so_err, &optlen);
        if (error_out) {
            *error_out = std::string("Socket error: ") +
                         (so_err ? std::strerror(so_err) : "POLLERR");
        }
        return -1;
    }
    return 1;
#endif
}

bool RfbTransport::OpenTcpSocket(const std::string& host,
                                 uint16_t port,
                                 int timeout_ms,
                                 std::string* error_out) {
    Close();
    aborted_.store(false, std::memory_order_release);
    bytes_received_.store(0, std::memory_order_relaxed);
    rx_head_ = 0;
    rx_tail_ = 0;

    if (!EnsureWinsockInitialized(error_out)) {
        return false;
    }

    std::string clean_host = StripIpv6Brackets(host);
    if (clean_host.empty() || port == 0) {
        if (error_out) *error_out = "Invalid TCP target host or port";
        return false;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* res = nullptr;
    std::string port_str = std::to_string(port);
    int gai = ::getaddrinfo(clean_host.c_str(), port_str.c_str(), &hints, &res);
    if (gai != 0 || !res) {
        if (error_out) {
            *error_out = "Failed to resolve host '" + clean_host + ":" + port_str + "'";
        }
        return false;
    }

    auto start_tp = std::chrono::steady_clock::now();
    std::string last_err = "Could not connect to " + clean_host + ":" + port_str;
    bool connected = false;

    for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        if (aborted_.load(std::memory_order_acquire)) {
            last_err = "Connection aborted";
            break;
        }

#ifdef _WIN32
        SOCKET raw = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (raw == INVALID_SOCKET) {
            continue;
        }
        NativeSocket candidate = static_cast<NativeSocket>(raw);
#else
        int raw = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (raw < 0) {
            continue;
        }
        NativeSocket candidate = static_cast<NativeSocket>(raw);
#endif
        ApplyLowLatencySocketOptions(candidate);
        SetNonBlocking(raw, true);

        int rc = ::connect(raw, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
        bool in_progress = false;
        if (rc != 0) {
#ifdef _WIN32
            int werr = WSAGetLastError();
            in_progress = (werr == WSAEWOULDBLOCK || werr == WSAEINPROGRESS);
#else
            in_progress = (errno == EINPROGRESS || errno == EALREADY);
#endif
        }

        if (rc == 0) {
            SetNonBlocking(raw, false);
            std::lock_guard<std::mutex> lock(sock_mutex_);
            if (aborted_.load(std::memory_order_acquire)) {
                CloseRawSocket(raw);
                last_err = "Connection aborted";
                break;
            }
            sock_ = candidate;
            connected = true;
            break;
        }

        if (!in_progress) {
            CloseRawSocket(raw);
            continue;
        }

        while (true) {
            auto elapsed_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start_tp)
                    .count());
            int remaining_ms = timeout_ms - elapsed_ms;
            if (remaining_ms <= 0) {
                last_err = "Timed out connecting to " + clean_host + ":" + port_str;
                break;
            }
            int slice_ms = std::min(remaining_ms, kAbortSliceMs);
            std::string poll_err;
            int pr = PollSocket(candidate, true, slice_ms, &poll_err);
            if (pr < 0) {
                last_err = poll_err.empty() ? last_err : poll_err;
                break;
            }
            if (pr == 0) {
                continue;
            }

            int so_err = 0;
#ifdef _WIN32
            int optlen = sizeof(so_err);
            ::getsockopt(raw, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_err), &optlen);
#else
            socklen_t optlen = sizeof(so_err);
            ::getsockopt(raw, SOL_SOCKET, SO_ERROR, &so_err, &optlen);
#endif
            if (so_err == 0) {
                SetNonBlocking(raw, false);
                std::lock_guard<std::mutex> lock(sock_mutex_);
                if (aborted_.load(std::memory_order_acquire)) {
                    last_err = "Connection aborted";
                    break;
                }
                sock_ = candidate;
                connected = true;
            } else {
                last_err = "TCP connect to " + clean_host + ":" + port_str + " refused/failed";
            }
            break;
        }

        if (connected) {
            break;
        }
        CloseRawSocket(raw);
    }

    ::freeaddrinfo(res);
    if (!connected && error_out) {
        *error_out = last_err;
    }
    return connected;
}

bool RfbTransport::ConnectDirect(const std::string& host,
                                 uint16_t port,
                                 int timeout_ms,
                                 std::string* error_out) {
    return OpenTcpSocket(host, port, timeout_ms, error_out);
}

bool RfbTransport::ReadExactWithDeadline(void* dst,
                                         size_t len,
                                         int timeout_ms,
                                         std::string* error_out) {
    auto start_tp = std::chrono::steady_clock::now();
    auto* out = static_cast<uint8_t*>(dst);
    size_t offset = 0;

    while (offset < len) {
        if (rx_head_ < rx_tail_) {
            size_t avail = rx_tail_ - rx_head_;
            size_t take = std::min(avail, len - offset);
            std::memcpy(out + offset, rx_buf_.data() + rx_head_, take);
            rx_head_ += take;
            if (rx_head_ == rx_tail_) {
                rx_head_ = 0;
                rx_tail_ = 0;
            }
            offset += take;
            continue;
        }

        auto elapsed_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start_tp)
                .count());
        int remaining_ms = timeout_ms - elapsed_ms;
        if (remaining_ms <= 0) {
            if (error_out) *error_out = "Timed out waiting for proxy/server handshake";
            return false;
        }

        int wr = WaitReadable(std::min(remaining_ms, kAbortSliceMs), error_out);
        if (wr < 0) {
            return false;
        }
        if (wr == 0) {
            continue;
        }

        NativeSocket s = kInvalidSocket;
        {
            std::lock_guard<std::mutex> lock(sock_mutex_);
            s = sock_;
        }
        if (s == kInvalidSocket || aborted_.load(std::memory_order_acquire)) {
            if (error_out) *error_out = "Connection closed";
            return false;
        }

#ifdef _WIN32
        int got = ::recv(static_cast<SOCKET>(s),
                         reinterpret_cast<char*>(rx_buf_.data()),
                         static_cast<int>(rx_buf_.size()),
                         0);
        if (got < 0) {
            int werr = WSAGetLastError();
            if (werr == WSAEINTR || werr == WSAEWOULDBLOCK) continue;
            if (error_out) *error_out = "Socket recv failed (" + std::to_string(werr) + ")";
            return false;
        }
#else
        ssize_t got = ::recv(static_cast<int>(s), rx_buf_.data(), rx_buf_.size(), 0);
        if (got < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            if (error_out) *error_out = std::string("Socket recv failed: ") + std::strerror(errno);
            return false;
        }
#endif
        if (got == 0) {
            if (error_out) *error_out = "Remote peer closed connection unexpectedly";
            return false;
        }
        bytes_received_.fetch_add(static_cast<uint64_t>(got), std::memory_order_relaxed);
        rx_head_ = 0;
        rx_tail_ = static_cast<size_t>(got);
    }
    return true;
}

bool RfbTransport::ConnectSocks5(const std::string& socks_host,
                                 uint16_t socks_port,
                                 const std::string& target_host,
                                 uint16_t target_port,
                                 int timeout_ms,
                                 std::string* error_out) {
    std::vector<uint8_t> connect_req;
    if (!BuildSocks5ConnectRequest(target_host, target_port, &connect_req, error_out)) {
        return false;
    }

    std::string tcp_err;
    if (!OpenTcpSocket(socks_host, socks_port, timeout_ms, &tcp_err)) {
        if (error_out) {
            *error_out = "SOCKS5 proxy (" + socks_host + ":" + std::to_string(socks_port) +
                         ") unreachable: " + tcp_err;
        }
        return false;
    }

    // 1. Greeting: VER=5, NMETHODS=1, METHOD=0x00 (No Authentication)
    static constexpr uint8_t kGreeting[3] = {0x05, 0x01, 0x00};
    if (!SendAll(kGreeting, sizeof(kGreeting), error_out)) {
        Close();
        return false;
    }

    uint8_t method_reply[2] = {0, 0};
    if (!ReadExactWithDeadline(method_reply, sizeof(method_reply), timeout_ms, error_out)) {
        Close();
        return false;
    }
    if (method_reply[0] != 0x05 || method_reply[1] != 0x00) {
        if (error_out) {
            *error_out = "SOCKS5 proxy rejected no-auth greeting";
        }
        Close();
        return false;
    }

    // 2. CONNECT target_host:target_port
    if (!SendAll(connect_req.data(), connect_req.size(), error_out)) {
        Close();
        return false;
    }

    uint8_t reply_hdr[4] = {0, 0, 0, 0};
    if (!ReadExactWithDeadline(reply_hdr, sizeof(reply_hdr), timeout_ms, error_out)) {
        Close();
        return false;
    }
    if (reply_hdr[0] != 0x05) {
        if (error_out) {
            *error_out = "Invalid SOCKS5 reply version: " + std::to_string(reply_hdr[0]);
        }
        Close();
        return false;
    }
    if (reply_hdr[1] != 0x00) {
        if (error_out) {
            *error_out = "SOCKS5 connect to " + target_host + ":" + std::to_string(target_port) +
                         " failed: " + Socks5ReplyErrorString(reply_hdr[1]);
        }
        Close();
        return false;
    }

    // 3. Drain BND.ADDR + BND.PORT
    uint8_t atyp = reply_hdr[3];
    uint8_t drain_buf[260];
    if (atyp == 0x01) {
        if (!ReadExactWithDeadline(drain_buf, 4 + 2, timeout_ms, error_out)) {
            Close();
            return false;
        }
    } else if (atyp == 0x04) {
        if (!ReadExactWithDeadline(drain_buf, 16 + 2, timeout_ms, error_out)) {
            Close();
            return false;
        }
    } else if (atyp == 0x03) {
        uint8_t dlen = 0;
        if (!ReadExactWithDeadline(&dlen, 1, timeout_ms, error_out) ||
            !ReadExactWithDeadline(drain_buf, static_cast<size_t>(dlen) + 2u, timeout_ms, error_out)) {
            Close();
            return false;
        }
    } else {
        if (error_out) {
            *error_out = "Unsupported SOCKS5 reply ATYP: " + std::to_string(atyp);
        }
        Close();
        return false;
    }

    return true;
}

bool RfbTransport::Connect(const std::string& target_host,
                           uint16_t target_port,
                           const std::string& socks5_proxy,
                           int timeout_ms,
                           std::string* error_out) {
    size_t non_ws = socks5_proxy.find_first_not_of(" \t\r\n");
    if (non_ws == std::string::npos) {
        return ConnectDirect(target_host, target_port, timeout_ms, error_out);
    }

    Endpoint proxy_ep{};
    if (!ParseHostPortSpec(socks5_proxy, 1055, &proxy_ep, error_out)) {
        return false;
    }
    return ConnectSocks5(proxy_ep.host, proxy_ep.port, target_host, target_port, timeout_ms, error_out);
}

int RfbTransport::WaitReadable(int timeout_ms, std::string* error_out) {
    if (aborted_.load(std::memory_order_acquire)) {
        if (error_out) *error_out = "Connection aborted";
        return -1;
    }
    if (rx_head_ < rx_tail_) {
        return 1;
    }
    NativeSocket s = kInvalidSocket;
    {
        std::lock_guard<std::mutex> lock(sock_mutex_);
        s = sock_;
    }
    if (s == kInvalidSocket) {
        if (error_out) *error_out = "Socket disconnected";
        return -1;
    }
    return PollSocket(s, false, timeout_ms, error_out);
}

bool RfbTransport::ReadExact(void* dst, size_t len, std::string* error_out) {
    auto* out = static_cast<uint8_t*>(dst);
    size_t offset = 0;

    while (offset < len) {
        if (aborted_.load(std::memory_order_acquire)) {
            if (error_out) *error_out = "Connection aborted";
            return false;
        }

        if (rx_head_ < rx_tail_) {
            size_t avail = rx_tail_ - rx_head_;
            size_t take = std::min(avail, len - offset);
            std::memcpy(out + offset, rx_buf_.data() + rx_head_, take);
            rx_head_ += take;
            if (rx_head_ == rx_tail_) {
                rx_head_ = 0;
                rx_tail_ = 0;
            }
            offset += take;
            continue;
        }

        int wr = WaitReadable(kAbortSliceMs, error_out);
        if (wr < 0) {
            return false;
        }
        if (wr == 0) {
            continue;
        }

        NativeSocket s = kInvalidSocket;
        {
            std::lock_guard<std::mutex> lock(sock_mutex_);
            s = sock_;
        }
        if (s == kInvalidSocket || aborted_.load(std::memory_order_acquire)) {
            if (error_out) *error_out = "Connection closed";
            return false;
        }

        // For large contiguous reads (>= 64 KB), read directly into caller's buffer!
        size_t remaining = len - offset;
        if (remaining >= rx_buf_.size()) {
#ifdef _WIN32
            int chunk = static_cast<int>(std::min<size_t>(remaining, 1024 * 1024));
            int got = ::recv(static_cast<SOCKET>(s), reinterpret_cast<char*>(out + offset), chunk, 0);
            if (got < 0) {
                int werr = WSAGetLastError();
                if (werr == WSAEINTR || werr == WSAEWOULDBLOCK) continue;
                if (error_out) *error_out = "Socket recv failed (" + std::to_string(werr) + ")";
                return false;
            }
#else
            ssize_t got = ::recv(static_cast<int>(s), out + offset, remaining, 0);
            if (got < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                if (error_out) *error_out = std::string("Socket recv failed: ") + std::strerror(errno);
                return false;
            }
#endif
            if (got == 0) {
                if (error_out) *error_out = "Remote server closed connection";
                return false;
            }
            bytes_received_.fetch_add(static_cast<uint64_t>(got), std::memory_order_relaxed);
            offset += static_cast<size_t>(got);
            continue;
        }

#ifdef _WIN32
        int got = ::recv(static_cast<SOCKET>(s),
                         reinterpret_cast<char*>(rx_buf_.data()),
                         static_cast<int>(rx_buf_.size()),
                         0);
        if (got < 0) {
            int werr = WSAGetLastError();
            if (werr == WSAEINTR || werr == WSAEWOULDBLOCK) continue;
            if (error_out) *error_out = "Socket recv failed (" + std::to_string(werr) + ")";
            return false;
        }
#else
        ssize_t got = ::recv(static_cast<int>(s), rx_buf_.data(), rx_buf_.size(), 0);
        if (got < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            if (error_out) *error_out = std::string("Socket recv failed: ") + std::strerror(errno);
            return false;
        }
#endif
        if (got == 0) {
            if (error_out) *error_out = "Remote server closed connection";
            return false;
        }
        bytes_received_.fetch_add(static_cast<uint64_t>(got), std::memory_order_relaxed);
        rx_head_ = 0;
        rx_tail_ = static_cast<size_t>(got);
    }
    return true;
}

bool RfbTransport::SendAll(const void* src, size_t len, std::string* error_out) {
    if (len == 0) return true;
    std::lock_guard<std::mutex> write_lock(send_mutex_);

    const auto* data = static_cast<const uint8_t*>(src);
    size_t sent = 0;
    while (sent < len) {
        if (aborted_.load(std::memory_order_acquire)) {
            if (error_out) *error_out = "Connection aborted";
            return false;
        }

        NativeSocket s = kInvalidSocket;
        {
            std::lock_guard<std::mutex> lock(sock_mutex_);
            s = sock_;
        }
        if (s == kInvalidSocket) {
            if (error_out) *error_out = "Socket disconnected";
            return false;
        }

#ifdef _WIN32
        int chunk = static_cast<int>(std::min<size_t>(len - sent, 1024 * 1024));
        int rc = ::send(static_cast<SOCKET>(s), reinterpret_cast<const char*>(data + sent), chunk, 0);
        if (rc < 0) {
            int werr = WSAGetLastError();
            if (werr == WSAEINTR) continue;
            if (werr == WSAEWOULDBLOCK) {
                if (PollSocket(s, true, kAbortSliceMs, error_out) < 0) return false;
                continue;
            }
            if (error_out) *error_out = "Socket send failed (" + std::to_string(werr) + ")";
            return false;
        }
#else
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags |= MSG_NOSIGNAL;
#endif
        ssize_t rc = ::send(static_cast<int>(s), data + sent, len - sent, flags);
        if (rc < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (PollSocket(s, true, kAbortSliceMs, error_out) < 0) return false;
                continue;
            }
            if (error_out) *error_out = std::string("Socket send failed: ") + std::strerror(errno);
            return false;
        }
#endif
        if (rc == 0) {
            if (error_out) *error_out = "Socket send returned 0";
            return false;
        }
        sent += static_cast<size_t>(rc);
    }
    return true;
}

}  // namespace mac_mirror
