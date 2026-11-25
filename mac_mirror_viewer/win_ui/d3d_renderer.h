#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <dwrite.h>

#include <cstdint>
#include <string>
#include <vector>

#include "rfb_session.h"

namespace mac_mirror {

enum class ModalField : int {
    Username   = 0,
    Password   = 1,
    TargetHost = 2,
    Socks5     = 3,
};

struct RectF {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    float width() const noexcept { return right - left; }
    float height() const noexcept { return bottom - top; }
    bool empty() const noexcept { return right <= left || bottom <= top; }
    bool contains(float x, float y) const noexcept {
        return !empty() && x >= left && x <= right && y >= top && y <= bottom;
    }
};

struct RenderUiState {
    float dpi_scale = 1.0f;
    bool fullscreen = true;
    bool swap_ctrl_cmd = true;
    float cursor_scale = 1.8f;

    float mouse_x = 0.0f;
    float mouse_y = 0.0f;
    bool mouse_in_window = true;

    SessionState session_state = SessionState::Connecting;
    SessionStats stats;

    // Login / Connection Modal
    bool show_modal = true;
    AuthKind auth_kind = AuthKind::AppleDiffieHellman;
    bool waiting_for_auth_input = false;
    bool show_advanced_settings = false;
    ModalField focused_field = ModalField::Username;
    bool caret_visible = true;

    std::string username_text;
    size_t password_char_count = 0;  // Only count passed to renderer; secret stays out of UI layer
    std::string target_host_text;
    std::string socks5_text;
    std::string status_or_error_text;
    bool is_error_banner = false;

    // Auto-hide Floating Pill HUD (0.0 = hidden, 1.0 = fully visible)
    float hud_reveal = 0.0f;
    bool dragging_cursor_slider = false;
};

struct UiHitRegions {
    RectF remote_viewport;

    RectF modal_card;
    RectF modal_gear_button;
    RectF modal_username_field;
    RectF modal_password_field;
    RectF modal_host_field;
    RectF modal_socks5_field;
    RectF modal_connect_button;
    RectF modal_exit_button;

    RectF hud_pill;
    RectF hud_slider_track;
    RectF hud_slider_hit_box;
    RectF hud_quality_button;
    RectF hud_keymap_button;
    RectF hud_window_button;
    RectF hud_exit_button;
};

class D3DRenderer {
public:
    D3DRenderer() = default;
    ~D3DRenderer();

    D3DRenderer(const D3DRenderer&) = delete;
    D3DRenderer& operator=(const D3DRenderer&) = delete;

    bool Initialize(HWND hwnd, uint32_t width, uint32_t height, float dpi_scale);
    void Shutdown();

    bool Resize(uint32_t width, uint32_t height, float dpi_scale);

    // Uploads any new dirty rectangles from session if GetFrameGeneration() advanced.
    void SyncFramebuffer(RfbSession* session);

    // Renders background, remote Mac texture, Login Modal / Floating Pill HUD, and vector macOS cursor.
    // Returns true if Present succeeded without occlusion.
    bool RenderFrame(const RenderUiState& state, UiHitRegions* out_regions);

    uint32_t width() const noexcept { return width_; }
    uint32_t height() const noexcept { return height_; }
    uint32_t remote_fb_width() const noexcept { return fb_width_; }
    uint32_t remote_fb_height() const noexcept { return fb_height_; }
    bool is_warp_driver() const noexcept { return is_warp_; }

    static RectF ComputeAspectViewport(uint32_t client_w,
                                       uint32_t client_h,
                                       uint32_t fb_w,
                                       uint32_t fb_h) noexcept;

private:
    bool CreateSwapChainResources();
    void ReleaseSwapChainResources();
    bool EnsureTextFormats(float dpi_scale);
    bool BuildCursorGeometry();

    void DrawLoginModal(const RenderUiState& state, UiHitRegions* regions);
    void DrawGearIcon(float cx, float cy, float radius, const D2D1_COLOR_F& color, float stroke_w);
    void DrawInputField(const RectF& box,
                        const std::wstring& label,
                        const std::wstring& value_or_placeholder,
                        bool is_placeholder,
                        bool focused,
                        bool show_caret,
                        float dpi_scale);
    void DrawFloatingHud(const RenderUiState& state, UiHitRegions* regions);
    void DrawMacVectorCursor(float x, float y, float scale);

    void DrawTextLine(const std::wstring& text,
                      IDWriteTextFormat* fmt,
                      const RectF& rect,
                      const D2D1_COLOR_F& color,
                      DWRITE_TEXT_ALIGNMENT h_align = DWRITE_TEXT_ALIGNMENT_LEADING,
                      DWRITE_PARAGRAPH_ALIGNMENT v_align = DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    HWND hwnd_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool is_warp_ = false;

    ID3D11Device* d3d_device_ = nullptr;
    ID3D11DeviceContext* d3d_context_ = nullptr;
    IDXGISwapChain* swap_chain_ = nullptr;

    ID2D1Factory* d2d_factory_ = nullptr;
    ID2D1RenderTarget* d2d_rt_ = nullptr;
    ID2D1DeviceContext* d2d_ctx_ = nullptr;
    ID2D1SolidColorBrush* brush_ = nullptr;
    ID2D1StrokeStyle* round_stroke_ = nullptr;
    ID2D1PathGeometry* mac_arrow_geom_ = nullptr;

    ID2D1Bitmap* frame_bitmap_ = nullptr;
    uint32_t fb_width_ = 0;
    uint32_t fb_height_ = 0;
    uint64_t uploaded_frame_gen_ = 0;

    IDWriteFactory* dwrite_factory_ = nullptr;
    float text_dpi_scale_ = 0.0f;
    IDWriteTextFormat* fmt_title_ = nullptr;
    IDWriteTextFormat* fmt_subtitle_ = nullptr;
    IDWriteTextFormat* fmt_label_ = nullptr;
    IDWriteTextFormat* fmt_input_ = nullptr;
    IDWriteTextFormat* fmt_button_ = nullptr;
    IDWriteTextFormat* fmt_hud_ = nullptr;
    IDWriteTextFormat* fmt_hud_bold_ = nullptr;
    IDWriteTextFormat* fmt_small_ = nullptr;
};

}  // namespace mac_mirror
#endif
