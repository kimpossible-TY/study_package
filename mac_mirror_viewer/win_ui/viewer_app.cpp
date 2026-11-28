#include "viewer_app.h"

#ifdef _WIN32

#include <dwmapi.h>
#include <imm.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "clipboard_text.h"

namespace mac_mirror {

namespace {

constexpr wchar_t kWindowClassName[] = L"MacMirrorViewerD3DWindow";

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

std::string WideToUtf8String(const std::wstring& w) {
    if (w.empty()) return std::string();
    int len = WideCharToMultiByte(
        CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return std::string();
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &out[0], len, nullptr, nullptr);
    return out;
}

// Decodes RFB ServerCutText bytes as UTF-8 (macOS screensharingd) with automatic
// ISO-8859-1 fallback for legacy RFB servers.
std::wstring DecodeRfbCutTextToWide(const std::string& raw) {
    if (raw.empty()) return std::wstring();
    int len = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
    if (len > 0) {
        std::wstring out(static_cast<size_t>(len), L'\0');
        MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(), static_cast<int>(raw.size()), &out[0], len);
        return out;
    }
    std::wstring latin1;
    latin1.reserve(raw.size());
    for (char ch : raw) {
        latin1.push_back(static_cast<wchar_t>(static_cast<uint8_t>(ch)));
    }
    return latin1;
}

void EnableDarkTitleBar(HWND hwnd) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
}

}  // namespace

ViewerApp::ViewerApp(AppOptions options) : options_(std::move(options)) {
    active_config_.host = options_.host;
    active_config_.port = options_.port;
    active_config_.socks5_proxy = options_.socks5_proxy;
    active_config_.initial_quality = options_.prefs.quality_mode;
    active_config_.shared = true;
    active_config_.view_only = false;

    modal_host_text_ = active_config_.host + ":" + std::to_string(active_config_.port);
    modal_socks5_text_ = active_config_.socks5_proxy;
    if (!options_.auto_user.empty()) {
        modal_username_ = options_.auto_user;
    }
    show_advanced_settings_ = options_.show_settings;
    key_mapper_.SetSwapCtrlCmd(options_.prefs.swap_ctrl_cmd);
    if (options_.show_hud) {
        hud_wanted_ = true;
        hud_pinned_by_hotkey_ = true;
        hud_reveal_ = 1.0f;
    }
}

ViewerApp::~ViewerApp() {
    StopSession();
    renderer_.Shutdown();
    SecureWipeString(&modal_username_);
    SecureWipeString(&modal_password_);
    SecureWipeString(&last_synced_clipboard_utf8_);
    {
        std::lock_guard<std::mutex> lk(clipboard_mutex_);
        SecureWipeString(&pending_server_clipboard_);
        has_pending_server_clipboard_ = false;
    }
    oneshot_creds_.Wipe();
    auth_response_.Wipe();
}

uint64_t ViewerApp::NowMs() noexcept {
    return static_cast<uint64_t>(GetTickCount64());
}

float ViewerApp::QueryWindowDpiScale(HWND hwnd) const noexcept {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        auto fn = reinterpret_cast<GetDpiForWindowFn>(
            reinterpret_cast<void*>(GetProcAddress(user32, "GetDpiForWindow")));
        if (fn && hwnd) {
            UINT dpi = fn(hwnd);
            if (dpi >= 72 && dpi <= 480) {
                return static_cast<float>(dpi) / 96.0f;
            }
        }
    }
    HDC screen_dc = GetDC(nullptr);
    float scale = 1.0f;
    if (screen_dc) {
        int dpi_x = GetDeviceCaps(screen_dc, LOGPIXELSX);
        if (dpi_x >= 72 && dpi_x <= 480) {
            scale = static_cast<float>(dpi_x) / 96.0f;
        }
        ReleaseDC(nullptr, screen_dc);
    }
    return scale;
}

bool ViewerApp::CreateMainWindow(HINSTANCE hinstance) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &ViewerApp::StaticWndProc;
    wc.hInstance = hinstance;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // Prevent GDI background erase flicker over D3D11 SwapChain
    wc.lpszClassName = kWindowClassName;
    RegisterClassExW(&wc);

    POINT cursor_pt = {};
    GetCursorPos(&cursor_pt);
    HMONITOR hmon = MonitorFromPoint(cursor_pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hmon, &mi)) {
        mi.rcMonitor = RECT{0, 0, 1920, 1080};
        mi.rcWork = RECT{0, 0, 1920, 1040};
    }

    // Seed sensible 16:10-friendly windowed geometry on the active monitor's work area.
    const int work_w = std::max<int>(800, mi.rcWork.right - mi.rcWork.left);
    const int work_h = std::max<int>(600, mi.rcWork.bottom - mi.rcWork.top);
    const int win_h = static_cast<int>(work_h * 0.84);
    const int win_w = std::min<int>(static_cast<int>(work_w * 0.88), static_cast<int>(win_h * 1.55));
    saved_windowed_rect_.left = mi.rcWork.left + (work_w - win_w) / 2;
    saved_windowed_rect_.top = mi.rcWork.top + (work_h - win_h) / 2;
    saved_windowed_rect_.right = saved_windowed_rect_.left + win_w;
    saved_windowed_rect_.bottom = saved_windowed_rect_.top + win_h;
    has_saved_windowed_rect_ = true;

    const DWORD initial_style = options_.prefs.fullscreen ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    const RECT initial_rect = options_.prefs.fullscreen ? mi.rcMonitor : saved_windowed_rect_;

    hwnd_ = CreateWindowExW(0,
                            kWindowClassName,
                            L"Mac Mirror",
                            initial_style,
                            initial_rect.left,
                            initial_rect.top,
                            initial_rect.right - initial_rect.left,
                            initial_rect.bottom - initial_rect.top,
                            nullptr,
                            nullptr,
                            hinstance,
                            this);
    if (!hwnd_) {
        return false;
    }

    // Disable Windows Korean/CJK IME interception so passwords & remote macOS shortcuts work cleanly.
    ImmAssociateContext(hwnd_, nullptr);
    EnableDarkTitleBar(hwnd_);
    clipboard_listener_registered_ = (AddClipboardFormatListener(hwnd_) != FALSE);
    last_observed_clipboard_seq_ = GetClipboardSequenceNumber();

    dpi_scale_ = QueryWindowDpiScale(hwnd_);

    RECT client_rc = {};
    GetClientRect(hwnd_, &client_rc);
    const uint32_t cw = static_cast<uint32_t>(std::max<LONG>(1, client_rc.right - client_rc.left));
    const uint32_t ch = static_cast<uint32_t>(std::max<LONG>(1, client_rc.bottom - client_rc.top));

    if (!renderer_.Initialize(hwnd_, cw, ch, dpi_scale_)) {
        return false;
    }

    // Initialize local cursor position from OS cursor (or centered inside client viewport).
    POINT client_pt = cursor_pt;
    if (ScreenToClient(hwnd_, &client_pt) &&
        client_pt.x > 0 && client_pt.y > 0 &&
        client_pt.x < static_cast<LONG>(cw) && client_pt.y < static_cast<LONG>(ch)) {
        mouse_x_ = static_cast<float>(client_pt.x);
        mouse_y_ = static_cast<float>(client_pt.y);
        mouse_initialized_ = true;
    } else {
        mouse_x_ = static_cast<float>(cw) * 0.52f;
        mouse_y_ = static_cast<float>(ch) * 0.54f;
    }

    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
    return true;
}

