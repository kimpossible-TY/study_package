#include "../include/rfb_session.h"

#include "rfb_adaptive.h"
#include "rfb_crypto.h"
#include "rfb_decoder.h"
#include "rfb_transport.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mac_mirror {

namespace {

constexpr uint16_t ReadBE16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) |
                                 static_cast<uint16_t>(p[1]));
}

constexpr uint32_t ReadBE32(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

constexpr int32_t ReadBES32(const uint8_t* p) noexcept {
    return static_cast<int32_t>(ReadBE32(p));
}

inline void WriteBE16(uint8_t* p, uint16_t v) noexcept {
    p[0] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[1] = static_cast<uint8_t>(v & 0xFFu);
}

inline void WriteBE32(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[3] = static_cast<uint8_t>(v & 0xFFu);
}

bool ParseRfbVersionBanner(const char banner[12], int* out_major, int* out_minor) noexcept {
    if (std::memcmp(banner, "RFB ", 4) != 0 || banner[7] != '.' || banner[11] != '\n') {
        return false;
    }
    auto parse3 = [](const char* s, int* out) -> bool {
        if (s[0] < '0' || s[0] > '9' ||
            s[1] < '0' || s[1] > '9' ||
            s[2] < '0' || s[2] > '9') {
            return false;
        }
        *out = (s[0] - '0') * 100 + (s[1] - '0') * 10 + (s[2] - '0');
        return true;
    };
    return parse3(banner + 4, out_major) && parse3(banner + 8, out_minor);
}

class RfbSessionImpl final : public RfbSession {
public:
    RfbSessionImpl() = default;
    ~RfbSessionImpl() override {
        Stop();
    }

    bool Start(const ConnectionConfig& config, SessionCallbacks callbacks) override {
        Stop();

        {
            std::lock_guard<std::mutex> lock(lifecycle_mutex_);
            config_ = config;
            callbacks_ = std::move(callbacks);
            stop_requested_.store(false, std::memory_order_release);
            state_.store(SessionState::Connecting, std::memory_order_release);

            uint8_t init_bpp = (config_.initial_quality == QualityMode::Turbo) ? 16 : 32;
            active_bpp_.store(init_bpp, std::memory_order_relaxed);
            fb_width_.store(0, std::memory_order_relaxed);
            fb_height_.store(0, std::memory_order_relaxed);

            {
                std::lock_guard<std::mutex> qlock(ctrl_mutex_);
                adaptive_ = AdaptiveQualityController(config_.initial_quality);
            }

            worker_ = std::thread(&RfbSessionImpl::WorkerMain, this);
        }
        return true;
    }

    void Stop() override {
        stop_requested_.store(true, std::memory_order_release);
        transport_.Abort();

        std::thread to_join;
        {
            std::lock_guard<std::mutex> lock(lifecycle_mutex_);
            if (worker_.joinable()) {
                to_join = std::move(worker_);
            }
        }
        if (to_join.joinable()) {
            if (to_join.get_id() == std::this_thread::get_id()) {
                to_join.detach();
            } else {
                to_join.join();
            }
        }
        transport_.Close();
        if (state_.load(std::memory_order_acquire) != SessionState::Error) {
            state_.store(SessionState::Disconnected, std::memory_order_release);
        }
    }

    SessionState GetState() const noexcept override {
        return state_.load(std::memory_order_acquire);
    }

    uint64_t GetFrameGeneration() const noexcept override {
        return frame_generation_.load(std::memory_order_acquire);
    }

    FramebufferLock LockFramebuffer(std::vector<Rect>* out_dirty_rects,
                                    bool* out_resized) override {
        FramebufferLock fb_lock;
        fb_lock.lock = std::unique_lock<std::mutex>(fb_mutex_);
        fb_lock.pixels_bgra = decoder_.PixelsBgra();
        fb_lock.width = decoder_.Width();
        fb_lock.height = decoder_.Height();
        fb_lock.stride_bytes = decoder_.StrideBytes();
        fb_lock.generation = frame_generation_.load(std::memory_order_relaxed);

        if (out_dirty_rects) {
            out_dirty_rects->clear();
            out_dirty_rects->swap(dirty_rects_);
        }
        if (out_resized) {
            *out_resized = fb_resized_;
            fb_resized_ = false;
        }
        return fb_lock;
    }

    uint64_t GetCursorGeneration() const noexcept override {
        return cursor_generation_.load(std::memory_order_acquire);
    }

    CursorState GetCursorState() const override {
        std::lock_guard<std::mutex> lock(cursor_mutex_);
        return cursor_state_;
    }

    void SendPointerEvent(uint16_t x, uint16_t y, uint8_t button_mask) override {
        if (config_.view_only || GetState() != SessionState::Connected) {
            return;
        }
        uint32_t w = fb_width_.load(std::memory_order_relaxed);
        uint32_t h = fb_height_.load(std::memory_order_relaxed);
        if (w > 0 && x >= w) x = static_cast<uint16_t>(w - 1);
        if (h > 0 && y >= h) y = static_cast<uint16_t>(h - 1);

        std::optional<PointerSample> immediate;
        {
            std::lock_guard<std::mutex> lock(ctrl_mutex_);
            immediate = adaptive_.OnPointerInput(
                AdaptiveQualityController::Clock::now(), x, y, button_mask);
        }
        if (immediate.has_value()) {
            SendWirePointerEvent(immediate->x, immediate->y, immediate->button_mask);
        }
    }

    void SendWheelEvent(uint16_t x,
                        uint16_t y,
                        int32_t steps_vertical,
                        int32_t steps_horizontal,
                        uint8_t current_button_mask) override {
        if (config_.view_only || GetState() != SessionState::Connected) {
            return;
        }
        if (steps_vertical == 0 && steps_horizontal == 0) {
            return;
        }

        uint32_t w = fb_width_.load(std::memory_order_relaxed);
        uint32_t h = fb_height_.load(std::memory_order_relaxed);
        if (w > 0 && x >= w) x = static_cast<uint16_t>(w - 1);
        if (h > 0 && y >= h) y = static_cast<uint16_t>(h - 1);

        {
            std::lock_guard<std::mutex> lock(ctrl_mutex_);
            adaptive_.OnWheelInput(AdaptiveQualityController::Clock::now());
        }

        uint8_t base_mask = static_cast<uint8_t>(current_button_mask & 0x07u);
        std::vector<uint8_t> batch;
        batch.reserve(48);

        auto append_clicks = [&](int32_t steps, uint8_t pos_bit, uint8_t neg_bit) {
            if (steps == 0) return;
            uint8_t wheel_bit = (steps > 0) ? pos_bit : neg_bit;
            int32_t count = std::min<int32_t>(std::abs(steps), 16);
            for (int32_t i = 0; i < count; ++i) {
                uint8_t press[6] = {5u, static_cast<uint8_t>(base_mask | wheel_bit), 0, 0, 0, 0};
                WriteBE16(press + 2, x);
                WriteBE16(press + 4, y);
                batch.insert(batch.end(), press, press + 6);

                uint8_t release[6] = {5u, base_mask, 0, 0, 0, 0};
                WriteBE16(release + 2, x);
                WriteBE16(release + 4, y);
                batch.insert(batch.end(), release, release + 6);
            }
        };

        append_clicks(steps_vertical, rfb_button::kWheelUp, rfb_button::kWheelDown);
        append_clicks(steps_horizontal, rfb_button::kWheelRight, rfb_button::kWheelLeft);

        if (!batch.empty()) {
            transport_.SendAll(batch.data(), batch.size());
        }
    }

    void SendKeyEvent(uint32_t keysym, bool down) override {
        if (config_.view_only || GetState() != SessionState::Connected) {
            return;
        }
        uint8_t pkt[8] = {4u, static_cast<uint8_t>(down ? 1u : 0u), 0u, 0u, 0, 0, 0, 0};
        WriteBE32(pkt + 4, keysym);
        transport_.SendAll(pkt, sizeof(pkt));
    }

    void SendClientCutText(const std::string& text) override {
        if (config_.view_only || GetState() != SessionState::Connected) {
            return;
        }
        if (text.size() > 1024u * 1024u) {
            return;
        }
        std::vector<uint8_t> pkt(8u + text.size(), 0u);
        pkt[0] = 6u;  // ClientCutText
        WriteBE32(pkt.data() + 4, static_cast<uint32_t>(text.size()));
        if (!text.empty()) {
            std::memcpy(pkt.data() + 8, text.data(), text.size());
        }
        transport_.SendAll(pkt.data(), pkt.size());
    }

    void SetQualityMode(QualityMode mode) override {
        std::lock_guard<std::mutex> lock(ctrl_mutex_);
        adaptive_.SetQualityMode(mode, AdaptiveQualityController::Clock::now());
    }

    QualityMode GetQualityMode() const noexcept override {
        std::lock_guard<std::mutex> lock(ctrl_mutex_);
        return adaptive_.GetQualityMode();
    }

    void RequestFullRefresh() override {
        std::lock_guard<std::mutex> lock(ctrl_mutex_);
        adaptive_.RequestFullRefresh();
    }

    SessionStats GetStats() const noexcept override {
        SessionStats stats{};
        stats.state = GetState();
        stats.active_bpp = active_bpp_.load(std::memory_order_relaxed);
        stats.fb_width = fb_width_.load(std::memory_order_relaxed);
        stats.fb_height = fb_height_.load(std::memory_order_relaxed);
        stats.bytes_received = transport_.GetBytesReceived();
        {
            std::lock_guard<std::mutex> lock(ctrl_mutex_);
            auto now = AdaptiveQualityController::Clock::now();
            stats.quality_mode = adaptive_.GetQualityMode();
            stats.fps = adaptive_.GetFps(now);
            stats.rtt_ms = adaptive_.GetRttMs();
            stats.frames_received = adaptive_.GetFramesReceived();
        }
        return stats;
    }

private:
    struct HandshakeInfo {
        std::string server_banner;
        int negotiated_minor = 8;
        std::vector<uint8_t> security_types;
    };

    void UpdateState(SessionState s, const std::string& status_text) {
        state_.store(s, std::memory_order_release);
        if (callbacks_.on_state_changed && !stop_requested_.load(std::memory_order_acquire)) {
            callbacks_.on_state_changed(s, status_text);
        }
    }

    void ReportError(const std::string& message, bool is_auth_failure) {
        if (stop_requested_.load(std::memory_order_acquire)) {
            state_.store(SessionState::Disconnected, std::memory_order_release);
            return;
        }
        state_.store(SessionState::Error, std::memory_order_release);
        if (callbacks_.on_state_changed) {
            callbacks_.on_state_changed(SessionState::Error, message);
        }
        if (callbacks_.on_error) {
            callbacks_.on_error(message, is_auth_failure);
        }
    }

    bool SendWirePointerEvent(uint16_t x, uint16_t y, uint8_t mask) {
        uint8_t pkt[6] = {5u, mask, 0, 0, 0, 0};
        WriteBE16(pkt + 2, x);
        WriteBE16(pkt + 4, y);
        return transport_.SendAll(pkt, sizeof(pkt));
    }

    bool SendWireSetPixelFormat(const RfbPixelFormat& pf, std::string* err) {
        uint8_t pkt[20];
        std::memset(pkt, 0, sizeof(pkt));
        pkt[0] = 0u;  // SetPixelFormat
        pf.Serialize(pkt + 4);
        return transport_.SendAll(pkt, sizeof(pkt), err);
    }

    bool SendWireSetEncodings(std::string* err) {
        static constexpr int32_t kEncodings[] = {
            rfb_encoding::kZRLE,
            rfb_encoding::kCopyRect,
            rfb_encoding::kZlib,
            rfb_encoding::kHextile,
            rfb_encoding::kRaw,
            rfb_encoding::kCursor,
            rfb_encoding::kDesktopSize,
        };
        constexpr uint16_t kCount = static_cast<uint16_t>(sizeof(kEncodings) / sizeof(kEncodings[0]));
        std::vector<uint8_t> pkt(4u + kCount * 4u, 0u);
        pkt[0] = 2u;  // SetEncodings
        WriteBE16(pkt.data() + 2, kCount);
        for (uint16_t i = 0; i < kCount; ++i) {
            WriteBE32(pkt.data() + 4 + i * 4, static_cast<uint32_t>(kEncodings[i]));
        }
        return transport_.SendAll(pkt.data(), pkt.size(), err);
    }

    bool SendWireFramebufferUpdateRequest(bool incremental,
                                          uint16_t x,
                                          uint16_t y,
                                          uint16_t w,
                                          uint16_t h,
                                          std::string* err) {
        uint8_t pkt[10];
        pkt[0] = 3u;  // FramebufferUpdateRequest
        pkt[1] = incremental ? 1u : 0u;
        WriteBE16(pkt + 2, x);
        WriteBE16(pkt + 4, y);
        WriteBE16(pkt + 6, w);
        WriteBE16(pkt + 8, h);
        return transport_.SendAll(pkt, sizeof(pkt), err);
    }

    std::string ReadServerReasonString() {
        uint8_t len_buf[4];
        if (!transport_.ReadExact(len_buf, 4)) {
            return {};
        }
        uint32_t rlen = ReadBE32(len_buf);
        if (rlen == 0 || rlen > 4096u) {
            return {};
        }
        std::string reason(rlen, '\0');
        if (!transport_.ReadExact(&reason[0], rlen)) {
            return {};
        }
        return reason;
    }

    bool ConnectAndNegotiateSecurityList(HandshakeInfo* out_info, std::string* err) {
        if (!transport_.Connect(config_.host, config_.port, config_.socks5_proxy, 12000, err)) {
            return false;
        }

        char server_ver[12];
        if (!transport_.ReadExact(server_ver, 12, err)) {
            return false;
        }

        int srv_major = 0;
        int srv_minor = 0;
        if (!ParseRfbVersionBanner(server_ver, &srv_major, &srv_minor)) {
            if (err) *err = "Invalid RFB server version banner";
            return false;
        }
        out_info->server_banner.assign(server_ver, 11);

        // macOS screensharingd advertises "RFB 003.889\n"; per RFC 6143 §7.1.1,
        // clients negotiate "RFB 003.008\n" so standard RFB 3.8 framing + SecurityType 30 apply.
        const char* client_ver = "RFB 003.008\n";
        out_info->negotiated_minor = 8;
        if (srv_major == 3 && srv_minor < 3) {
            if (err) *err = "Unsupported legacy RFB server version (< 3.3)";
            return false;
        } else if (srv_major == 3 && srv_minor < 7) {
            client_ver = "RFB 003.003\n";
            out_info->negotiated_minor = 3;
        } else if (srv_major == 3 && srv_minor == 7) {
            client_ver = "RFB 003.007\n";
            out_info->negotiated_minor = 7;
        }

        if (!transport_.SendAll(client_ver, 12, err)) {
            return false;
        }

        out_info->security_types.clear();
        if (out_info->negotiated_minor >= 7) {
            uint8_t count = 0;
            if (!transport_.ReadExact(&count, 1, err)) {
                return false;
            }
            if (count == 0) {
                std::string reason = ReadServerReasonString();
                if (err) {
                    *err = reason.empty() ? "VNC server rejected connection" : reason;
                }
                return false;
            }
            out_info->security_types.resize(count);
            if (!transport_.ReadExact(out_info->security_types.data(), count, err)) {
                return false;
            }
        } else {
            uint8_t sec32[4];
            if (!transport_.ReadExact(sec32, 4, err)) {
                return false;
            }
            uint32_t st = ReadBE32(sec32);
            if (st == 0) {
                std::string reason = ReadServerReasonString();
                if (err) {
                    *err = reason.empty() ? "VNC server rejected connection" : reason;
                }
                return false;
            }
            out_info->security_types.push_back(static_cast<uint8_t>(st & 0xFFu));
        }
        return true;
    }

    bool PerformSelectedAuth(uint8_t chosen_sec,
                             int negotiated_minor,
                             const AuthCredentials& creds,
                             bool* out_challenge_io_failed,
                             std::string* err) {
        *out_challenge_io_failed = false;

        if (negotiated_minor >= 7) {
            if (!transport_.SendAll(&chosen_sec, 1, err)) {
                *out_challenge_io_failed = true;
                return false;
            }
        }

        if (chosen_sec == 1u) {
            // SecurityType 1: None
            if (negotiated_minor >= 8) {
                uint8_t res_buf[4];
                if (!transport_.ReadExact(res_buf, 4, err)) return false;
                if (ReadBE32(res_buf) != 0) {
                    std::string reason = ReadServerReasonString();
                    if (err) *err = reason.empty() ? "Server rejected unauthenticated login" : reason;
                    return false;
                }
            }
            return true;
        }

        if (chosen_sec == 30u) {
            // SecurityType 30: Apple Remote Desktop Diffie-Hellman
            uint8_t dh_hdr[4];
            if (!transport_.ReadExact(dh_hdr, 4, err)) {
                *out_challenge_io_failed = true;
                return false;
            }
            uint16_t generator = ReadBE16(dh_hdr);
            uint16_t key_size = ReadBE16(dh_hdr + 2);
            if (key_size < 16 || key_size > 2048) {
                if (err) *err = "Invalid Apple DH key length from server: " + std::to_string(key_size);
                return false;
            }

            std::vector<uint8_t> prime(key_size);
            std::vector<uint8_t> server_pub(key_size);
            if (!transport_.ReadExact(prime.data(), key_size, err) ||
                !transport_.ReadExact(server_pub.data(), key_size, err)) {
                *out_challenge_io_failed = true;
                return false;
            }

            std::vector<uint8_t> reply_pkt;
            if (!BuildAppleDhResponse(generator, key_size, prime.data(), server_pub.data(),
                                      creds.username, creds.password, &reply_pkt, err)) {
                return false;
            }

            bool send_ok = transport_.SendAll(reply_pkt.data(), reply_pkt.size(), err);
            SecureZeroBuffer(reply_pkt.data(), reply_pkt.size());
            if (!send_ok) {
                return false;
            }
        } else if (chosen_sec == 2u) {
            // SecurityType 2: Standard VncAuth (16-byte DES challenge)
            uint8_t challenge[16];
            if (!transport_.ReadExact(challenge, 16, err)) {
                *out_challenge_io_failed = true;
                return false;
            }
            uint8_t response[16];
            if (!EncryptVncAuthChallenge(challenge, creds.password, response)) {
                if (err) *err = "VncAuth DES encryption failed";
                return false;
            }
            bool send_ok = transport_.SendAll(response, 16, err);
            SecureZeroBuffer(response, sizeof(response));
            if (!send_ok) {
                return false;
            }
        } else {
            if (err) *err = "Unsupported RFB security type " + std::to_string(chosen_sec);
            return false;
        }

        // Read 32-bit SecurityResult
        uint8_t sec_res_buf[4];
        if (!transport_.ReadExact(sec_res_buf, 4, err)) {
            if (err) *err = "Authentication rejected by remote Mac (connection closed)";
            return false;
        }
        uint32_t sec_status = ReadBE32(sec_res_buf);
        if (sec_status != 0) {
            std::string reason;
            if (negotiated_minor >= 8) {
                reason = ReadServerReasonString();
            }
            if (err) {
                *err = reason.empty() ? "Invalid Mac username or password" : reason;
            }
            return false;
        }
        return true;
    }

    void WorkerMain() {
        UpdateState(SessionState::Connecting,
                    config_.socks5_proxy.empty()
                        ? ("Connecting to " + config_.host + ":" + std::to_string(config_.port))
                        : ("Connecting via SOCKS5 " + config_.socks5_proxy + " -> " +
                           config_.host + ":" + std::to_string(config_.port)));

        HandshakeInfo hs{};
        std::string err;
        if (!ConnectAndNegotiateSecurityList(&hs, &err)) {
            ReportError(err.empty() ? "Connection failed" : err, false);
            return;
        }

        auto has_sec = [&](uint8_t t) -> bool {
            return std::find(hs.security_types.begin(), hs.security_types.end(), t) !=
                   hs.security_types.end();
        };

        bool has_none = has_sec(1u);
        bool has_apple_dh = has_sec(30u);
        bool has_vnc_auth = has_sec(2u);

        if (!has_none && !has_apple_dh && !has_vnc_auth) {
            ReportError("Mac server offered no supported VNC security types", false);
            return;
        }

        AuthCredentials creds;
        uint8_t chosen_sec = 1u;

        if (!has_none) {
            UpdateState(SessionState::Authenticating, "Waiting for credentials");
            if (!callbacks_.on_auth_required) {
                ReportError("Authentication required by server, but no credential handler set", true);
                return;
            }

            AuthRequest req{};
            req.kind = has_apple_dh ? AuthKind::AppleDiffieHellman : AuthKind::VncPassword;
            req.server_banner = hs.server_banner;

            creds = callbacks_.on_auth_required(req);
            if (creds.cancelled || stop_requested_.load(std::memory_order_acquire)) {
                creds.Wipe();
                state_.store(SessionState::Disconnected, std::memory_order_release);
                return;
            }

            if (has_apple_dh && (!creds.username.empty() || !has_vnc_auth)) {
                chosen_sec = 30u;
            } else {
                chosen_sec = 2u;
            }
            UpdateState(SessionState::Authenticating, "Authenticating with Mac");
        }

        bool challenge_io_failed = false;
        bool auth_ok = PerformSelectedAuth(
            chosen_sec, hs.negotiated_minor, creds, &challenge_io_failed, &err);

        // If the user stayed at the Login Modal long enough for screensharingd's pre-auth
        // idle timer to close the socket, transparently re-open once and submit credentials!
        if (!auth_ok && challenge_io_failed && !has_none &&
            !stop_requested_.load(std::memory_order_acquire)) {
            HandshakeInfo retry_hs{};
            std::string retry_err;
            if (ConnectAndNegotiateSecurityList(&retry_hs, &retry_err)) {
                bool dummy_io = false;
                err.clear();
                auth_ok = PerformSelectedAuth(
                    chosen_sec, retry_hs.negotiated_minor, creds, &dummy_io, &err);
            }
        }
        creds.Wipe();

        if (!auth_ok) {
            ReportError(err.empty() ? "Authentication failed" : err, /*is_auth_failure=*/!has_none);
            return;
        }

        // ClientInit (1 byte: shared flag)
        uint8_t shared_byte = config_.shared ? 1u : 0u;
        if (!transport_.SendAll(&shared_byte, 1, &err)) {
            ReportError("Failed sending RFB ClientInit: " + err, false);
            return;
        }

        // ServerInit (24 bytes + desktop name)
        uint8_t srv_init[24];
        if (!transport_.ReadExact(srv_init, sizeof(srv_init), &err)) {
            ReportError("Failed reading RFB ServerInit: " + err, false);
            return;
        }

        uint16_t init_w = ReadBE16(srv_init);
        uint16_t init_h = ReadBE16(srv_init + 2);
        RfbPixelFormat srv_pf = RfbPixelFormat::Parse(srv_init + 4);
        uint32_t name_len = ReadBE32(srv_init + 20);
        if (name_len > 65536u) {
            ReportError("Invalid desktop name length in ServerInit", false);
            return;
        }
        std::string desktop_name(name_len, '\0');
        if (name_len > 0 && !transport_.ReadExact(&desktop_name[0], name_len, &err)) {
            ReportError("Failed reading desktop name: " + err, false);
            return;
        }

        if (!decoder_.ResetSession(init_w, init_h, srv_pf, &fb_mutex_)) {
            ReportError("Invalid initial framebuffer dimensions", false);
            return;
        }

        fb_width_.store(init_w, std::memory_order_relaxed);
        fb_height_.store(init_h, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(fb_mutex_);
            fb_resized_ = true;
            dirty_rects_.clear();
            dirty_rects_.push_back(Rect{0, 0, init_w, init_h});
        }

        if (!SendWireSetEncodings(&err)) {
            ReportError("Failed sending SetEncodings: " + err, false);
            return;
        }

        uint8_t current_bpp = (GetQualityMode() == QualityMode::Turbo) ? 16 : 32;
        RfbPixelFormat active_pf =
            (current_bpp == 16) ? RfbPixelFormat::Rgb565() : RfbPixelFormat::Bgrx8888();
        if (!SendWireSetPixelFormat(active_pf, &err)) {
            ReportError("Failed sending initial SetPixelFormat: " + err, false);
            return;
        }
        decoder_.SetPixelFormat(active_pf);
        active_bpp_.store(current_bpp, std::memory_order_relaxed);

        UpdateState(SessionState::Connected, desktop_name);
        if (callbacks_.on_connected && !stop_requested_.load(std::memory_order_acquire)) {
            callbacks_.on_connected(init_w, init_h, desktop_name);
        }

        uint16_t cur_w = init_w;
        uint16_t cur_h = init_h;
        bool fbur_in_flight = false;

        if (!SendWireFramebufferUpdateRequest(false, 0, 0, cur_w, cur_h, &err)) {
            ReportError("Failed sending initial FramebufferUpdateRequest: " + err, false);
            return;
        }
        fbur_in_flight = true;
        {
            std::lock_guard<std::mutex> lock(ctrl_mutex_);
            adaptive_.CommitAppliedFormat(current_bpp, true);
            adaptive_.OnUpdateRequestSent(AdaptiveQualityController::Clock::now(), false);
        }

        auto reader_fn = [this](void* dst, size_t len, std::string* r_err) -> bool {
            return transport_.ReadExact(dst, len, r_err);
        };

        // Main RFB streaming & Adaptive Quality loop
        while (!stop_requested_.load(std::memory_order_acquire)) {
            std::optional<PointerSample> due_ptr;
            QualityDecision decision{};
            bool has_pending_ptr = false;
            {
                std::lock_guard<std::mutex> lock(ctrl_mutex_);
                auto now = AdaptiveQualityController::Clock::now();
                due_ptr = adaptive_.FlushPendingPointer(now);
                has_pending_ptr = adaptive_.HasPendingPointer();
                decision = adaptive_.PeekDecision(now, current_bpp);
            }

            if (due_ptr.has_value() && !config_.view_only) {
                if (!SendWirePointerEvent(due_ptr->x, due_ptr->y, due_ptr->button_mask)) {
                    ReportError("Connection lost while sending pointer input", false);
                    return;
                }
            }

            if (!fbur_in_flight) {
                // RFC 6143 §7.5.1 safe point: no FramebufferUpdateRequest is outstanding!
                if (decision.format_changed) {
                    RfbPixelFormat next_pf = (decision.target_bpp == 16)
                                                 ? RfbPixelFormat::Rgb565()
                                                 : RfbPixelFormat::Bgrx8888();
                    if (!SendWireSetPixelFormat(next_pf, &err)) {
                        ReportError("Connection lost sending SetPixelFormat: " + err, false);
                        return;
                    }
                    decoder_.SetPixelFormat(next_pf);
                    current_bpp = decision.target_bpp;
                    active_bpp_.store(current_bpp, std::memory_order_relaxed);
                }

                bool incremental = !decision.request_full_refresh;
                if (!SendWireFramebufferUpdateRequest(incremental, 0, 0, cur_w, cur_h, &err)) {
                    ReportError("Connection lost sending FramebufferUpdateRequest: " + err, false);
                    return;
                }
                fbur_in_flight = true;
                {
                    std::lock_guard<std::mutex> lock(ctrl_mutex_);
                    adaptive_.CommitAppliedFormat(current_bpp, !incremental);
                    adaptive_.OnUpdateRequestSent(AdaptiveQualityController::Clock::now(), false);
                }
            }

            int poll_ms = has_pending_ptr ? 4 : 20;
            int wr = transport_.WaitReadable(poll_ms, &err);
            if (wr < 0) {
                ReportError(err.empty() ? "Disconnected from Mac" : err, false);
                return;
            }
            if (wr == 0) {
                continue;
            }

            uint8_t msg_type = 0;
            if (!transport_.ReadExact(&msg_type, 1, &err)) {
                ReportError(err.empty() ? "Disconnected from Mac" : err, false);
                return;
            }

            if (msg_type == 0u) {
                // FramebufferUpdate
                uint8_t fbu_hdr[3];
                if (!transport_.ReadExact(fbu_hdr, 3, &err)) {
                    ReportError("Failed reading FramebufferUpdate header: " + err, false);
                    return;
                }
                uint16_t num_rects = ReadBE16(fbu_hdr + 1);
                std::vector<Rect> frame_rects;
                frame_rects.reserve(num_rects);
                bool frame_resized = false;

                for (uint16_t r = 0; r < num_rects; ++r) {
                    uint8_t rect_hdr[12];
                    if (!transport_.ReadExact(rect_hdr, 12, &err)) {
                        ReportError("Failed reading rectangle header: " + err, false);
                        return;
                    }
                    uint16_t rx = ReadBE16(rect_hdr);
                    uint16_t ry = ReadBE16(rect_hdr + 2);
                    uint16_t rw = ReadBE16(rect_hdr + 4);
                    uint16_t rh = ReadBE16(rect_hdr + 6);
                    int32_t enc = ReadBES32(rect_hdr + 8);

                    DecodeOutcome outcome = DecodeOutcome::PixelRectUpdated;
                    CursorState new_cursor;
                    if (!decoder_.DecodeRectangle(rx, ry, rw, rh, enc, reader_fn,
                                                  &outcome, &new_cursor, &err)) {
                        ReportError("RFB decode error: " + err, false);
                        return;
                    }

                    if (outcome == DecodeOutcome::CursorUpdated) {
                        std::lock_guard<std::mutex> clock(cursor_mutex_);
                        uint64_t next_cgen = cursor_generation_.load(std::memory_order_relaxed) + 1u;
                        new_cursor.generation = next_cgen;
                        cursor_state_ = std::move(new_cursor);
                        cursor_generation_.store(next_cgen, std::memory_order_release);
                    } else if (outcome == DecodeOutcome::DesktopResized) {
                        cur_w = rw;
                        cur_h = rh;
                        fb_width_.store(rw, std::memory_order_relaxed);
                        fb_height_.store(rh, std::memory_order_relaxed);
                        frame_resized = true;
                        std::lock_guard<std::mutex> qlock(ctrl_mutex_);
                        adaptive_.RequestFullRefresh();
                    } else if (rw > 0 && rh > 0) {
                        frame_rects.push_back(Rect{rx, ry, rw, rh});
                    }
                }

                fbur_in_flight = false;

                {
                    std::lock_guard<std::mutex> qlock(ctrl_mutex_);
                    adaptive_.OnUpdateCompleted(AdaptiveQualityController::Clock::now());
                }

                if (frame_resized || !frame_rects.empty()) {
                    uint64_t new_gen = 0;
                    {
                        std::lock_guard<std::mutex> fblock(fb_mutex_);
                        if (frame_resized) {
                            fb_resized_ = true;
                            dirty_rects_.clear();
                            dirty_rects_.push_back(Rect{0, 0, cur_w, cur_h});
                        } else {
                            if (dirty_rects_.size() + frame_rects.size() > 256u) {
                                dirty_rects_.clear();
                                dirty_rects_.push_back(Rect{0, 0, cur_w, cur_h});
                            } else {
                                dirty_rects_.insert(dirty_rects_.end(),
                                                    frame_rects.begin(),
                                                    frame_rects.end());
                            }
                        }
                        new_gen = frame_generation_.fetch_add(1, std::memory_order_acq_rel) + 1u;
                    }
                    if (callbacks_.on_frame_updated &&
                        !stop_requested_.load(std::memory_order_acquire)) {
                        callbacks_.on_frame_updated(new_gen);
                    }
                }
            } else if (msg_type == 1u) {
                // SetColourMapEntries
                uint8_t cmap_hdr[5];
                if (!transport_.ReadExact(cmap_hdr, 5, &err)) {
                    ReportError("Failed reading SetColourMapEntries: " + err, false);
                    return;
                }
                uint16_t first_col = ReadBE16(cmap_hdr + 1);
                uint16_t num_cols = ReadBE16(cmap_hdr + 3);
                std::vector<uint8_t> entries(static_cast<size_t>(num_cols) * 6u);
                if (!entries.empty() && !transport_.ReadExact(entries.data(), entries.size(), &err)) {
                    ReportError("Failed reading colour map entries: " + err, false);
                    return;
                }
                for (uint16_t i = 0; i < num_cols; ++i) {
                    decoder_.SetColourMapEntry(
                        static_cast<uint16_t>(first_col + i),
                        ReadBE16(entries.data() + i * 6),
                        ReadBE16(entries.data() + i * 6 + 2),
                        ReadBE16(entries.data() + i * 6 + 4));
                }
            } else if (msg_type == 2u) {
                // Bell (0 payload bytes)
                continue;
            } else if (msg_type == 3u) {
                // ServerCutText
                uint8_t cut_hdr[7];
                if (!transport_.ReadExact(cut_hdr, 7, &err)) {
                    ReportError("Failed reading ServerCutText header: " + err, false);
                    return;
                }
                uint32_t cut_len = ReadBE32(cut_hdr + 3);
                if (cut_len > 16u * 1024u * 1024u) {
                    ReportError("ServerCutText exceeds 16 MB safety limit", false);
                    return;
                }
                if (cut_len <= 1024u * 1024u) {
                    std::string cut_text(cut_len, '\0');
                    if (cut_len > 0 && !transport_.ReadExact(&cut_text[0], cut_len, &err)) {
                        ReportError("Failed reading ServerCutText payload: " + err, false);
                        return;
                    }
                    if (callbacks_.on_server_cut_text &&
                        !stop_requested_.load(std::memory_order_acquire)) {
                        callbacks_.on_server_cut_text(cut_text);
                    }
                } else {
                    std::vector<uint8_t> drain(65536u);
                    size_t left = cut_len;
                    while (left > 0) {
                        size_t chunk = std::min<size_t>(left, drain.size());
                        if (!transport_.ReadExact(drain.data(), chunk, &err)) {
                            ReportError("Failed draining ServerCutText: " + err, false);
                            return;
                        }
                        left -= chunk;
                    }
                }
            } else {
                ReportError("Unexpected RFB server message type: " + std::to_string(msg_type), false);
                return;
            }
        }
    }

    std::mutex lifecycle_mutex_;
    std::thread worker_;
    ConnectionConfig config_{};
    SessionCallbacks callbacks_{};

    RfbTransport transport_;
    RfbDecoder decoder_;

    mutable std::mutex fb_mutex_;
    std::vector<Rect> dirty_rects_;
    bool fb_resized_ = false;
    std::atomic<uint64_t> frame_generation_{0};

    mutable std::mutex cursor_mutex_;
    CursorState cursor_state_{};
    std::atomic<uint64_t> cursor_generation_{0};

    mutable std::mutex ctrl_mutex_;
    AdaptiveQualityController adaptive_;

    std::atomic<bool> stop_requested_{false};
    std::atomic<SessionState> state_{SessionState::Disconnected};
    std::atomic<uint8_t> active_bpp_{32};
    std::atomic<uint32_t> fb_width_{0};
    std::atomic<uint32_t> fb_height_{0};
};

}  // namespace

std::unique_ptr<RfbSession> RfbSession::Create() {
    return std::make_unique<RfbSessionImpl>();
}

}  // namespace mac_mirror
