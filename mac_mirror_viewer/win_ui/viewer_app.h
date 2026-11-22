#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "cli_options.h"
#include "d3d_renderer.h"
#include "keymap_win32.h"
#include "rfb_session.h"

namespace mac_mirror {

class ViewerApp {
public:
    explicit ViewerApp(AppOptions options);
    ~ViewerApp();

    ViewerApp(const ViewerApp&) = delete;
    ViewerApp& operator=(const ViewerApp&) = delete;

    int Run(HINSTANCE hinstance);

private:
    static constexpr UINT kWmAppFrameReady  = WM_APP + 1;
    static constexpr UINT kWmAppSyncSession = WM_APP + 2;
    static constexpr UINT kWmAppAuthPrompt  = WM_APP + 3;

    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);

    bool CreateMainWindow(HINSTANCE hinstance);
    void ApplyWindowMode(bool fullscreen, bool initial_show);
    void ToggleFullscreen();
    void AdjustCursorScale(float delta);
    void SetCursorScaleFromSliderX(float client_x);
    void CycleQualityMode();
    void PersistUiPrefs();

    void StartSession();
    void StopSession();
    void CancelPendingAuth();
    AuthCredentials OnWorkerAuthRequired(const AuthRequest& request);

    void SubmitLoginModal();
    void AdvanceModalFocus(bool reverse);
    std::string* ActiveModalFieldBuffer();
    void PasteClipboardIntoModalField();

    bool MapClientToRemote(float client_x, float client_y, uint16_t* out_x, uint16_t* out_y) const;
    void HandleRemoteMouseMove(float x, float y);
    void HandleRemoteMouseButton(float x, float y, uint8_t button_bit, bool down);
    void HandleRemoteMouseWheel(float x, float y, int delta, bool horizontal);

    void UpdateAnimationsAndTimers(uint64_t now_ms);
    void RenderNow();

    static uint64_t NowMs() noexcept;
    float QueryWindowDpiScale(HWND hwnd) const noexcept;

    AppOptions options_;
    ConnectionConfig active_config_;

    HWND hwnd_ = nullptr;
    float dpi_scale_ = 1.0f;
    RECT saved_windowed_rect_ = {};
    bool has_saved_windowed_rect_ = false;

    D3DRenderer renderer_;
    UiHitRegions hit_regions_ = {};
    Win32KeyMapper key_mapper_;

    std::unique_ptr<RfbSession> session_;
    std::atomic<bool> frame_msg_queued_{false};
    uint64_t last_rendered_frame_gen_ = 0;

    // Cross-thread session status / error synchronization
    std::mutex event_mutex_;
    SessionState latest_session_state_ = SessionState::Connecting;
    std::string latest_status_text_ = "Mac 화면 공유 서버에 연결 중...";
    bool latest_is_error_ = false;
    bool latest_is_auth_failure_ = false;
    bool session_event_dirty_ = true;

    // Worker <-> UI auth handoff
    std::mutex auth_mutex_;
    std::condition_variable auth_cv_;
    bool auth_waiting_ = false;
    bool auth_submitted_ = false;
    bool auth_cancelled_ = false;
    AuthRequest pending_auth_req_;
    AuthCredentials auth_response_;

    bool has_oneshot_creds_ = false;
    AuthCredentials oneshot_creds_;
    bool auto_env_attempted_ = false;

    // Login Modal UI state (username & password wiped on submit/destroy!)
    bool show_modal_ = true;
    AuthKind modal_auth_kind_ = AuthKind::AppleDiffieHellman;
    bool waiting_for_auth_input_ = false;
    bool show_advanced_settings_ = false;
    ModalField focused_field_ = ModalField::Username;
    std::string modal_username_;
    std::string modal_password_;
    std::string modal_host_text_;
    std::string modal_socks5_text_;
    std::string banner_text_;
    bool banner_is_error_ = false;

    // Auto-hide Floating Pill HUD state
    bool hud_wanted_ = false;
    bool hud_pinned_by_hotkey_ = false;
    bool hud_pointer_entered_ = false;
    float hud_reveal_ = 0.0f;
    uint64_t hud_last_active_ms_ = 0;
    uint64_t hud_peek_duration_ms_ = 1200;
    bool dragging_slider_ = false;

    // Pointer & wheel tracking
    float mouse_x_ = 0.0f;
    float mouse_y_ = 0.0f;
    bool mouse_in_window_ = true;
    bool mouse_initialized_ = false;
    uint8_t remote_button_mask_ = 0;
    int wheel_accum_v_ = 0;
    int wheel_accum_h_ = 0;

    // Render loop pacing
    bool needs_render_ = true;
    uint64_t last_anim_tick_ms_ = 0;
    uint64_t last_stats_refresh_ms_ = 0;
    uint64_t last_present_ms_ = 0;
};

}  // namespace mac_mirror
#endif