void ViewerApp::ApplyWindowMode(bool fullscreen, bool initial_show) {
    if (!hwnd_) return;

    if (fullscreen) {
        if (!options_.prefs.fullscreen && !initial_show) {
            GetWindowRect(hwnd_, &saved_windowed_rect_);
            has_saved_windowed_rect_ = true;
        }
        HMONITOR hmon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(hmon, &mi)) {
            SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            SetWindowPos(hwnd_,
                         HWND_TOP,
                         mi.rcMonitor.left,
                         mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
        }
    } else {
        SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        const RECT& r = saved_windowed_rect_;
        SetWindowPos(hwnd_,
                     HWND_NOTOPMOST,
                     r.left,
                     r.top,
                     std::max<LONG>(640, r.right - r.left),
                     std::max<LONG>(420, r.bottom - r.top),
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    }

    options_.prefs.fullscreen = fullscreen;
    needs_render_ = true;
}

void ViewerApp::ToggleFullscreen() {
    ApplyWindowMode(!options_.prefs.fullscreen, false);
    PersistUiPrefs();
}

void ViewerApp::AdjustCursorScale(float delta) {
    const float next = std::round(ClampCursorScale(options_.prefs.cursor_scale + delta) * 10.0f) / 10.0f;
    options_.prefs.cursor_scale = next;
    PersistUiPrefs();

    if (!show_modal_) {
        hud_wanted_ = true;
        hud_last_active_ms_ = NowMs();
        hud_peek_duration_ms_ = 1500;
    }
    needs_render_ = true;
}

void ViewerApp::SetCursorScaleFromSliderX(float client_x) {
    const RectF& tr = hit_regions_.hud_slider_track;
    if (tr.width() <= 1.0f) return;
    const float t = std::clamp((client_x - tr.left) / tr.width(), 0.0f, 1.0f);
    const float raw = 1.0f + t * 3.0f;
    options_.prefs.cursor_scale = std::round(ClampCursorScale(raw) * 10.0f) / 10.0f;
    hud_last_active_ms_ = NowMs();
    needs_render_ = true;
}

void ViewerApp::CycleQualityMode() {
    QualityMode next = QualityMode::Adaptive;
    switch (options_.prefs.quality_mode) {
        case QualityMode::Adaptive: next = QualityMode::Sharp; break;
        case QualityMode::Sharp:    next = QualityMode::Turbo; break;
        case QualityMode::Turbo:    next = QualityMode::Adaptive; break;
    }
    options_.prefs.quality_mode = next;
    active_config_.initial_quality = next;
    if (session_) {
        session_->SetQualityMode(next);
    }
    PersistUiPrefs();
    hud_last_active_ms_ = NowMs();
    needs_render_ = true;
}

void ViewerApp::ToggleSwapCtrlCmd() {
    if (session_) {
        key_mapper_.ReleaseAll(session_.get());
    }
    options_.prefs.swap_ctrl_cmd = !options_.prefs.swap_ctrl_cmd;
    key_mapper_.SetSwapCtrlCmd(options_.prefs.swap_ctrl_cmd);
    PersistUiPrefs();

    if (!show_modal_) {
        hud_wanted_ = true;
        hud_last_active_ms_ = NowMs();
        hud_peek_duration_ms_ = 1500;
    }
    needs_render_ = true;
}

void ViewerApp::PersistUiPrefs() {
    SaveUiPrefsToDisk(options_.prefs);
}

void ViewerApp::SyncLocalClipboardToRemote(bool force) {
    if (!hwnd_ || !session_ || show_modal_ || session_->GetState() != SessionState::Connected) {
        return;
    }

    const DWORD seq = GetClipboardSequenceNumber();
    if (has_written_remote_clipboard_ && seq == last_written_clipboard_seq_) {
        last_observed_clipboard_seq_ = seq;
        return;
    }
    if (!force && seq == last_observed_clipboard_seq_) {
        return;
    }
    if (has_modal_pasted_clipboard_ && seq == modal_pasted_clipboard_seq_) {
        last_observed_clipboard_seq_ = seq;
        return;
    }

    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        last_observed_clipboard_seq_ = seq;
        return;
    }
    if (!OpenClipboard(hwnd_)) {
        return;
    }

    std::wstring wide_text;
    HANDLE hdata = GetClipboardData(CF_UNICODETEXT);
    if (hdata) {
        const wchar_t* wptr = static_cast<const wchar_t*>(GlobalLock(hdata));
        if (wptr) {
            constexpr size_t kMaxWideChars = 512u * 1024u;
            size_t n = wcsnlen(wptr, kMaxWideChars);
            wide_text.assign(wptr, n);
            GlobalUnlock(hdata);
        }
    }
    CloseClipboard();

    last_observed_clipboard_seq_ = seq;
    if (wide_text.empty()) {
        return;
    }

    std::string utf8 = NormalizeLineEndingsToLf(WideToUtf8String(wide_text));
    if (utf8.empty() || utf8.size() > 1024u * 1024u) {
        return;
    }
    if (!force && utf8 == last_synced_clipboard_utf8_) {
        return;
    }

    last_synced_clipboard_utf8_ = utf8;
    has_written_remote_clipboard_ = false;
    session_->SendClientCutText(utf8);
}

