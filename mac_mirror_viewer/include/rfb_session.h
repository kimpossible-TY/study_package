#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mac_mirror {

enum class QualityMode : uint8_t {
    Adaptive = 0,  // 16-bit RGB565 ZRLE+CopyRect during active input, 32-bit BGRX8888 after 250ms idle
    Sharp    = 1,  // Always 32-bit BGRX8888 lossless ZRLE+CopyRect
    Turbo    = 2,  // Always 16-bit RGB565 high-FPS ZRLE+CopyRect
};

inline const char* QualityModeToString(QualityMode mode) noexcept {
    switch (mode) {
        case QualityMode::Adaptive: return "Adaptive";
        case QualityMode::Sharp:    return "Sharp";
        case QualityMode::Turbo:    return "Turbo";
    }
    return "Adaptive";
}

inline QualityMode ParseQualityMode(const std::string& value) noexcept {
    if (value == "sharp" || value == "Sharp" || value == "SHARP") {
        return QualityMode::Sharp;
    }
    if (value == "turbo" || value == "Turbo" || value == "TURBO") {
        return QualityMode::Turbo;
    }
    return QualityMode::Adaptive;
}

enum class AuthKind : uint8_t {
    None               = 0,  // SecurityType 1
    VncPassword        = 1,  // SecurityType 2 (standard 8-byte DES VncAuth, password only)
    AppleDiffieHellman = 2,  // SecurityType 30 (macOS Screen Sharing username + password DH)
};

enum class SessionState : uint8_t {
    Disconnected   = 0,
    Connecting     = 1,
    Authenticating = 2,
    Connected      = 3,
    Error          = 4,
};

namespace rfb_button {
constexpr uint8_t kLeft       = 0x01;
constexpr uint8_t kMiddle     = 0x02;
constexpr uint8_t kRight      = 0x04;
constexpr uint8_t kWheelUp    = 0x08;
constexpr uint8_t kWheelDown  = 0x10;
constexpr uint8_t kWheelLeft  = 0x20;
constexpr uint8_t kWheelRight = 0x40;
}  // namespace rfb_button

namespace rfb_keysym {
constexpr uint32_t kBackSpace = 0xFF08;
constexpr uint32_t kTab       = 0xFF09;
constexpr uint32_t kReturn    = 0xFF0D;
constexpr uint32_t kPause     = 0xFF13;
constexpr uint32_t kScrollLock = 0xFF14;
constexpr uint32_t kEscape    = 0xFF1B;
constexpr uint32_t kDelete    = 0xFFFF;
constexpr uint32_t kHome      = 0xFF50;
constexpr uint32_t kLeft      = 0xFF51;
constexpr uint32_t kUp        = 0xFF52;
constexpr uint32_t kRight     = 0xFF53;
constexpr uint32_t kDown      = 0xFF54;
constexpr uint32_t kPageUp    = 0xFF55;
constexpr uint32_t kPageDown  = 0xFF56;
constexpr uint32_t kEnd       = 0xFF57;
constexpr uint32_t kInsert    = 0xFF63;
constexpr uint32_t kF1        = 0xFFBE;
constexpr uint32_t kF2        = 0xFFBF;
constexpr uint32_t kF3        = 0xFFC0;
constexpr uint32_t kF4        = 0xFFC1;
constexpr uint32_t kF5        = 0xFFC2;
constexpr uint32_t kF6        = 0xFFC3;
constexpr uint32_t kF7        = 0xFFC4;
constexpr uint32_t kF8        = 0xFFC5;
constexpr uint32_t kF9        = 0xFFC6;
constexpr uint32_t kF10       = 0xFFC7;
constexpr uint32_t kF11       = 0xFFC8;
constexpr uint32_t kF12       = 0xFFC9;
constexpr uint32_t kShiftL    = 0xFFE1;
constexpr uint32_t kShiftR    = 0xFFE2;
constexpr uint32_t kControlL  = 0xFFE3;
constexpr uint32_t kControlR  = 0xFFE4;
constexpr uint32_t kCapsLock  = 0xFFE5;
constexpr uint32_t kMetaL     = 0xFFE7;  // macOS Command (Left)
constexpr uint32_t kMetaR     = 0xFFE8;  // macOS Command (Right)
constexpr uint32_t kAltL      = 0xFFE9;  // macOS Option (Left)
constexpr uint32_t kAltR      = 0xFFEA;  // macOS Option (Right)
constexpr uint32_t kSuperL    = 0xFFEB;
constexpr uint32_t kSuperR    = 0xFFEC;
}  // namespace rfb_keysym

struct ConnectionConfig {
    std::string host = "100.80.74.94";
    uint16_t port = 5900;
    std::string socks5_proxy;  // Optional "host:port" (e.g. "127.0.0.1:1055"); empty = direct TCP
    QualityMode initial_quality = QualityMode::Turbo;
    bool shared = true;
    bool view_only = false;
};

struct AuthRequest {
    AuthKind kind = AuthKind::AppleDiffieHellman;
    std::string server_banner;  // Optional informational string (e.g. "RFB 003.889")
};

// In-memory credentials only; NEVER persist username or password to disk.
struct AuthCredentials {
    bool cancelled = false;
    std::string username;
    std::string password;

    void Wipe() noexcept {
        auto scrub = [](std::string& s) noexcept {
            if (!s.empty()) {
                volatile char* p = &s[0];
                for (size_t i = 0; i < s.size(); ++i) {
                    p[i] = 0;
                }
                s.clear();
            }
        };
        scrub(username);
        scrub(password);
    }

