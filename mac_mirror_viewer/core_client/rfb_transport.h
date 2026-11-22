#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace mac_mirror {

struct Endpoint {
    std::string host;
    uint16_t port = 0;
};

// Parses "host:port", "[ipv6]:port", or "host" (when default_port != 0).
bool ParseHostPortSpec(const std::string& spec,
                       uint16_t default_port,
                       Endpoint* out_endpoint,
                       std::string* error_out = nullptr);

// Builds an RFC 1928 SOCKS5 CONNECT request packet:
//   [0x05, 0x01, 0x00, ATYP, DST.ADDR..., DST.PORT_HI, DST.PORT_LO]
// Supports IPv4 (ATYP=0x01), IPv6 (ATYP=0x04), and Domain name (ATYP=0x03).
bool BuildSocks5ConnectRequest(const std::string& target_host,
                               uint16_t target_port,
                               std::vector<uint8_t>* out_packet,
                               std::string* error_out = nullptr);

// Human-readable RFC 1928 reply code description.
std::string Socks5ReplyErrorString(uint8_t reply_code);

// Cross-platform (WinSock2 / POSIX) low-jitter TCP + SOCKS5 client transport
// with 64 KB user-space read buffering, 1 MB SO_RCVBUF, TCP_NODELAY, and
// thread-safe SendAll + instant Abort().
class RfbTransport {
public:
    RfbTransport();
    ~RfbTransport();

    RfbTransport(const RfbTransport&) = delete;
    RfbTransport& operator=(const RfbTransport&) = delete;

    // Connects directly to target_host:target_port when socks5_proxy is empty,
    // or tunnels via RFC 1928 SOCKS5 proxy ("host:port") otherwise.
    bool Connect(const std::string& target_host,
                 uint16_t target_port,
                 const std::string& socks5_proxy,
                 int timeout_ms = 12000,
                 std::string* error_out = nullptr);

    bool ConnectDirect(const std::string& host,
                       uint16_t port,
                       int timeout_ms = 12000,
                       std::string* error_out = nullptr);

    bool ConnectSocks5(const std::string& socks_host,
                       uint16_t socks_port,
                       const std::string& target_host,
                       uint16_t target_port,
                       int timeout_ms = 12000,
                       std::string* error_out = nullptr);

    // Thread-safe immediate cancellation: wakes any blocked Connect/WaitReadable/ReadExact.
    void Abort() noexcept;

    // Closes underlying socket and resets read buffer (call from owner thread after worker stops).
    void Close() noexcept;

    bool IsConnected() const noexcept;
    bool IsAborted() const noexcept { return aborted_.load(std::memory_order_acquire); }

    // Returns:
    //   1 : data is immediately available in user-space buffer or kernel socket
    //   0 : timeout elapsed without data
    //  -1 : socket error or Abort() called
    int WaitReadable(int timeout_ms, std::string* error_out = nullptr);

    // Reads exactly `len` bytes into `dst`, blocking until complete, EOF, error, or Abort().
    bool ReadExact(void* dst, size_t len, std::string* error_out = nullptr);

    // Thread-safe atomic write of `len` bytes to the socket.
    bool SendAll(const void* src, size_t len, std::string* error_out = nullptr);

    uint64_t GetBytesReceived() const noexcept {
        return bytes_received_.load(std::memory_order_relaxed);
    }

private:
    using NativeSocket = uintptr_t;
    static constexpr NativeSocket kInvalidSocket = ~static_cast<NativeSocket>(0);

    bool OpenTcpSocket(const std::string& host,
                       uint16_t port,
                       int timeout_ms,
                       std::string* error_out);
    void ApplyLowLatencySocketOptions(NativeSocket sock) noexcept;
    bool ReadExactWithDeadline(void* dst,
                               size_t len,
                               int timeout_ms,
                               std::string* error_out);
    int PollSocket(NativeSocket sock, bool for_write, int timeout_ms, std::string* error_out);

    mutable std::mutex sock_mutex_;
    std::mutex send_mutex_;
    NativeSocket sock_ = kInvalidSocket;
    std::atomic<bool> aborted_{false};
    std::atomic<uint64_t> bytes_received_{0};

    std::vector<uint8_t> rx_buf_;
    size_t rx_head_ = 0;
    size_t rx_tail_ = 0;
};

}  // namespace mac_mirror