bool ViewerApp::TryWriteWindowsClipboardText(const std::wstring& wide_text) {
    if (!hwnd_ || !OpenClipboard(hwnd_)) {
        return false;
    }
    if (!EmptyClipboard()) {
        CloseClipboard();
        return false;
    }

    const size_t bytes = (wide_text.size() + 1u) * sizeof(wchar_t);
    HGLOBAL hmem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!hmem) {
        CloseClipboard();
        return false;
    }
    void* dst = GlobalLock(hmem);
    if (!dst) {
        GlobalFree(hmem);
        CloseClipboard();
        return false;
    }
    if (!wide_text.empty()) {
        std::memcpy(dst, wide_text.data(), wide_text.size() * sizeof(wchar_t));
    }
    static_cast<wchar_t*>(dst)[wide_text.size()] = L'\0';
    GlobalUnlock(hmem);

    if (!SetClipboardData(CF_UNICODETEXT, hmem)) {
        GlobalFree(hmem);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

void ViewerApp::ApplyPendingServerClipboard() {
    if (!hwnd_ || show_modal_) {
        pending_clipboard_retries_ = 0;
        return;
    }

    std::string raw_text;
    {
        std::lock_guard<std::mutex> lk(clipboard_mutex_);
        if (!has_pending_server_clipboard_) {
            pending_clipboard_retries_ = 0;
            return;
        }
        raw_text = pending_server_clipboard_;
    }

    if (raw_text.empty()) {
        std::lock_guard<std::mutex> lk(clipboard_mutex_);
        has_pending_server_clipboard_ = false;
        pending_clipboard_retries_ = 0;
        return;
    }

    std::wstring decoded_wide = DecodeRfbCutTextToWide(raw_text);
    std::string normalized_lf = NormalizeLineEndingsToLf(WideToUtf8String(decoded_wide));
    if (normalized_lf == last_synced_clipboard_utf8_ && !normalized_lf.empty()) {
        std::lock_guard<std::mutex> lk(clipboard_mutex_);
        has_pending_server_clipboard_ = false;
        SecureWipeString(&pending_server_clipboard_);
        pending_clipboard_retries_ = 0;
        return;
    }

    std::string crlf_utf8 = NormalizeLineEndingsToCrlf(normalized_lf);
    std::wstring wide_crlf = DecodeRfbCutTextToWide(crlf_utf8);
    if (TryWriteWindowsClipboardText(wide_crlf)) {
        last_synced_clipboard_utf8_ = std::move(normalized_lf);
        last_written_clipboard_seq_ = GetClipboardSequenceNumber();
        last_observed_clipboard_seq_ = last_written_clipboard_seq_;
        has_written_remote_clipboard_ = true;
        pending_clipboard_retries_ = 0;
        std::lock_guard<std::mutex> lk(clipboard_mutex_);
        has_pending_server_clipboard_ = false;
        SecureWipeString(&pending_server_clipboard_);
    } else if (pending_clipboard_retries_ > 0) {
        --pending_clipboard_retries_;
        next_clipboard_retry_ms_ = NowMs() + 50u;
    }
}

void ViewerApp::ClearRemoteClipboardOnExit() {
    if (!hwnd_ || !has_written_remote_clipboard_) {
        return;
    }
    if (GetClipboardSequenceNumber() == last_written_clipboard_seq_) {
        if (OpenClipboard(hwnd_)) {
            EmptyClipboard();
            CloseClipboard();
        }
    }
    has_written_remote_clipboard_ = false;
    SecureWipeString(&last_synced_clipboard_utf8_);
}

void ViewerApp::CancelPendingAuth() {
    std::lock_guard<std::mutex> lk(auth_mutex_);
    if (auth_waiting_) {
        auth_cancelled_ = true;
        auth_response_.Wipe();
        auth_response_.cancelled = true;
        auth_cv_.notify_all();
    }
}

void ViewerApp::StopSession() {
    CancelPendingAuth();
    if (session_) {
        key_mapper_.ReleaseAll(session_.get());
        session_->Stop();
    }
}

void ViewerApp::StartSession() {
    StopSession();
    if (!session_) {
        session_ = RfbSession::Create();
    }

    {
        std::lock_guard<std::mutex> lk(event_mutex_);
        latest_session_state_ = SessionState::Connecting;
        latest_status_text_ = "Mac 화면 공유 서버에 연결 중...";
        latest_is_error_ = false;
        latest_is_auth_failure_ = false;
        session_event_dirty_ = true;
    }

    show_modal_ = true;
    waiting_for_auth_input_ = false;
    banner_is_error_ = false;
    banner_text_ = "Mac 화면 공유 서버에 연결 중...";
    last_rendered_frame_gen_ = 0;
    needs_render_ = true;

    SessionCallbacks cb;
    cb.on_auth_required = [this](const AuthRequest& req) -> AuthCredentials {
        return OnWorkerAuthRequired(req);
    };

    cb.on_state_changed = [this](SessionState st, const std::string& msg) {
        {
            std::lock_guard<std::mutex> lk(event_mutex_);
            latest_session_state_ = st;
            if (!msg.empty() && !latest_is_error_) {
                latest_status_text_ = msg;
            }
            session_event_dirty_ = true;
        }
        if (hwnd_) {
            PostMessageW(hwnd_, kWmAppSyncSession, 0, 0);
        }
    };

    cb.on_connected = [this](uint32_t /*w*/, uint32_t /*h*/, const std::string& /*desktop*/) {
        {
            std::lock_guard<std::mutex> lk(event_mutex_);
            latest_session_state_ = SessionState::Connected;
            latest_is_error_ = false;
            latest_status_text_.clear();
            session_event_dirty_ = true;
        }
        if (hwnd_) {
            PostMessageW(hwnd_, kWmAppSyncSession, 0, 0);
        }
    };

    cb.on_frame_updated = [this](uint64_t /*gen*/) {
        if (hwnd_ && !frame_msg_queued_.exchange(true, std::memory_order_acq_rel)) {
            PostMessageW(hwnd_, kWmAppFrameReady, 0, 0);
        }
    };

    cb.on_server_cut_text = [this](const std::string& text) {
        {
            std::lock_guard<std::mutex> lk(clipboard_mutex_);
            pending_server_clipboard_ = text;
            has_pending_server_clipboard_ = true;
        }
        if (hwnd_) {
            PostMessageW(hwnd_, kWmAppServerClipboard, 0, 0);
        }
    };

    cb.on_error = [this](const std::string& err, bool is_auth_failure) {
        {
            std::lock_guard<std::mutex> lk(event_mutex_);
            latest_session_state_ = SessionState::Error;
            latest_status_text_ = err.empty() ? "연결이 종료되었습니다." : err;
            latest_is_error_ = true;
            latest_is_auth_failure_ = is_auth_failure;
            session_event_dirty_ = true;
        }
        if (hwnd_) {
            PostMessageW(hwnd_, kWmAppSyncSession, 0, 0);
        }
    };

    session_->Start(active_config_, std::move(cb));
}

AuthCredentials ViewerApp::OnWorkerAuthRequired(const AuthRequest& request) {
    // 1. Check automated test environment credentials on initial connect attempt.
    if (!auto_env_attempted_ && !options_.auto_pass_env.empty()) {
        auto_env_attempted_ = true;
        std::string env_pass;
        if (ReadEnvVarUtf8(options_.auto_pass_env, &env_pass)) {
            AuthCredentials auto_creds;
            auto_creds.cancelled = false;
            auto_creds.username = options_.auto_user;
            auto_creds.password = env_pass;
            SecureWipeString(&env_pass);
            if (request.kind != AuthKind::AppleDiffieHellman || !auto_creds.username.empty()) {
                return auto_creds;
            }
        }
    }

    // 2. Check one-shot credentials submitted alongside host/SOCKS5 change or reconnect.
    {
        std::unique_lock<std::mutex> lk(auth_mutex_);
        if (has_oneshot_creds_) {
            has_oneshot_creds_ = false;
            AuthCredentials out = std::move(oneshot_creds_);
            oneshot_creds_.Wipe();
            return out;
        }

        auth_waiting_ = true;
        auth_submitted_ = false;
        auth_cancelled_ = false;
        pending_auth_req_ = request;
        auth_response_.Wipe();

        if (hwnd_) {
            PostMessageW(hwnd_, kWmAppAuthPrompt, static_cast<WPARAM>(request.kind), 0);
        }

        auth_cv_.wait(lk, [this]() { return auth_submitted_ || auth_cancelled_; });
        auth_waiting_ = false;

        AuthCredentials result = std::move(auth_response_);
        auth_response_.Wipe();
        if (auth_cancelled_) {
            result.Wipe();
            result.cancelled = true;
        }
        return result;
    }
}

void ViewerApp::SubmitLoginModal() {
    std::string parsed_host;
    uint16_t parsed_port = active_config_.port;
    if (!ParseHostPortString(modal_host_text_, &parsed_host, &parsed_port, 5900)) {
        banner_is_error_ = true;
        banner_text_ = "올바른 대상 호스트 형식(예: 100.80.74.94:5900)을 입력해 주세요.";
        show_advanced_settings_ = true;
        focused_field_ = ModalField::TargetHost;
        needs_render_ = true;
        return;
    }

    std::string normalized_socks5;
    if (!modal_socks5_text_.empty()) {
        std::string shost;
        uint16_t sport = 1055;
        if (!ParseHostPortString(modal_socks5_text_, &shost, &sport, 1055)) {
            banner_is_error_ = true;
            banner_text_ = "올바른 SOCKS5 주소(예: 127.0.0.1:1055)를 입력하거나 비워 주세요.";
            show_advanced_settings_ = true;
            focused_field_ = ModalField::Socks5;
            needs_render_ = true;
            return;
        }
        normalized_socks5 = shost + ":" + std::to_string(sport);
    }

    const bool route_changed = (parsed_host != active_config_.host) ||
                               (parsed_port != active_config_.port) ||
                               (normalized_socks5 != active_config_.socks5_proxy);

    const bool need_user = waiting_for_auth_input_ && (modal_auth_kind_ == AuthKind::AppleDiffieHellman);
    const bool need_pass = waiting_for_auth_input_ && (modal_auth_kind_ != AuthKind::None);

    if (!route_changed && need_user && modal_username_.empty()) {
        banner_is_error_ = true;
        banner_text_ = "Mac 사용자 이름을 입력해 주세요.";
        focused_field_ = ModalField::Username;
        needs_render_ = true;
        return;
    }
    if (!route_changed && need_pass && modal_password_.empty()) {
        banner_is_error_ = true;
        banner_text_ = "암호를 입력해 주세요.";
        focused_field_ = ModalField::Password;
        needs_render_ = true;
        return;
    }

    // Check if worker thread is currently paused inside OnWorkerAuthRequired with unchanged route.
    {
        std::lock_guard<std::mutex> lk(auth_mutex_);
        if (auth_waiting_ && !route_changed) {
            auth_response_.cancelled = false;
            auth_response_.username = modal_username_;
            auth_response_.password = modal_password_;
            auth_submitted_ = true;
            auth_cv_.notify_all();

            // Scrub password from UI buffer immediately on submit; keep username only until session ends.
            SecureWipeString(&modal_password_);
            waiting_for_auth_input_ = false;
            banner_is_error_ = false;
            banner_text_ = "인증 확인 중...";
            needs_render_ = true;
            return;
        }
    }

    // Otherwise update route config and reconnect cleanly (carrying entered credentials in-memory if provided).
    active_config_.host = parsed_host;
    active_config_.port = parsed_port;
    active_config_.socks5_proxy = normalized_socks5;
    modal_host_text_ = active_config_.host + ":" + std::to_string(active_config_.port);
    modal_socks5_text_ = active_config_.socks5_proxy;

    {
        std::lock_guard<std::mutex> lk(auth_mutex_);
        if (!modal_password_.empty()) {
            oneshot_creds_.cancelled = false;
            oneshot_creds_.username = modal_username_;
            oneshot_creds_.password = modal_password_;
            has_oneshot_creds_ = true;
            SecureWipeString(&modal_password_);
        } else {
            has_oneshot_creds_ = false;
            oneshot_creds_.Wipe();
        }
    }

    StartSession();
}

void ViewerApp::AdvanceModalFocus(bool reverse) {
    const bool show_auth_inputs = waiting_for_auth_input_ || banner_is_error_;
    std::vector<ModalField> order;
    if (show_auth_inputs && modal_auth_kind_ == AuthKind::AppleDiffieHellman) {
        order.push_back(ModalField::Username);
    }
    if (show_auth_inputs && modal_auth_kind_ != AuthKind::None) {
        order.push_back(ModalField::Password);
    }
    if (show_advanced_settings_) {
        order.push_back(ModalField::TargetHost);
        order.push_back(ModalField::Socks5);
    }
    if (order.empty()) return;

    auto it = std::find(order.begin(), order.end(), focused_field_);
    if (it == order.end()) {
        focused_field_ = order.front();
    } else {
        const ptrdiff_t idx = it - order.begin();
        const ptrdiff_t n = static_cast<ptrdiff_t>(order.size());
        const ptrdiff_t next_idx = reverse ? ((idx - 1 + n) % n) : ((idx + 1) % n);
        focused_field_ = order[static_cast<size_t>(next_idx)];
    }
    needs_render_ = true;
}

std::string* ViewerApp::ActiveModalFieldBuffer() {
    const bool show_auth_inputs = waiting_for_auth_input_ || banner_is_error_;
    switch (focused_field_) {
        case ModalField::Username:
            return (show_auth_inputs && modal_auth_kind_ == AuthKind::AppleDiffieHellman) ? &modal_username_ : nullptr;
        case ModalField::Password:
            return (show_auth_inputs && modal_auth_kind_ != AuthKind::None) ? &modal_password_ : nullptr;
        case ModalField::TargetHost:
            return show_advanced_settings_ ? &modal_host_text_ : nullptr;
        case ModalField::Socks5:
            return show_advanced_settings_ ? &modal_socks5_text_ : nullptr;
    }
    return nullptr;
}

void ViewerApp::PasteClipboardIntoModalField() {
    std::string* target = ActiveModalFieldBuffer();
    if (!target || !hwnd_) return;
    if (!OpenClipboard(hwnd_)) return;

    HANDLE hdata = GetClipboardData(CF_UNICODETEXT);
    if (hdata) {
        const wchar_t* wptr = static_cast<const wchar_t*>(GlobalLock(hdata));
        if (wptr) {
            std::wstring clean;
            for (size_t i = 0; wptr[i] != L'\0' && i < 256; ++i) {
                if (wptr[i] != L'\r' && wptr[i] != L'\n' && wptr[i] != L'\t') {
                    clean.push_back(wptr[i]);
                }
            }
            GlobalUnlock(hdata);
            std::string utf8 = WideToUtf8String(clean);
            if (target->size() + utf8.size() <= 256) {
                target->append(utf8);
            }
            SecureWipeString(&utf8);
            volatile wchar_t* vp = clean.empty() ? nullptr : &clean[0];
            for (size_t i = 0; i < clean.size(); ++i) vp[i] = 0;
            modal_pasted_clipboard_seq_ = GetClipboardSequenceNumber();
            has_modal_pasted_clipboard_ = true;
            needs_render_ = true;
        }
    }
    CloseClipboard();
}

bool ViewerApp::MapClientToRemote(float client_x,
                                  float client_y,
                                  uint16_t* out_x,
                                  uint16_t* out_y) const {
    const uint32_t fb_w = renderer_.remote_fb_width();
    const uint32_t fb_h = renderer_.remote_fb_height();
    const RectF& vp = hit_regions_.remote_viewport;
    if (fb_w == 0 || fb_h == 0 || vp.width() <= 1.0f || vp.height() <= 1.0f) {
        return false;
    }
    const float rel_x = std::clamp((client_x - vp.left) / vp.width(), 0.0f, 1.0f);
    const float rel_y = std::clamp((client_y - vp.top) / vp.height(), 0.0f, 1.0f);
    const uint32_t rx = std::min<uint32_t>(fb_w - 1u, static_cast<uint32_t>(rel_x * static_cast<float>(fb_w)));
    const uint32_t ry = std::min<uint32_t>(fb_h - 1u, static_cast<uint32_t>(rel_y * static_cast<float>(fb_h)));
    *out_x = static_cast<uint16_t>(rx);
    *out_y = static_cast<uint16_t>(ry);
    return true;
}

void ViewerApp::HandleRemoteMouseMove(float x, float y) {
    if (!session_ || show_modal_) return;
    if (hud_reveal_ > 0.25f && hit_regions_.hud_pill.contains(x, y)) {
        return;
    }
    uint16_t rx = 0, ry = 0;
    if (MapClientToRemote(x, y, &rx, &ry)) {
        session_->SendPointerEvent(rx, ry, remote_button_mask_);
    }
}

void ViewerApp::HandleRemoteMouseButton(float x, float y, uint8_t button_bit, bool down) {
    if (down) {
        remote_button_mask_ |= button_bit;
    } else {
        remote_button_mask_ &= static_cast<uint8_t>(~button_bit);
    }
    if (!session_ || show_modal_) return;
    uint16_t rx = 0, ry = 0;
    if (MapClientToRemote(x, y, &rx, &ry)) {
        session_->SendPointerEvent(rx, ry, remote_button_mask_);
    }
}

void ViewerApp::HandleRemoteMouseWheel(float x, float y, int delta, bool horizontal) {
    if (!session_ || show_modal_) return;
    if (hud_reveal_ > 0.25f && hit_regions_.hud_pill.contains(x, y)) return;

    ScrollWheelAccumulator* accum = horizontal ? &wheel_accum_h_ : &wheel_accum_v_;
    const uint64_t now_ms = NowMs();
    const int steps = accum->Accumulate(delta, now_ms);
    if (steps == 0) return;

    uint16_t rx = 0, ry = 0;
    if (MapClientToRemote(x, y, &rx, &ry)) {
        if (horizontal) {
            session_->SendWheelEvent(rx, ry, 0, steps, remote_button_mask_);
        } else {
            renderer_.NotifyVerticalWheel(now_ms, steps);
            session_->SendWheelEvent(rx, ry, steps, 0, remote_button_mask_);
        }
    }
}

LRESULT CALLBACK ViewerApp::StaticWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    ViewerApp* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<ViewerApp*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<ViewerApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->HandleMessage(msg, wparam, lparam);
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT ViewerApp::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps = {};
            BeginPaint(hwnd_, &ps);
            EndPaint(hwnd_, &ps);
            needs_render_ = true;
            return 0;
        }

        case WM_SIZE: {
            const uint32_t w = LOWORD(lparam);
            const uint32_t h = HIWORD(lparam);
            if (w > 0 && h > 0 && wparam != SIZE_MINIMIZED) {
                dpi_scale_ = QueryWindowDpiScale(hwnd_);
                renderer_.Resize(w, h, dpi_scale_);
                if (!mouse_initialized_) {
                    mouse_x_ = static_cast<float>(w) * 0.52f;
                    mouse_y_ = static_cast<float>(h) * 0.54f;
                }
                needs_render_ = true;
            }
            return 0;
        }

        case WM_DPICHANGED: {
            dpi_scale_ = static_cast<float>(HIWORD(wparam)) / 96.0f;
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            if (suggested && !options_.prefs.fullscreen) {
                SetWindowPos(hwnd_,
                             nullptr,
                             suggested->left,
                             suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            needs_render_ = true;
            return 0;
        }

        case WM_SETCURSOR: {
            if (LOWORD(lparam) == HTCLIENT) {
                if (!show_modal_) {
                    // Hide OS cursor inside the live session window; D3D renders zero-lag vector macOS cursor!
                    SetCursor(nullptr);
                    return TRUE;
                }
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                return TRUE;
            }
            break;
        }

        case WM_SETFOCUS: {
            SyncLocalClipboardToRemote(false);
            return 0;
        }

        case WM_CLIPBOARDUPDATE: {
            SyncLocalClipboardToRemote(false);
            return 0;
        }

        case WM_KILLFOCUS: {
            remote_button_mask_ = 0;
            dragging_slider_ = false;
            wheel_accum_v_.Reset();
            wheel_accum_h_.Reset();
            if (session_) {
                key_mapper_.ReleaseAll(session_.get());
            }
            needs_render_ = true;
            return 0;
        }

        case kWmAppFrameReady: {
            frame_msg_queued_.store(false, std::memory_order_release);
            needs_render_ = true;
            return 0;
        }

        case kWmAppServerClipboard: {
            pending_clipboard_retries_ = 5;
            next_clipboard_retry_ms_ = 0;
            ApplyPendingServerClipboard();
            return 0;
        }

        case kWmAppAuthPrompt: {
            modal_auth_kind_ = static_cast<AuthKind>(wparam);
            show_modal_ = true;
            waiting_for_auth_input_ = true;
            banner_is_error_ = false;
            banner_text_.clear();
            if (modal_auth_kind_ == AuthKind::AppleDiffieHellman && modal_username_.empty()) {
                focused_field_ = ModalField::Username;
            } else {
                focused_field_ = ModalField::Password;
            }
            needs_render_ = true;
            return 0;
        }

        case kWmAppSyncSession: {
            SessionState st = SessionState::Connecting;
            std::string text;
            bool is_err = false;
            bool is_auth_fail = false;
            {
                std::lock_guard<std::mutex> lk(event_mutex_);
                st = latest_session_state_;
                text = latest_status_text_;
                is_err = latest_is_error_;
                is_auth_fail = latest_is_auth_failure_;
                session_event_dirty_ = false;
            }

            if (st == SessionState::Connected) {
                show_modal_ = false;
                waiting_for_auth_input_ = false;
                banner_is_error_ = false;
                banner_text_.clear();
                // Scrub username from memory too once session is authenticated and live!
                SecureWipeString(&modal_username_);
                SecureWipeString(&modal_password_);
                // Push existing Windows text clipboard to Mac unless it was used for Login Modal credentials.
                SyncLocalClipboardToRemote(true);
            } else if (is_err || st == SessionState::Error) {
                show_modal_ = true;
                waiting_for_auth_input_ = true;
                banner_is_error_ = true;
                banner_text_ = text;
                if (is_auth_fail) {
                    focused_field_ = (modal_auth_kind_ == AuthKind::AppleDiffieHellman && modal_username_.empty())
                                         ? ModalField::Username
                                         : ModalField::Password;
                }
            } else if (!waiting_for_auth_input_ && !text.empty()) {
                banner_is_error_ = false;
                banner_text_ = text;
            }
            needs_render_ = true;
            return 0;
        }

        case WM_MOUSEMOVE: {
            const float mx = static_cast<float>(GET_X_LPARAM(lparam));
            const float my = static_cast<float>(GET_Y_LPARAM(lparam));
            mouse_x_ = mx;
            mouse_y_ = my;
            mouse_in_window_ = true;
            mouse_initialized_ = true;
            needs_render_ = true;

            TRACKMOUSEEVENT tme = {};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd_;
            TrackMouseEvent(&tme);

            if (dragging_slider_) {
                SetCursorScaleFromSliderX(mx);
                return 0;
            }

            if (!show_modal_) {
                // Top 6px trigger reveals the Floating Pill HUD smoothly.
                if (my <= 6.0f * dpi_scale_) {
                    hud_wanted_ = true;
                    hud_pointer_entered_ = true;
                    hud_last_active_ms_ = NowMs();
                    hud_peek_duration_ms_ = 1200;
                } else if (hud_wanted_ && hit_regions_.hud_pill.contains(mx, my)) {
                    hud_pointer_entered_ = true;
                    hud_last_active_ms_ = NowMs();
                    hud_peek_duration_ms_ = 1200;
                }
                HandleRemoteMouseMove(mx, my);
            }
            return 0;
        }

        case WM_MOUSELEAVE: {
            if (!options_.prefs.fullscreen) {
                mouse_in_window_ = false;
                needs_render_ = true;
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            SetFocus(hwnd_);
            const float mx = static_cast<float>(GET_X_LPARAM(lparam));
            const float my = static_cast<float>(GET_Y_LPARAM(lparam));
            mouse_x_ = mx;
            mouse_y_ = my;
            mouse_initialized_ = true;
            needs_render_ = true;

            if (show_modal_) {
                if (hit_regions_.modal_gear_button.contains(mx, my)) {
                    show_advanced_settings_ = !show_advanced_settings_;
                    if (show_advanced_settings_ && !waiting_for_auth_input_) {
                        focused_field_ = ModalField::TargetHost;
                    }
                } else if (hit_regions_.modal_username_field.contains(mx, my)) {
                    focused_field_ = ModalField::Username;
                } else if (hit_regions_.modal_password_field.contains(mx, my)) {
                    focused_field_ = ModalField::Password;
                } else if (show_advanced_settings_ && hit_regions_.modal_host_field.contains(mx, my)) {
                    focused_field_ = ModalField::TargetHost;
                } else if (show_advanced_settings_ && hit_regions_.modal_socks5_field.contains(mx, my)) {
                    focused_field_ = ModalField::Socks5;
                } else if (hit_regions_.modal_connect_button.contains(mx, my)) {
                    SubmitLoginModal();
                } else if (hit_regions_.modal_exit_button.contains(mx, my)) {
                    DestroyWindow(hwnd_);
                }
                return 0;
            }

            if (hud_reveal_ > 0.25f && hit_regions_.hud_pill.contains(mx, my)) {
                hud_last_active_ms_ = NowMs();
                hud_pointer_entered_ = true;
                if (hit_regions_.hud_slider_hit_box.contains(mx, my)) {
                    dragging_slider_ = true;
                    SetCapture(hwnd_);
                    SetCursorScaleFromSliderX(mx);
                } else if (hit_regions_.hud_quality_button.contains(mx, my)) {
                    CycleQualityMode();
                } else if (hit_regions_.hud_keymap_button.contains(mx, my)) {
                    ToggleSwapCtrlCmd();
                } else if (hit_regions_.hud_window_button.contains(mx, my)) {
                    ToggleFullscreen();
                } else if (hit_regions_.hud_exit_button.contains(mx, my)) {
                    DestroyWindow(hwnd_);
                }
                return 0;
            }

            key_mapper_.CancelPendingTap();
            SetCapture(hwnd_);
            HandleRemoteMouseButton(mx, my, rfb_button::kLeft, true);
            return 0;
        }

        case WM_LBUTTONUP: {
            const float mx = static_cast<float>(GET_X_LPARAM(lparam));
            const float my = static_cast<float>(GET_Y_LPARAM(lparam));
            if (dragging_slider_) {
                dragging_slider_ = false;
                ReleaseCapture();
                PersistUiPrefs();
                needs_render_ = true;
                return 0;
            }
            if (!show_modal_) {
                HandleRemoteMouseButton(mx, my, rfb_button::kLeft, false);
                if (remote_button_mask_ == 0) {
                    ReleaseCapture();
                }
            }
            return 0;
        }

        case WM_RBUTTONDOWN: {
            if (!show_modal_) {
                const float mx = static_cast<float>(GET_X_LPARAM(lparam));
                const float my = static_cast<float>(GET_Y_LPARAM(lparam));
                if (!(hud_reveal_ > 0.25f && hit_regions_.hud_pill.contains(mx, my))) {
                    key_mapper_.CancelPendingTap();
                    SetCapture(hwnd_);
                    HandleRemoteMouseButton(mx, my, rfb_button::kRight, true);
                }
            }
            return 0;
        }

        case WM_RBUTTONUP: {
            if (!show_modal_) {
                HandleRemoteMouseButton(
                    static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)), rfb_button::kRight, false);
                if (remote_button_mask_ == 0) ReleaseCapture();
            }
            return 0;
        }

        case WM_MBUTTONDOWN: {
            if (!show_modal_) {
                const float mx = static_cast<float>(GET_X_LPARAM(lparam));
                const float my = static_cast<float>(GET_Y_LPARAM(lparam));
                if (!(hud_reveal_ > 0.25f && hit_regions_.hud_pill.contains(mx, my))) {
                    key_mapper_.CancelPendingTap();
                    SetCapture(hwnd_);
                    HandleRemoteMouseButton(mx, my, rfb_button::kMiddle, true);
                }
            }
            return 0;
        }

        case WM_MBUTTONUP: {
            if (!show_modal_) {
                HandleRemoteMouseButton(
                    static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)), rfb_button::kMiddle, false);
                if (remote_button_mask_ == 0) ReleaseCapture();
            }
            return 0;
        }

        case WM_MOUSEWHEEL: {
            POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(hwnd_, &pt);
            HandleRemoteMouseWheel(
                static_cast<float>(pt.x), static_cast<float>(pt.y), GET_WHEEL_DELTA_WPARAM(wparam), false);
            return 0;
        }

        case WM_MOUSEHWHEEL: {
            POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(hwnd_, &pt);
            HandleRemoteMouseWheel(
                static_cast<float>(pt.x), static_cast<float>(pt.y), GET_WHEEL_DELTA_WPARAM(wparam), true);
            return 0;
        }

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool alt  = (GetKeyState(VK_MENU) & 0x8000) != 0;
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

            // Global viewer hotkeys: Ctrl+Alt+Space, Ctrl+Alt+F, Ctrl+Alt+K, Ctrl+Alt+=, Ctrl+Alt+-
            if (ctrl && alt) {
                if (wparam == VK_SPACE) {
                    if (session_) key_mapper_.ReleaseAll(session_.get());
                    if (!show_modal_) {
                        hud_wanted_ = !hud_wanted_;
                        hud_pinned_by_hotkey_ = hud_wanted_;
                        hud_pointer_entered_ = false;
                        hud_last_active_ms_ = NowMs();
                        needs_render_ = true;
                    }
                    return 0;
                }
                if (wparam == 'F') {
                    if (session_) key_mapper_.ReleaseAll(session_.get());
                    ToggleFullscreen();
                    return 0;
                }
                if (wparam == 'K') {
                    ToggleSwapCtrlCmd();
                    return 0;
                }
                if (wparam == VK_OEM_PLUS || wparam == VK_ADD || wparam == '=' || wparam == '+') {
                    if (session_) key_mapper_.ReleaseAll(session_.get());
                    AdjustCursorScale(+0.2f);
                    return 0;
                }
                if (wparam == VK_OEM_MINUS || wparam == VK_SUBTRACT || wparam == '-') {
                    if (session_) key_mapper_.ReleaseAll(session_.get());
                    AdjustCursorScale(-0.2f);
                    return 0;
                }
            }

            if (show_modal_) {
                if (wparam == VK_ESCAPE) {
                    DestroyWindow(hwnd_);
                    return 0;
                }
                if (wparam == VK_RETURN) {
                    SubmitLoginModal();
                    return 0;
                }
                if (wparam == VK_TAB) {
                    AdvanceModalFocus(shift);
                    return 0;
                }
                if (ctrl && wparam == 'V') {
                    PasteClipboardIntoModalField();
                    return 0;
                }
                if ((ctrl && wparam == VK_OEM_COMMA) || wparam == VK_F2) {
                    show_advanced_settings_ = !show_advanced_settings_;
                    needs_render_ = true;
                    return 0;
                }
                if (wparam == VK_BACK) {
                    std::string* buf = ActiveModalFieldBuffer();
                    if (buf && !buf->empty()) {
                        if (ctrl) {
                            SecureWipeString(buf);
                        } else {
                            // Erase trailing UTF-8 codepoint cleanly.
                            while (!buf->empty() && (static_cast<uint8_t>(buf->back()) & 0xC0u) == 0x80u) {
                                buf->back() = '\0';
                                buf->pop_back();
                            }
                            if (!buf->empty()) {
                                buf->back() = '\0';
                                buf->pop_back();
                            }
                        }
                        needs_render_ = true;
                    }
                    return 0;
                }
                return 0;
            }

            if (session_) {
                if (key_mapper_.IsRemotePasteShortcut(wparam)) {
                    SyncLocalClipboardToRemote(false);
                }
                key_mapper_.HandleKeyDown(session_.get(), wparam, lparam, NowMs());
            }
            return 0;
        }

        case WM_KEYUP:
        case WM_SYSKEYUP: {
            if (!show_modal_ && session_) {
                key_mapper_.HandleKeyUp(session_.get(), wparam, lparam, NowMs());
            }
            return 0;
        }

        case WM_SYSCHAR:
            // Prevent Windows Alt+Space / Alt+key system menu popup or MessageBeep ding.
            return 0;

        case WM_SYSCOMMAND:
            if ((wparam & 0xFFF0) == SC_KEYMENU) {
                return 0;
            }
            break;

        case WM_CHAR: {
            if (show_modal_) {
                const wchar_t ch = static_cast<wchar_t>(wparam);
                if (ch >= 0x20 && ch != 0x7F) {
                    std::string* buf = ActiveModalFieldBuffer();
                    if (buf && buf->size() < 256) {
                        std::wstring ws(1, ch);
                        std::string u8 = WideToUtf8String(ws);
                        buf->append(u8);
                        SecureWipeString(&u8);
                        needs_render_ = true;
                    }
                }
                return 0;
            }
            return 0;
        }

        case WM_DESTROY: {
            if (clipboard_listener_registered_) {
                RemoveClipboardFormatListener(hwnd_);
                clipboard_listener_registered_ = false;
            }
            ClearRemoteClipboardOnExit();
            StopSession();
            PostQuitMessage(0);
            return 0;
        }

        default:
            break;
    }

    return DefWindowProcW(hwnd_, msg, wparam, lparam);
}