    ~AuthCredentials() {
        Wipe();
    }
};

struct Rect {
    uint16_t x = 0;
    uint16_t y = 0;
    uint16_t w = 0;
    uint16_t h = 0;
};

// RAII read lock over the session's BGRA32 framebuffer.
// Pixel byte layout in memory (compatible with DXGI_FORMAT_B8G8R8A8_UNORM on little-endian):
//   pixels_bgra[offset + 0] = Blue
//   pixels_bgra[offset + 1] = Green
//   pixels_bgra[offset + 2] = Red
//   pixels_bgra[offset + 3] = Alpha (always 0xFF opaque)
struct FramebufferLock {
    std::unique_lock<std::mutex> lock;
    const uint8_t* pixels_bgra = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride_bytes = 0;  // Typically width * 4
    uint64_t generation = 0;
};

// RichCursor (-239) state decoded by the RFB client engine.
struct CursorState {
    uint64_t generation = 0;
    bool has_server_cursor = false;
    bool cursor_is_blank = false;  // True when server sent 0x0 or all-transparent cursor
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t hotspot_x = 0;
    uint16_t hotspot_y = 0;
    std::vector<uint8_t> pixels_bgra;  // [B, G, R, A] straight alpha, width * height * 4 bytes
};

struct SessionStats {
    SessionState state = SessionState::Disconnected;
    QualityMode quality_mode = QualityMode::Adaptive;
    uint8_t active_bpp = 32;  // 16 during Turbo/Adaptive-burst, 32 during Sharp/Adaptive-idle
    double fps = 0.0;
    double rtt_ms = 0.0;
    uint32_t fb_width = 0;
    uint32_t fb_height = 0;
    uint64_t frames_received = 0;
    uint64_t bytes_received = 0;
};

struct SessionCallbacks {
    // Called synchronously on the RFB worker thread when server requires credentials.
    // UI implementation blocks the worker thread until user submits Login Modal or cancels/stops.
    std::function<AuthCredentials(const AuthRequest& request)> on_auth_required;

    // Called when session state changes.
    std::function<void(SessionState state, const std::string& status_text)> on_state_changed;

    // Called once ServerInit succeeds with initial framebuffer dimensions and desktop name.
    std::function<void(uint32_t width, uint32_t height, const std::string& desktop_name)> on_connected;

    // Called after each completed FramebufferUpdate (or DesktopSize change) increments generation.
    std::function<void(uint64_t generation)> on_frame_updated;

    // Called when the remote server pushes clipboard text via RFB ServerCutText (msg type 3).
    std::function<void(const std::string& text)> on_server_cut_text;

    // Called on connection failure, auth rejection, or remote disconnect.
    // is_auth_failure == true indicates invalid credentials (UI re-prompts in Login Modal).
    std::function<void(const std::string& error_message, bool is_auth_failure)> on_error;
};

// Thread-safe C++17 RFB client session interface.
// Implemented by core_client (RfbSession::Create()) and consumed by win_ui.
class RfbSession {
public:
    static std::unique_ptr<RfbSession> Create();

    virtual ~RfbSession() = default;

    // Starts an asynchronous RFB connection worker thread using config & callbacks.
    // If a session is already active, stops it cleanly before starting the new one.
    virtual bool Start(const ConnectionConfig& config, SessionCallbacks callbacks) = 0;

    // Requests immediate shutdown, closes active socket to unblock I/O, and joins worker thread
    // (safe to call from UI thread; idempotent).
    virtual void Stop() = 0;

    virtual SessionState GetState() const noexcept = 0;

    // Lock-free monotonic generation counter incremented after every completed FramebufferUpdate
    // or DesktopSize resize. Allows 120Hz UI loop to skip locking when unchanged.
    virtual uint64_t GetFrameGeneration() const noexcept = 0;

    // Locks the BGRA32 framebuffer for GPU texture upload.
    // If out_dirty_rects is non-null, swaps accumulated dirty rects since previous consume into it.
    // If out_resized is non-null, sets *out_resized = true if framebuffer dimensions changed since
    // previous consume and clears the internal resized flag.
    virtual FramebufferLock LockFramebuffer(std::vector<Rect>* out_dirty_rects = nullptr,
                                            bool* out_resized = nullptr) = 0;

    // RichCursor state inspection.
    virtual uint64_t GetCursorGeneration() const noexcept = 0;
    virtual CursorState GetCursorState() const = 0;

    // Thread-safe input submission (queued/sent to server; ignored if disconnected or view_only).
    virtual void SendPointerEvent(uint16_t x, uint16_t y, uint8_t button_mask) = 0;
    virtual void SendWheelEvent(uint16_t x,
                                uint16_t y,
                                int32_t steps_vertical,
                                int32_t steps_horizontal = 0,
                                uint8_t current_button_mask = 0) = 0;
    virtual void SendKeyEvent(uint32_t keysym, bool down) = 0;
    virtual void SendClientCutText(const std::string& text) { (void)text; }

    // Runtime quality mode & full refresh control.
    virtual void SetQualityMode(QualityMode mode) = 0;
    virtual QualityMode GetQualityMode() const noexcept = 0;
    virtual void RequestFullRefresh() = 0;

    // Thread-safe snapshot of live FPS, RTT (ms), and session metrics for the HUD.
    virtual SessionStats GetStats() const noexcept = 0;
};

}  // namespace mac_mirror