void ViewerApp::UpdateAnimationsAndTimers(uint64_t now_ms) {
    if (last_anim_tick_ms_ == 0) {
        last_anim_tick_ms_ = now_ms;
    }
    const float dt = std::clamp(static_cast<float>(now_ms - last_anim_tick_ms_) * 0.001f, 0.0f, 0.1f);
    last_anim_tick_ms_ = now_ms;

    // Sync real OS pointer position if moved externally (e.g. SendInput / SetCursorPos in automation)
    if (hwnd_ && !show_modal_) {
        POINT os_pt = {};
        if (GetCursorPos(&os_pt) && ScreenToClient(hwnd_, &os_pt)) {
            if (os_pt.x >= 0 && os_pt.y >= 0 &&
                os_pt.x < static_cast<LONG>(renderer_.width()) &&
                os_pt.y < static_cast<LONG>(renderer_.height())) {
                const float nx = static_cast<float>(os_pt.x);
                const float ny = static_cast<float>(os_pt.y);
                if (std::fabs(nx - mouse_x_) >= 0.5f || std::fabs(ny - mouse_y_) >= 0.5f) {
                    mouse_x_ = nx;
                    mouse_y_ = ny;
                    mouse_in_window_ = true;
                    if (ny <= 6.0f * dpi_scale_) {
                        hud_wanted_ = true;
                        hud_pointer_entered_ = true;
                        hud_last_active_ms_ = now_ms;
                        hud_peek_duration_ms_ = 1200;
                    }
                    needs_render_ = true;
                }
            }
        }
    }

    // Retry deferred Mac -> Windows clipboard write if OpenClipboard was momentarily locked.
    if (pending_clipboard_retries_ > 0 && now_ms >= next_clipboard_retry_ms_) {
        ApplyPendingServerClipboard();
    }

    // Check if new RFB frame arrived
    if (session_ && session_->GetFrameGeneration() != last_rendered_frame_gen_) {
        needs_render_ = true;
    }

    // Auto-hide Floating Pill HUD (~1.2s after pointer leaves HUD)
    if (!show_modal_ && hud_wanted_ && !dragging_slider_) {
        const bool hovering_hud = (mouse_y_ <= 8.0f * dpi_scale_) ||
                                  hit_regions_.hud_pill.contains(mouse_x_, mouse_y_);
        if (hovering_hud) {
            hud_pointer_entered_ = true;
            hud_last_active_ms_ = now_ms;
        } else if (!hud_pinned_by_hotkey_ || hud_pointer_entered_) {
            if (now_ms >= hud_last_active_ms_ + hud_peek_duration_ms_) {
                hud_wanted_ = false;
                hud_pinned_by_hotkey_ = false;
                hud_pointer_entered_ = false;
                needs_render_ = true;
            }
        }
    }

    const float target_reveal = (!show_modal_ && hud_wanted_) ? 1.0f : 0.0f;
    if (std::fabs(hud_reveal_ - target_reveal) > 0.004f) {
        const float step = dt / 0.17f;
        if (hud_reveal_ < target_reveal) {
            hud_reveal_ = std::min(target_reveal, hud_reveal_ + step);
        } else {
            hud_reveal_ = std::max(target_reveal, hud_reveal_ - step);
        }
        needs_render_ = true;
    }

    if (renderer_.AdvanceScrollAnimation(now_ms)) {
        needs_render_ = true;
    }

    // Refresh HUD FPS/RTT telemetry every 250ms while HUD is visible, or blink modal caret every 500ms.
    const uint64_t refresh_period = show_modal_ ? 500u : ((hud_reveal_ > 0.01f) ? 250u : 0u);
    if (refresh_period > 0 && now_ms - last_stats_refresh_ms_ >= refresh_period) {
        last_stats_refresh_ms_ = now_ms;
        needs_render_ = true;
    }
}

void ViewerApp::RenderNow() {
    if (!hwnd_) return;

    const uint64_t now_ms = NowMs();
    renderer_.SyncFramebuffer(session_.get(), now_ms);
    if (session_) {
        last_rendered_frame_gen_ = session_->GetFrameGeneration();
    }

    RenderUiState ui;
    ui.dpi_scale = dpi_scale_;
    ui.fullscreen = options_.prefs.fullscreen;
    ui.swap_ctrl_cmd = options_.prefs.swap_ctrl_cmd;
    ui.cursor_scale = options_.prefs.cursor_scale;
    ui.mouse_x = mouse_x_;
    ui.mouse_y = mouse_y_;
    ui.mouse_in_window = mouse_in_window_;

    if (session_) {
        ui.stats = session_->GetStats();
        ui.session_state = ui.stats.state;
    } else {
        ui.session_state = SessionState::Disconnected;
        ui.stats.quality_mode = options_.prefs.quality_mode;
    }

    ui.show_modal = show_modal_;
    ui.auth_kind = modal_auth_kind_;
    ui.waiting_for_auth_input = waiting_for_auth_input_;
    ui.show_advanced_settings = show_advanced_settings_;
    ui.focused_field = focused_field_;
    ui.caret_visible = ((NowMs() / 500u) % 2u) == 0u;
    ui.username_text = modal_username_;
    ui.password_char_count = modal_password_.size();
    ui.target_host_text = modal_host_text_;
    ui.socks5_text = modal_socks5_text_;
    ui.status_or_error_text = banner_text_;
    ui.is_error_banner = banner_is_error_;
    ui.hud_reveal = hud_reveal_;
    ui.dragging_cursor_slider = dragging_slider_;

    renderer_.RenderFrame(ui, &hit_regions_);
    needs_render_ = false;
    last_present_ms_ = NowMs();
}

int ViewerApp::Run(HINSTANCE hinstance) {
    if (!CreateMainWindow(hinstance)) {
        MessageBoxW(nullptr,
                    L"Direct3D 11 / Direct2D 초기화에 실패했습니다.",
                    L"Mac Mirror Viewer",
                    MB_ICONERROR | MB_OK);
        return 1;
    }

    // Persist initial non-sensitive UI preferences if explicitly passed via CLI.
    if (options_.cursor_scale_set || options_.quality_set ||
        options_.fullscreen_set || options_.swap_ctrl_cmd_set) {
        PersistUiPrefs();
    }

    // Render initial dark backdrop + connecting card immediately before network handshake begins!
    RenderNow();

    // Immediately initiate connection on startup (One-Click UX).
    StartSession();

    MSG msg = {};
    bool running = true;
    while (running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;

        const uint64_t now_ms = NowMs();
        UpdateAnimationsAndTimers(now_ms);

        if (needs_render_) {
            // On software WARP adapter, cap redraws to ~60Hz (16ms) so RFB decode gets full CPU headroom.
            if (renderer_.is_warp_driver() && now_ms - last_present_ms_ < 15u) {
                const DWORD rem = static_cast<DWORD>(15u - (now_ms - last_present_ms_));
                MsgWaitForMultipleObjects(0, nullptr, FALSE, rem, QS_ALLINPUT);
                continue;
            }
            RenderNow();
            continue;
        }

        const bool animating =
            std::fabs(hud_reveal_ - ((!show_modal_ && hud_wanted_) ? 1.0f : 0.0f)) > 0.004f ||
            renderer_.IsScrollAnimating();
        DWORD wait_ms = 200;
        if (animating) {
            wait_ms = renderer_.IsScrollAnimating() ? 8 : 12;
        } else if (pending_clipboard_retries_ > 0) {
            wait_ms = 50;
        } else if (!show_modal_ && hud_wanted_) {
            wait_ms = 60;
        } else if (show_modal_) {
            wait_ms = 120;
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, wait_ms, QS_ALLINPUT);
    }

    StopSession();
    return static_cast<int>(msg.wParam);
}

}  // namespace mac_mirror
#endif
