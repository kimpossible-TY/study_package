#include "d3d_renderer.h"

#ifdef _WIN32

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "cli_options.h"

namespace mac_mirror {

namespace {

template <typename T>
void SafeRelease(T*& ptr) noexcept {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

D2D1_COLOR_F RgbaHex(uint32_t rgb, float alpha = 1.0f) noexcept {
    const float r = static_cast<float>((rgb >> 16) & 0xFFu) / 255.0f;
    const float g = static_cast<float>((rgb >> 8) & 0xFFu) / 255.0f;
    const float b = static_cast<float>(rgb & 0xFFu) / 255.0f;
    return D2D1::ColorF(r, g, b, alpha);
}

D2D1_RECT_F ToD2DRect(const RectF& r) noexcept {
    return D2D1::RectF(r.left, r.top, r.right, r.bottom);
}

std::wstring Utf8ToWideLocal(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], n);
    return out;
}

}  // namespace

D3DRenderer::~D3DRenderer() {
    Shutdown();
}

RectF D3DRenderer::ComputeAspectViewport(uint32_t client_w,
                                         uint32_t client_h,
                                         uint32_t fb_w,
                                         uint32_t fb_h) noexcept {
    const float cw = static_cast<float>(std::max(1u, client_w));
    const float ch = static_cast<float>(std::max(1u, client_h));
    // Default to Mac's native 16:10 aspect ratio before first ServerInit arrives.
    const float target_aspect = (fb_w > 0 && fb_h > 0)
                                    ? (static_cast<float>(fb_w) / static_cast<float>(fb_h))
                                    : (16.0f / 10.0f);
    const float win_aspect = cw / ch;

    float vp_w = cw;
    float vp_h = ch;
    if (win_aspect > target_aspect) {
        vp_h = ch;
        vp_w = vp_h * target_aspect;
    } else {
        vp_w = cw;
        vp_h = vp_w / target_aspect;
    }

    const float left = std::round((cw - vp_w) * 0.5f);
    const float top  = std::round((ch - vp_h) * 0.5f);
    return RectF{left, top, left + std::round(vp_w), top + std::round(vp_h)};
}

bool D3DRenderer::Initialize(HWND hwnd, uint32_t width, uint32_t height, float dpi_scale) {
    Shutdown();
    hwnd_ = hwnd;
    width_ = std::max(1u, width);
    height_ = std::max(1u, height);

    HRESULT hr = D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr, reinterpret_cast<void**>(&d2d_factory_));
    if (FAILED(hr) || !d2d_factory_) {
        return false;
    }

    hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&dwrite_factory_));
    if (FAILED(hr) || !dwrite_factory_) {
        return false;
    }

    D2D1_STROKE_STYLE_PROPERTIES stroke_props = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_ROUND,
        D2D1_CAP_STYLE_ROUND,
        D2D1_CAP_STYLE_ROUND,
        D2D1_LINE_JOIN_ROUND,
        2.0f,
        D2D1_DASH_STYLE_SOLID,
        0.0f);
    hr = d2d_factory_->CreateStrokeStyle(stroke_props, nullptr, 0, &round_stroke_);
    if (FAILED(hr)) {
        return false;
    }

    if (!BuildCursorGeometry()) {
        return false;
    }

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = width_;
    sd.BufferDesc.Height = height_;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd_;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;  // Borderless fullscreen uses WS_POPUP + Windowed=TRUE for zero-flicker DWM & CopyFromScreen
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL created_level = D3D_FEATURE_LEVEL_11_0;
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    hr = D3D11CreateDeviceAndSwapChain(nullptr,
                                       D3D_DRIVER_TYPE_HARDWARE,
                                       nullptr,
                                       flags,
                                       levels,
                                       static_cast<UINT>(sizeof(levels) / sizeof(levels[0])),
                                       D3D11_SDK_VERSION,
                                       &sd,
                                       &swap_chain_,
                                       &d3d_device_,
                                       &created_level,
                                       &d3d_context_);
    if (FAILED(hr)) {
        // Fallback to WARP software rasterizer on headless / cloud VMs without hardware 3D adapter.
        hr = D3D11CreateDeviceAndSwapChain(nullptr,
                                           D3D_DRIVER_TYPE_WARP,
                                           nullptr,
                                           flags,
                                           levels,
                                           static_cast<UINT>(sizeof(levels) / sizeof(levels[0])),
                                           D3D11_SDK_VERSION,
                                           &sd,
                                           &swap_chain_,
                                           &d3d_device_,
                                           &created_level,
                                           &d3d_context_);
        if (FAILED(hr)) {
            return false;
        }
        is_warp_ = true;
    } else {
        IDXGIDevice* dxgi_dev = nullptr;
        if (SUCCEEDED(d3d_device_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_dev))) &&
            dxgi_dev) {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgi_dev->GetAdapter(&adapter)) && adapter) {
                DXGI_ADAPTER_DESC desc = {};
                if (SUCCEEDED(adapter->GetDesc(&desc))) {
                    // Microsoft Basic Render Driver (VendorId 0x1414, DeviceId 0x008C)
                    if (desc.VendorId == 0x1414 && desc.DeviceId == 0x008C) {
                        is_warp_ = true;
                    }
                }
                adapter->Release();
            }
            dxgi_dev->Release();
        }
    }

    // Prevent DXGI from intercepting Alt+Enter exclusive fullscreen transitions.
    IDXGIDevice* dxgi_device = nullptr;
    if (SUCCEEDED(d3d_device_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_device))) &&
        dxgi_device) {
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(dxgi_device->GetAdapter(&adapter)) && adapter) {
            IDXGIFactory* dxgi_factory = nullptr;
            if (SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&dxgi_factory))) &&
                dxgi_factory) {
                dxgi_factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
                dxgi_factory->Release();
            }
            adapter->Release();
        }
        dxgi_device->Release();
    }

    if (!CreateSwapChainResources()) {
        return false;
    }
    if (!EnsureTextFormats(dpi_scale)) {
        return false;
    }
    return true;
}

void D3DRenderer::Shutdown() {
    ReleaseSwapChainResources();

    SafeRelease(fmt_title_);
    SafeRelease(fmt_subtitle_);
    SafeRelease(fmt_label_);
    SafeRelease(fmt_input_);
    SafeRelease(fmt_button_);
    SafeRelease(fmt_hud_);
    SafeRelease(fmt_hud_bold_);
    SafeRelease(fmt_small_);
    text_dpi_scale_ = 0.0f;

    SafeRelease(mac_arrow_geom_);
    SafeRelease(round_stroke_);
    SafeRelease(dwrite_factory_);
    SafeRelease(d2d_factory_);

    SafeRelease(swap_chain_);
    SafeRelease(d3d_context_);
    SafeRelease(d3d_device_);
}

bool D3DRenderer::CreateSwapChainResources() {
    if (!swap_chain_ || !d2d_factory_) return false;

    IDXGISurface* dxgi_surface = nullptr;
    HRESULT hr = swap_chain_->GetBuffer(0, __uuidof(IDXGISurface), reinterpret_cast<void**>(&dxgi_surface));
    if (FAILED(hr) || !dxgi_surface) {
        return false;
    }

    // Use explicit 96 DPI on D2D render target so coordinates match 1:1 physical client pixels.
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f,
        96.0f);

    hr = d2d_factory_->CreateDxgiSurfaceRenderTarget(dxgi_surface, &props, &d2d_rt_);
    dxgi_surface->Release();
    if (FAILED(hr) || !d2d_rt_) {
        return false;
    }

    d2d_rt_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    d2d_rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);

    // Optional Direct2D 1.1 DeviceContext for multi-sample / anisotropic Retina texture scaling.
    d2d_rt_->QueryInterface(__uuidof(ID2D1DeviceContext), reinterpret_cast<void**>(&d2d_ctx_));

    hr = d2d_rt_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &brush_);
    if (FAILED(hr) || !brush_) {
        return false;
    }

    // Force remote texture re-upload onto new D2D render target on next frame.
    uploaded_frame_gen_ = 0;
    return true;
}

void D3DRenderer::ReleaseSwapChainResources() {
    SafeRelease(frame_bitmap_);
    SafeRelease(brush_);
    SafeRelease(d2d_ctx_);
    SafeRelease(d2d_rt_);
    uploaded_frame_gen_ = 0;
}

bool D3DRenderer::Resize(uint32_t width, uint32_t height, float dpi_scale) {
    if (!swap_chain_) return false;
    const uint32_t new_w = std::max(1u, width);
    const uint32_t new_h = std::max(1u, height);

    EnsureTextFormats(dpi_scale);
    if (new_w == width_ && new_h == height_ && d2d_rt_) {
        return true;
    }

    ReleaseSwapChainResources();
    if (d3d_context_) {
        d3d_context_->ClearState();
        d3d_context_->Flush();
    }

    HRESULT hr = swap_chain_->ResizeBuffers(0, new_w, new_h, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        return false;
    }

    width_ = new_w;
    height_ = new_h;
    return CreateSwapChainResources();
}

bool D3DRenderer::EnsureTextFormats(float dpi_scale) {
    if (!dwrite_factory_) return false;
    const float s = std::clamp(dpi_scale, 0.75f, 4.0f);
    if (std::fabs(s - text_dpi_scale_) < 0.01f && fmt_title_) {
        return true;
    }

    SafeRelease(fmt_title_);
    SafeRelease(fmt_subtitle_);
    SafeRelease(fmt_label_);
    SafeRelease(fmt_input_);
    SafeRelease(fmt_button_);
    SafeRelease(fmt_hud_);
    SafeRelease(fmt_hud_bold_);
    SafeRelease(fmt_small_);

    auto make_fmt = [&](float px, DWRITE_FONT_WEIGHT weight, IDWriteTextFormat** out) -> bool {
        HRESULT hr = dwrite_factory_->CreateTextFormat(L"Segoe UI",
                                                       nullptr,
                                                       weight,
                                                       DWRITE_FONT_STYLE_NORMAL,
                                                       DWRITE_FONT_STRETCH_NORMAL,
                                                       px * s,
                                                       L"ko-KR",
                                                       out);
        if (FAILED(hr) || !*out) return false;
        (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        return true;
    };

    if (!make_fmt(20.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &fmt_title_)) return false;
    if (!make_fmt(12.5f, DWRITE_FONT_WEIGHT_REGULAR, &fmt_subtitle_)) return false;
    if (!make_fmt(12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &fmt_label_)) return false;
    if (!make_fmt(14.0f, DWRITE_FONT_WEIGHT_REGULAR, &fmt_input_)) return false;
    if (!make_fmt(14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &fmt_button_)) return false;
    if (!make_fmt(12.5f, DWRITE_FONT_WEIGHT_MEDIUM, &fmt_hud_)) return false;
    if (!make_fmt(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &fmt_hud_bold_)) return false;
    if (!make_fmt(11.0f, DWRITE_FONT_WEIGHT_REGULAR, &fmt_small_)) return false;

    text_dpi_scale_ = s;
    return true;
}

bool D3DRenderer::BuildCursorGeometry() {
    if (!d2d_factory_) return false;
    SafeRelease(mac_arrow_geom_);

    HRESULT hr = d2d_factory_->CreatePathGeometry(&mac_arrow_geom_);
    if (FAILED(hr) || !mac_arrow_geom_) return false;

    ID2D1GeometrySink* sink = nullptr;
    hr = mac_arrow_geom_->Open(&sink);
    if (FAILED(hr) || !sink) return false;

    // Authentic macOS pointer silhouette with tip hotspot at (0, 0), base height ~19.5px at 1.0x.
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);
    sink->BeginFigure(D2D1::Point2F(0.0f, 0.0f), D2D1_FIGURE_BEGIN_FILLED);
    const D2D1_POINT_2F pts[] = {
        D2D1::Point2F(0.0f, 17.2f),
        D2D1::Point2F(4.3f, 13.1f),
        D2D1::Point2F(7.2f, 19.6f),
        D2D1::Point2F(9.6f, 18.5f),
        D2D1::Point2F(6.7f, 12.1f),
        D2D1::Point2F(12.4f, 12.1f),
    };
    sink->AddLines(pts, static_cast<UINT32>(sizeof(pts) / sizeof(pts[0])));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    hr = sink->Close();
    sink->Release();
    return SUCCEEDED(hr);
}

void D3DRenderer::SyncFramebuffer(RfbSession* session, uint64_t now_ms) {
    if (!session || !d2d_rt_) return;

    const uint64_t latest_gen = session->GetFrameGeneration();
    if (latest_gen == 0 || (latest_gen == uploaded_frame_gen_ && frame_bitmap_ != nullptr)) {
        return;
    }

    std::vector<Rect> dirty_rects;
    bool resized = false;
    FramebufferLock fb = session->LockFramebuffer(&dirty_rects, &resized);
    if (!fb.pixels_bgra || fb.width == 0 || fb.height == 0 || fb.stride_bytes < fb.width * 4u) {
        return;
    }

    const size_t expected_bytes = static_cast<size_t>(fb.height) * fb.stride_bytes;
    const bool need_recreate = (!frame_bitmap_ || resized || fb.width != fb_width_ || fb.height != fb_height_);
    if (need_recreate) {
        SafeRelease(frame_bitmap_);
        scroll_interp_.ResetAnimation();
        D2D1_BITMAP_PROPERTIES bmp_props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
        HRESULT hr = d2d_rt_->CreateBitmap(
            D2D1::SizeU(fb.width, fb.height), fb.pixels_bgra, fb.stride_bytes, &bmp_props, &frame_bitmap_);
        if (SUCCEEDED(hr) && frame_bitmap_) {
            fb_width_ = fb.width;
            fb_height_ = fb.height;
            uploaded_frame_gen_ = fb.generation;
            prev_fb_bgra_.assign(fb.pixels_bgra, fb.pixels_bgra + expected_bytes);
        }
        return;
    }

    // Compute dirty bounding box before uploading so wheel-scroll translation detection
    // can compare prev_fb_bgra_ against the new framebuffer strictly within the changed area.
    ScrollRegion dirty_bounds{};
    if (dirty_rects.empty()) {
        dirty_bounds = ScrollRegion{0, 0, fb.width, fb.height};
    } else {
        uint32_t min_x = fb.width;
        uint32_t min_y = fb.height;
        uint32_t max_x = 0;
        uint32_t max_y = 0;
        for (const Rect& r : dirty_rects) {
            if (r.w == 0 || r.h == 0 || r.x >= fb.width || r.y >= fb.height) continue;
            const uint32_t right = std::min<uint32_t>(fb.width, static_cast<uint32_t>(r.x) + r.w);
            const uint32_t bottom = std::min<uint32_t>(fb.height, static_cast<uint32_t>(r.y) + r.h);
            if (right <= r.x || bottom <= r.y) continue;
            min_x = std::min<uint32_t>(min_x, r.x);
            min_y = std::min<uint32_t>(min_y, r.y);
            max_x = std::max<uint32_t>(max_x, right);
            max_y = std::max<uint32_t>(max_y, bottom);
        }
        if (max_x > min_x && max_y > min_y) {
            dirty_bounds = ScrollRegion{min_x, min_y, max_x - min_x, max_y - min_y};
        }
    }

    if (prev_fb_bgra_.size() == expected_bytes && dirty_bounds.w > 0 && dirty_bounds.h > 0) {
        scroll_interp_.OnFrameUpdated(
            now_ms, prev_fb_bgra_.data(), fb.pixels_bgra, fb.width, fb.height, fb.stride_bytes, dirty_bounds);
    }

    if (dirty_rects.empty()) {
        D2D1_RECT_U full_rect = D2D1::RectU(0, 0, fb.width, fb.height);
        frame_bitmap_->CopyFromMemory(&full_rect, fb.pixels_bgra, fb.stride_bytes);
        prev_fb_bgra_.assign(fb.pixels_bgra, fb.pixels_bgra + expected_bytes);
    } else {
        const bool update_prev = (prev_fb_bgra_.size() == expected_bytes);
        for (const Rect& r : dirty_rects) {
            if (r.w == 0 || r.h == 0 || r.x >= fb.width || r.y >= fb.height) continue;
            const uint32_t right = std::min<uint32_t>(fb.width, static_cast<uint32_t>(r.x) + r.w);
            const uint32_t bottom = std::min<uint32_t>(fb.height, static_cast<uint32_t>(r.y) + r.h);
            if (right <= r.x || bottom <= r.y) continue;

            D2D1_RECT_U dst = D2D1::RectU(r.x, r.y, right, bottom);
            const uint8_t* src_ptr =
                fb.pixels_bgra + static_cast<size_t>(r.y) * fb.stride_bytes + static_cast<size_t>(r.x) * 4u;
            frame_bitmap_->CopyFromMemory(&dst, src_ptr, fb.stride_bytes);

            if (update_prev) {
                const size_t row_bytes = static_cast<size_t>(right - r.x) * 4u;
                for (uint32_t row = r.y; row < bottom; ++row) {
                    const size_t off = static_cast<size_t>(row) * fb.stride_bytes + static_cast<size_t>(r.x) * 4u;
                    std::memcpy(prev_fb_bgra_.data() + off, fb.pixels_bgra + off, row_bytes);
                }
            }
        }
        if (!update_prev) {
            prev_fb_bgra_.assign(fb.pixels_bgra, fb.pixels_bgra + expected_bytes);
        }
    }
    uploaded_frame_gen_ = fb.generation;
}

bool D3DRenderer::RenderFrame(const RenderUiState& state, UiHitRegions* out_regions) {
    if (!d2d_rt_ || !swap_chain_ || !brush_) return false;
    EnsureTextFormats(state.dpi_scale);

    UiHitRegions regions = {};
    regions.remote_viewport = ComputeAspectViewport(width_, height_, fb_width_, fb_height_);

    d2d_rt_->BeginDraw();
    d2d_rt_->SetTransform(D2D1::Matrix3x2F::Identity());

    // #0B0D12 deep obsidian background
    d2d_rt_->Clear(RgbaHex(0x0B0D12, 1.0f));

    if (frame_bitmap_ && fb_width_ > 0 && fb_height_ > 0) {
        const D2D1_RECT_F dst_rect = ToD2DRect(regions.remote_viewport);
        const D2D1_RECT_F src_rect =
            D2D1::RectF(0.0f, 0.0f, static_cast<float>(fb_width_), static_cast<float>(fb_height_));

        auto draw_bmp = [&](const D2D1_RECT_F& dst, const D2D1_RECT_F& src) {
            if (d2d_ctx_) {
                const bool is_downscaling = (regions.remote_viewport.width() < static_cast<float>(fb_width_));
                const D2D1_INTERPOLATION_MODE interp = is_downscaling
                                                           ? D2D1_INTERPOLATION_MODE_MULTI_SAMPLE_LINEAR
                                                           : D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC;
                d2d_ctx_->DrawBitmap(frame_bitmap_, &dst, 1.0f, interp, &src, nullptr);
            } else {
                d2d_rt_->DrawBitmap(
                    frame_bitmap_, dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, src);
            }
        };

        // 1. Always draw the full crisp framebuffer first (fixed UI + newly revealed edge strip).
        draw_bmp(dst_rect, src_rect);

        // 2. If a wheel-triggered vertical scroll glide is active, smoothly slide ONLY the
        //    scrolled region inside a tight clip rect at 60/120Hz with 100% original text pixels.
        const ScrollVisualState& sv = scroll_interp_.state();
        if (!state.show_modal && sv.active && std::fabs(sv.offset_y) >= ScrollMotionInterpolator::kSettleThresholdPx &&
            sv.region.w > 0 && sv.region.h > 0) {
            const float sx = regions.remote_viewport.width() / static_cast<float>(fb_width_);
            const float sy = regions.remote_viewport.height() / static_cast<float>(fb_height_);
            const D2D1_RECT_F clip_dst = D2D1::RectF(
                regions.remote_viewport.left + static_cast<float>(sv.region.x) * sx,
                regions.remote_viewport.top + static_cast<float>(sv.region.y) * sy,
                regions.remote_viewport.left + static_cast<float>(sv.region.x + sv.region.w) * sx,
                regions.remote_viewport.top + static_cast<float>(sv.region.y + sv.region.h) * sy);
            const float dy_vp = sv.offset_y * sy;
            const D2D1_RECT_F shifted_dst = D2D1::RectF(
                clip_dst.left,
                clip_dst.top + dy_vp,
                clip_dst.right,
                clip_dst.bottom + dy_vp);
            const D2D1_RECT_F region_src = D2D1::RectF(
                static_cast<float>(sv.region.x),
                static_cast<float>(sv.region.y),
                static_cast<float>(sv.region.x + sv.region.w),
                static_cast<float>(sv.region.y + sv.region.h));

            d2d_rt_->PushAxisAlignedClip(clip_dst, D2D1_ANTIALIAS_MODE_ALIASED);
            draw_bmp(shifted_dst, region_src);
            d2d_rt_->PopAxisAlignedClip();
        }
    }

    if (state.show_modal) {
        // Subtle dim vignette if frame existed behind modal
        if (frame_bitmap_) {
            brush_->SetColor(RgbaHex(0x080A0F, 0.72f));
            d2d_rt_->FillRectangle(
                D2D1::RectF(0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_)), brush_);
        }
        DrawLoginModal(state, &regions);
    }

    if (!state.show_modal && state.hud_reveal > 0.005f) {
        DrawFloatingHud(state, &regions);
    }

    // Zero-latency vector macOS arrow pointer rendered directly into SwapChain at 60/120Hz!
    if (!state.show_modal && state.mouse_in_window) {
        const float effective_scale = ClampCursorScale(state.cursor_scale) * std::clamp(state.dpi_scale, 0.75f, 4.0f);
        DrawMacVectorCursor(state.mouse_x, state.mouse_y, effective_scale);
    }

    HRESULT hr = d2d_rt_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        ReleaseSwapChainResources();
        CreateSwapChainResources();
        if (out_regions) *out_regions = regions;
        return false;
    }

    if (out_regions) {
        *out_regions = regions;
    }

    const UINT sync_interval = is_warp_ ? 0u : 1u;
    HRESULT present_hr = swap_chain_->Present(sync_interval, 0);
    return present_hr != DXGI_STATUS_OCCLUDED && SUCCEEDED(present_hr);
}

void D3DRenderer::DrawTextLine(const std::wstring& text,
                               IDWriteTextFormat* fmt,
                               const RectF& rect,
                               const D2D1_COLOR_F& color,
                               DWRITE_TEXT_ALIGNMENT h_align,
                               DWRITE_PARAGRAPH_ALIGNMENT v_align) {
    if (!d2d_rt_ || !brush_ || !fmt || text.empty() || rect.empty()) return;
    fmt->SetTextAlignment(h_align);
    fmt->SetParagraphAlignment(v_align);
    brush_->SetColor(color);
    d2d_rt_->DrawText(
        text.c_str(), static_cast<UINT32>(text.size()), fmt, ToD2DRect(rect), brush_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void D3DRenderer::DrawGearIcon(float cx,
                               float cy,
                               float radius,
                               const D2D1_COLOR_F& color,
                               float stroke_w) {
    if (!d2d_rt_ || !brush_) return;
    brush_->SetColor(color);

    const float body_r = radius * 0.68f;
    const float hole_r = radius * 0.30f;
    constexpr int kTeeth = 8;
    constexpr float kPi = 3.14159265f;

    for (int i = 0; i < kTeeth; ++i) {
        const float a = static_cast<float>(i) * (2.0f * kPi / static_cast<float>(kTeeth));
        const float x0 = cx + std::cos(a) * (body_r - stroke_w * 0.3f);
        const float y0 = cy + std::sin(a) * (body_r - stroke_w * 0.3f);
        const float x1 = cx + std::cos(a) * radius;
        const float y1 = cy + std::sin(a) * radius;
        d2d_rt_->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), brush_, stroke_w * 1.35f, round_stroke_);
    }

    d2d_rt_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), body_r, body_r), brush_, stroke_w);
    d2d_rt_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), hole_r, hole_r), brush_, stroke_w * 0.85f);
}

void D3DRenderer::DrawInputField(const RectF& box,
                                 const std::wstring& label,
                                 const std::wstring& value_or_placeholder,
                                 bool is_placeholder,
                                 bool focused,
                                 bool show_caret,
                                 float dpi_scale) {
    const float s = std::clamp(dpi_scale, 0.75f, 4.0f);
    RectF label_rect{box.left, box.top - 21.0f * s, box.right, box.top - 3.0f * s};
    DrawTextLine(label, fmt_label_, label_rect, RgbaHex(0xB4BDD0, 1.0f));

    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(ToD2DRect(box), 8.0f * s, 8.0f * s);
    if (focused) {
        D2D1_ROUNDED_RECT glow = D2D1::RoundedRect(
            D2D1::RectF(box.left - 2.0f * s, box.top - 2.0f * s, box.right + 2.0f * s, box.bottom + 2.0f * s),
            10.0f * s,
            10.0f * s);
        brush_->SetColor(RgbaHex(0x4F8CFF, 0.22f));
        d2d_rt_->FillRoundedRectangle(glow, brush_);
    }

    brush_->SetColor(RgbaHex(0x0E1118, 1.0f));
    d2d_rt_->FillRoundedRectangle(rr, brush_);

    brush_->SetColor(focused ? RgbaHex(0x4F8CFF, 1.0f) : RgbaHex(0x283044, 1.0f));
    d2d_rt_->DrawRoundedRectangle(rr, brush_, focused ? (1.6f * s) : (1.0f * s));

    std::wstring display = value_or_placeholder;
    if (focused && show_caret) {
        if (is_placeholder) {
            display = L"|";
        } else {
            display.push_back(L'|');
        }
    }

    RectF text_rect{box.left + 12.0f * s, box.top, box.right - 12.0f * s, box.bottom};
    const D2D1_COLOR_F text_color =
        (is_placeholder && !(focused && show_caret)) ? RgbaHex(0x566079, 1.0f) : RgbaHex(0xF2F5FC, 1.0f);
    DrawTextLine(display, fmt_input_, text_rect, text_color);
}

void D3DRenderer::DrawLoginModal(const RenderUiState& state, UiHitRegions* regions) {
    const float s = std::clamp(state.dpi_scale, 0.75f, 4.0f);
    const float cw = static_cast<float>(width_);
    const float ch = static_cast<float>(height_);

    // Subtle top ambient blue glow behind card
    D2D1_ELLIPSE glow_ellipse =
        D2D1::Ellipse(D2D1::Point2F(cw * 0.5f, ch * 0.42f), 290.0f * s, 210.0f * s);
    brush_->SetColor(RgbaHex(0x3B82F6, 0.055f));
    d2d_rt_->FillEllipse(glow_ellipse, brush_);

    const bool show_auth_inputs = state.waiting_for_auth_input || state.is_error_banner;
    const bool show_user_field = show_auth_inputs && (state.auth_kind == AuthKind::AppleDiffieHellman);
    const bool show_pass_field = show_auth_inputs && (state.auth_kind != AuthKind::None);

    float card_h = 166.0f * s;  // Header + route badge + buttons + footer
    if (!state.status_or_error_text.empty() && (state.is_error_banner || !show_auth_inputs)) {
        card_h += 48.0f * s;
    }
    if (show_user_field) card_h += 68.0f * s;
    if (show_pass_field) card_h += 68.0f * s;
    if (state.show_advanced_settings) {
        card_h += 158.0f * s;
    }

    const float card_w = std::min(cw - 32.0f * s, 430.0f * s);
    const float card_left = std::round((cw - card_w) * 0.5f);
    const float card_top = std::round((ch - card_h) * 0.5f);
    const RectF card{card_left, card_top, card_left + card_w, card_top + card_h};
    regions->modal_card = card;

    // Soft layered drop shadow
    for (int i = 3; i >= 1; --i) {
        const float spread = static_cast<float>(i) * 5.0f * s;
        const float y_off = static_cast<float>(i) * 4.0f * s;
        D2D1_ROUNDED_RECT sh = D2D1::RoundedRect(
            D2D1::RectF(card.left - spread, card.top - spread * 0.4f + y_off, card.right + spread, card.bottom + spread + y_off),
            (16.0f + static_cast<float>(i) * 3.0f) * s,
            (16.0f + static_cast<float>(i) * 3.0f) * s);
        brush_->SetColor(RgbaHex(0x000000, 0.16f));
        d2d_rt_->FillRoundedRectangle(sh, brush_);
    }

    // #161922 frosted dark card body + crisp 1px border
    D2D1_ROUNDED_RECT card_rr = D2D1::RoundedRect(ToD2DRect(card), 16.0f * s, 16.0f * s);
    brush_->SetColor(RgbaHex(0x161922, 0.98f));
    d2d_rt_->FillRoundedRectangle(card_rr, brush_);
    brush_->SetColor(RgbaHex(0x2A3144, 1.0f));
    d2d_rt_->DrawRoundedRectangle(card_rr, brush_, 1.1f * s);

    const float pad_x = 26.0f * s;
    const float inner_left = card.left + pad_x;
    const float inner_right = card.right - pad_x;
    float y = card.top + 22.0f * s;

    // Top-right ⚙ gear button
    RectF gear_btn{inner_right - 36.0f * s, y, inner_right, y + 36.0f * s};
    regions->modal_gear_button = gear_btn;
    const bool gear_hover = gear_btn.contains(state.mouse_x, state.mouse_y);
    D2D1_ROUNDED_RECT gear_rr = D2D1::RoundedRect(ToD2DRect(gear_btn), 9.0f * s, 9.0f * s);
    brush_->SetColor(state.show_advanced_settings ? RgbaHex(0x223252, 1.0f)
                                                  : (gear_hover ? RgbaHex(0x252C3E, 1.0f) : RgbaHex(0x1D2230, 1.0f)));
    d2d_rt_->FillRoundedRectangle(gear_rr, brush_);
    brush_->SetColor(state.show_advanced_settings ? RgbaHex(0x4F8CFF, 1.0f) : RgbaHex(0x2E364B, 1.0f));
    d2d_rt_->DrawRoundedRectangle(gear_rr, brush_, 1.0f * s);
    DrawGearIcon((gear_btn.left + gear_btn.right) * 0.5f,
                 (gear_btn.top + gear_btn.bottom) * 0.5f,
                 8.5f * s,
                 state.show_advanced_settings ? RgbaHex(0x82B1FF, 1.0f) : RgbaHex(0xBCC5D8, 1.0f),
                 1.7f * s);

    // Title & subtitle
    DrawTextLine(L"Mac 화면 미러링",
                 fmt_title_,
                 RectF{inner_left, y - 2.0f * s, gear_btn.left - 8.0f * s, y + 26.0f * s},
                 RgbaHex(0xF5F7FC, 1.0f));

    std::wstring subtitle = L"macOS 화면 공유 로그인 (Apple DH)";
    if (!show_auth_inputs && !state.is_error_banner) {
        subtitle = L"Mac 화면 공유 서버에 연결하는 중...";
    } else if (state.auth_kind == AuthKind::VncPassword) {
        subtitle = L"VNC 비밀번호 인증";
    }
    std::string route_info = state.socks5_text.empty()
                                 ? ("Direct · " + state.target_host_text)
                                 : ("SOCKS5 " + state.socks5_text + " → " + state.target_host_text);
    subtitle += L"  ·  " + Utf8ToWideLocal(route_info);
    DrawTextLine(subtitle,
                 fmt_small_,
                 RectF{inner_left, y + 26.0f * s, inner_right, y + 44.0f * s},
                 RgbaHex(0x8B95AC, 1.0f));
    y += 56.0f * s;

    // Status or Error Banner
    if (!state.status_or_error_text.empty() && (state.is_error_banner || !show_auth_inputs)) {
        RectF banner{inner_left, y, inner_right, y + 36.0f * s};
        D2D1_ROUNDED_RECT brr = D2D1::RoundedRect(ToD2DRect(banner), 8.0f * s, 8.0f * s);
        if (state.is_error_banner) {
            brush_->SetColor(RgbaHex(0x2D161D, 1.0f));
            d2d_rt_->FillRoundedRectangle(brr, brush_);
            brush_->SetColor(RgbaHex(0x7D2D3D, 1.0f));
            d2d_rt_->DrawRoundedRectangle(brr, brush_, 1.0f * s);
            DrawTextLine(Utf8ToWideLocal(state.status_or_error_text),
                         fmt_subtitle_,
                         RectF{banner.left + 12.0f * s, banner.top, banner.right - 12.0f * s, banner.bottom},
                         RgbaHex(0xFF8594, 1.0f));
        } else {
            brush_->SetColor(RgbaHex(0x182235, 1.0f));
            d2d_rt_->FillRoundedRectangle(brr, brush_);
            brush_->SetColor(RgbaHex(0x2D466E, 1.0f));
            d2d_rt_->DrawRoundedRectangle(brr, brush_, 1.0f * s);
            DrawTextLine(Utf8ToWideLocal(state.status_or_error_text),
                         fmt_subtitle_,
                         RectF{banner.left + 12.0f * s, banner.top, banner.right - 12.0f * s, banner.bottom},
                         RgbaHex(0x8AB8FF, 1.0f));
        }
        y += 48.0f * s;
    }

    // Username field (Apple DH)
    if (show_user_field) {
        y += 22.0f * s;
        RectF ubox{inner_left, y, inner_right, y + 40.0f * s};
        regions->modal_username_field = ubox;
        const bool empty = state.username_text.empty();
        DrawInputField(ubox,
                       L"Mac 사용자 이름",
                       empty ? L"macOS 계정 이름 입력" : Utf8ToWideLocal(state.username_text),
                       empty,
                       state.focused_field == ModalField::Username,
                       state.caret_visible,
                       s);
        y += 46.0f * s;
    }

    // Password field
    if (show_pass_field) {
        y += 22.0f * s;
        RectF pbox{inner_left, y, inner_right, y + 40.0f * s};
        regions->modal_password_field = pbox;
        const bool empty = (state.password_char_count == 0);
        std::wstring bullets(state.password_char_count, L'\u25CF');
        DrawInputField(pbox,
                       (state.auth_kind == AuthKind::VncPassword) ? L"VNC 암호" : L"암호",
                       empty ? L"암호 입력" : bullets,
                       empty,
                       state.focused_field == ModalField::Password,
                       state.caret_visible,
                       s);
        y += 46.0f * s;
    }

    // Collapsible ⚙ Advanced Target Host & SOCKS5 Proxy settings
    if (state.show_advanced_settings) {
        y += 6.0f * s;
        brush_->SetColor(RgbaHex(0x252C3E, 1.0f));
        d2d_rt_->DrawLine(
            D2D1::Point2F(inner_left, y), D2D1::Point2F(inner_right, y), brush_, 1.0f * s);
        y += 12.0f * s;

        y += 20.0f * s;
        RectF hbox{inner_left, y, inner_right, y + 38.0f * s};
        regions->modal_host_field = hbox;
        const bool host_empty = state.target_host_text.empty();
        DrawInputField(hbox,
                       L"대상 호스트 (IP:포트)",
                       host_empty ? L"100.80.74.94:5900" : Utf8ToWideLocal(state.target_host_text),
                       host_empty,
                       state.focused_field == ModalField::TargetHost,
                       state.caret_visible,
                       s);
        y += 44.0f * s;

        y += 20.0f * s;
        RectF sbox{inner_left, y, inner_right, y + 38.0f * s};
        regions->modal_socks5_field = sbox;
        const bool socks_empty = state.socks5_text.empty();
        DrawInputField(sbox,
                       L"SOCKS5 프록시 (선택)",
                       socks_empty ? L"직접 연결 (예: 127.0.0.1:1055)" : Utf8ToWideLocal(state.socks5_text),
                       socks_empty,
                       state.focused_field == ModalField::Socks5,
                       state.caret_visible,
                       s);
        y += 46.0f * s;
    } else {
        y += 6.0f * s;
    }

    // Action buttons row
    const float exit_w = 84.0f * s;
    const float gap = 10.0f * s;
    RectF connect_btn{inner_left, y, inner_right - exit_w - gap, y + 42.0f * s};
    RectF exit_btn{inner_right - exit_w, y, inner_right, y + 42.0f * s};
    regions->modal_connect_button = connect_btn;
    regions->modal_exit_button = exit_btn;

    const bool conn_hover = connect_btn.contains(state.mouse_x, state.mouse_y);
    D2D1_ROUNDED_RECT conn_rr = D2D1::RoundedRect(ToD2DRect(connect_btn), 9.0f * s, 9.0f * s);
    brush_->SetColor(conn_hover ? RgbaHex(0x2563EB, 1.0f) : RgbaHex(0x3B82F6, 1.0f));
    d2d_rt_->FillRoundedRectangle(conn_rr, brush_);
    DrawTextLine(show_auth_inputs ? L"연결하기 (Enter)" : L"다시 연결 (Enter)",
                 fmt_button_,
                 connect_btn,
                 RgbaHex(0xFFFFFF, 1.0f),
                 DWRITE_TEXT_ALIGNMENT_CENTER);

    const bool exit_hover = exit_btn.contains(state.mouse_x, state.mouse_y);
    D2D1_ROUNDED_RECT exit_rr = D2D1::RoundedRect(ToD2DRect(exit_btn), 9.0f * s, 9.0f * s);
    brush_->SetColor(exit_hover ? RgbaHex(0x2B3347, 1.0f) : RgbaHex(0x1E2433, 1.0f));
    d2d_rt_->FillRoundedRectangle(exit_rr, brush_);
    brush_->SetColor(RgbaHex(0x303950, 1.0f));
    d2d_rt_->DrawRoundedRectangle(exit_rr, brush_, 1.0f * s);
    DrawTextLine(L"종료", fmt_button_, exit_btn, RgbaHex(0xD2D9E8, 1.0f), DWRITE_TEXT_ALIGNMENT_CENTER);

    y += 48.0f * s;
    DrawTextLine(L"Tab 이동 · Enter 연결 · Esc 종료  |  공용 PC 보안: 계정 정보 저장 안 함",
                 fmt_small_,
                 RectF{inner_left, y, inner_right, y + 18.0f * s},
                 RgbaHex(0x6D778F, 1.0f),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
}

void D3DRenderer::DrawFloatingHud(const RenderUiState& state, UiHitRegions* regions) {
    const float cw = static_cast<float>(width_);
    const float base_s = std::clamp(state.dpi_scale, 0.75f, 4.0f);
    // Fit comfortably even if user shrinks windowed mode below 820px.
    const float s = std::min(base_s, std::max(0.58f, (cw - 24.0f) / 796.0f));

    const float pill_w = 788.0f * s;
    const float pill_h = 42.0f * s;
    const float target_top = 12.0f * s;
    const float hidden_top = -pill_h - 8.0f * s;
    const float ease = 1.0f - std::pow(1.0f - std::clamp(state.hud_reveal, 0.0f, 1.0f), 3.0f);
    const float pill_top = hidden_top + (target_top - hidden_top) * ease;
    const float pill_left = std::round((cw - pill_w) * 0.5f);

    RectF pill{pill_left, pill_top, pill_left + pill_w, pill_top + pill_h};
    regions->hud_pill = pill;
    const float radius = pill_h * 0.5f;

    // Soft floating shadow
    D2D1_ROUNDED_RECT shadow_rr = D2D1::RoundedRect(
        D2D1::RectF(pill.left - 3.0f * s, pill.top + 4.0f * s, pill.right + 3.0f * s, pill.bottom + 7.0f * s),
        radius + 3.0f * s,
        radius + 3.0f * s);
    brush_->SetColor(RgbaHex(0x000000, 0.42f));
    d2d_rt_->FillRoundedRectangle(shadow_rr, brush_);

    D2D1_ROUNDED_RECT pill_rr = D2D1::RoundedRect(ToD2DRect(pill), radius, radius);
    brush_->SetColor(RgbaHex(0x141822, 0.95f));
    d2d_rt_->FillRoundedRectangle(pill_rr, brush_);
    brush_->SetColor(RgbaHex(0x2E374D, 1.0f));
    d2d_rt_->DrawRoundedRectangle(pill_rr, brush_, 1.1f * s);

    const float cy = (pill.top + pill.bottom) * 0.5f;
    float x = pill.left + 16.0f * s;

    auto draw_divider = [&](float dx) {
        brush_->SetColor(RgbaHex(0x2B3347, 1.0f));
        d2d_rt_->DrawLine(
            D2D1::Point2F(dx, cy - 10.0f * s), D2D1::Point2F(dx, cy + 10.0f * s), brush_, 1.0f * s);
    };

    // 1. [● XX FPS · XX ms]
    D2D1_COLOR_F dot_color = RgbaHex(0x22C55E, 1.0f);
    if (state.session_state != SessionState::Connected) {
        dot_color = RgbaHex(0xF59E0B, 1.0f);
    } else if (state.stats.rtt_ms > 110.0) {
        dot_color = RgbaHex(0xEF4444, 1.0f);
    } else if (state.stats.rtt_ms > 45.0) {
        dot_color = RgbaHex(0xF59E0B, 1.0f);
    }

    brush_->SetColor(D2D1::ColorF(dot_color.r, dot_color.g, dot_color.b, 0.25f));
    d2d_rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + 5.0f * s, cy), 6.5f * s, 6.5f * s), brush_);
    brush_->SetColor(dot_color);
    d2d_rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + 5.0f * s, cy), 4.0f * s, 4.0f * s), brush_);

    char stat_buf[64] = {};
    if (state.stats.frames_received > 0) {
        std::snprintf(stat_buf,
                      sizeof(stat_buf),
                      "%.0f FPS \xC2\xB7 %.0f ms",
                      std::max(0.0, state.stats.fps),
                      std::max(0.0, state.stats.rtt_ms));
    } else {
        std::snprintf(stat_buf, sizeof(stat_buf), "-- FPS \xC2\xB7 -- ms");
    }
    DrawTextLine(Utf8ToWideLocal(stat_buf),
                 fmt_hud_bold_,
                 RectF{x + 15.0f * s, pill.top, x + 130.0f * s, pill.bottom},
                 RgbaHex(0xEDF1FA, 1.0f));
    x += 134.0f * s;
    draw_divider(x);
    x += 12.0f * s;

    // 2. [커서 1.0x━━●━━4.0x (1.8x)]
    DrawTextLine(L"커서 1.0x",
                 fmt_hud_,
                 RectF{x, pill.top, x + 58.0f * s, pill.bottom},
                 RgbaHex(0xAEB8CE, 1.0f));
    x += 61.0f * s;

    const float track_w = 94.0f * s;
    const float track_h = 4.0f * s;
    RectF track{x, cy - track_h * 0.5f, x + track_w, cy + track_h * 0.5f};
    regions->hud_slider_track = track;
    regions->hud_slider_hit_box = RectF{x - 6.0f * s, pill.top + 4.0f * s, x + track_w + 6.0f * s, pill.bottom - 4.0f * s};

    D2D1_ROUNDED_RECT track_bg = D2D1::RoundedRect(ToD2DRect(track), 2.0f * s, 2.0f * s);
    brush_->SetColor(RgbaHex(0x2B3347, 1.0f));
    d2d_rt_->FillRoundedRectangle(track_bg, brush_);

    const float norm = (ClampCursorScale(state.cursor_scale) - 1.0f) / 3.0f;
    const float thumb_x = track.left + norm * track_w;
    RectF active_track{track.left, track.top, thumb_x, track.bottom};
    if (!active_track.empty()) {
        brush_->SetColor(RgbaHex(0x4F8CFF, 1.0f));
        d2d_rt_->FillRoundedRectangle(D2D1::RoundedRect(ToD2DRect(active_track), 2.0f * s, 2.0f * s), brush_);
    }

    const bool slider_hover =
        state.dragging_cursor_slider || regions->hud_slider_hit_box.contains(state.mouse_x, state.mouse_y);
    const float thumb_r = (slider_hover ? 6.8f : 5.6f) * s;
    brush_->SetColor(RgbaHex(0x4F8CFF, 0.30f));
    d2d_rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumb_x, cy), thumb_r + 2.5f * s, thumb_r + 2.5f * s), brush_);
    brush_->SetColor(RgbaHex(0xFFFFFF, 1.0f));
    d2d_rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumb_x, cy), thumb_r, thumb_r), brush_);

    x += track_w + 6.0f * s;
    char scale_txt[32] = {};
    std::snprintf(scale_txt, sizeof(scale_txt), "4.0x (%.1fx)", static_cast<double>(ClampCursorScale(state.cursor_scale)));
    DrawTextLine(Utf8ToWideLocal(scale_txt),
                 fmt_hud_bold_,
                 RectF{x, pill.top, x + 78.0f * s, pill.bottom},
                 RgbaHex(0x88B8FF, 1.0f));
    x += 82.0f * s;
    draw_divider(x);
    x += 10.0f * s;

    // 3. [Adaptive / Sharp / Turbo]
    RectF q_btn{x, cy - 14.0f * s, x + 84.0f * s, cy + 14.0f * s};
    regions->hud_quality_button = q_btn;
    const bool q_hover = q_btn.contains(state.mouse_x, state.mouse_y);
    D2D1_COLOR_F q_accent = RgbaHex(0x38BDF8, 1.0f);
    if (state.stats.quality_mode == QualityMode::Sharp) {
        q_accent = RgbaHex(0xA78BFA, 1.0f);
    } else if (state.stats.quality_mode == QualityMode::Turbo) {
        q_accent = RgbaHex(0x34D399, 1.0f);
    }
    D2D1_ROUNDED_RECT q_rr = D2D1::RoundedRect(ToD2DRect(q_btn), 14.0f * s, 14.0f * s);
    brush_->SetColor(q_hover ? RgbaHex(0x253048, 1.0f) : RgbaHex(0x1C2334, 1.0f));
    d2d_rt_->FillRoundedRectangle(q_rr, brush_);
    brush_->SetColor(D2D1::ColorF(q_accent.r, q_accent.g, q_accent.b, 0.65f));
    d2d_rt_->DrawRoundedRectangle(q_rr, brush_, 1.0f * s);
    DrawTextLine(Utf8ToWideLocal(QualityModeToString(state.stats.quality_mode)),
                 fmt_hud_bold_,
                 q_btn,
                 q_accent,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
    x += 94.0f * s;
    draw_divider(x);
    x += 10.0f * s;

    // 4. [Ctrl↔⌘ / Ctrl=Ctrl] modifier swap toggle
    RectF km_btn{x, cy - 14.0f * s, x + 78.0f * s, cy + 14.0f * s};
    regions->hud_keymap_button = km_btn;
    const bool km_hover = km_btn.contains(state.mouse_x, state.mouse_y);
    D2D1_ROUNDED_RECT km_rr = D2D1::RoundedRect(ToD2DRect(km_btn), 14.0f * s, 14.0f * s);
    if (state.swap_ctrl_cmd) {
        brush_->SetColor(km_hover ? RgbaHex(0x25385A, 1.0f) : RgbaHex(0x1D2B45, 1.0f));
        d2d_rt_->FillRoundedRectangle(km_rr, brush_);
        brush_->SetColor(RgbaHex(0x4F8CFF, 0.80f));
        d2d_rt_->DrawRoundedRectangle(km_rr, brush_, 1.0f * s);
        DrawTextLine(L"Ctrl\u2194\u2318",
                     fmt_hud_bold_,
                     km_btn,
                     RgbaHex(0x8CC0FF, 1.0f),
                     DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        brush_->SetColor(km_hover ? RgbaHex(0x293247, 1.0f) : RgbaHex(0x1D2332, 1.0f));
        d2d_rt_->FillRoundedRectangle(km_rr, brush_);
        brush_->SetColor(RgbaHex(0x313B52, 1.0f));
        d2d_rt_->DrawRoundedRectangle(km_rr, brush_, 1.0f * s);
        DrawTextLine(L"Ctrl=Ctrl",
                     fmt_hud_,
                     km_btn,
                     RgbaHex(0xAEB8CE, 1.0f),
                     DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    x += 88.0f * s;
    draw_divider(x);
    x += 10.0f * s;

    // 5. [창모드 / 전체화면]
    RectF win_btn{x, cy - 14.0f * s, x + 76.0f * s, cy + 14.0f * s};
    regions->hud_window_button = win_btn;
    const bool win_hover = win_btn.contains(state.mouse_x, state.mouse_y);
    D2D1_ROUNDED_RECT win_rr = D2D1::RoundedRect(ToD2DRect(win_btn), 14.0f * s, 14.0f * s);
    brush_->SetColor(win_hover ? RgbaHex(0x293247, 1.0f) : RgbaHex(0x1D2332, 1.0f));
    d2d_rt_->FillRoundedRectangle(win_rr, brush_);
    brush_->SetColor(RgbaHex(0x313B52, 1.0f));
    d2d_rt_->DrawRoundedRectangle(win_rr, brush_, 1.0f * s);
    DrawTextLine(state.fullscreen ? L"창모드" : L"전체화면",
                 fmt_hud_,
                 win_btn,
                 RgbaHex(0xE4E9F5, 1.0f),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
    x += 86.0f * s;
    draw_divider(x);
    x += 10.0f * s;

    // 5. [종료]
    RectF quit_btn{x, cy - 14.0f * s, x + 58.0f * s, cy + 14.0f * s};
    regions->hud_exit_button = quit_btn;
    const bool quit_hover = quit_btn.contains(state.mouse_x, state.mouse_y);
    D2D1_ROUNDED_RECT quit_rr = D2D1::RoundedRect(ToD2DRect(quit_btn), 14.0f * s, 14.0f * s);
    brush_->SetColor(quit_hover ? RgbaHex(0x3F1D28, 1.0f) : RgbaHex(0x271820, 1.0f));
    d2d_rt_->FillRoundedRectangle(quit_rr, brush_);
    brush_->SetColor(RgbaHex(0x612938, 1.0f));
    d2d_rt_->DrawRoundedRectangle(quit_rr, brush_, 1.0f * s);
    DrawTextLine(L"종료",
                 fmt_hud_bold_,
                 quit_btn,
                 RgbaHex(0xFF8090, 1.0f),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
}

void D3DRenderer::DrawMacVectorCursor(float x, float y, float scale) {
    if (!d2d_rt_ || !brush_ || !mac_arrow_geom_) return;
    const float s = std::clamp(scale, 0.75f, 16.0f);

    // 1. Two-pass soft macOS drop shadow
    d2d_rt_->SetTransform(
        D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation(x + 1.1f * s, y + 1.9f * s));
    brush_->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.18f));
    d2d_rt_->DrawGeometry(mac_arrow_geom_, brush_, 2.4f, round_stroke_);
    d2d_rt_->FillGeometry(mac_arrow_geom_, brush_);

    d2d_rt_->SetTransform(
        D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation(x + 0.5f * s, y + 0.9f * s));
    brush_->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.28f));
    d2d_rt_->FillGeometry(mac_arrow_geom_, brush_);

    // 2. Crisp anti-aliased white border + deep black macOS arrow body at exact subpixel tip (x, y)
    d2d_rt_->SetTransform(D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation(x, y));
    brush_->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.98f));
    d2d_rt_->DrawGeometry(mac_arrow_geom_, brush_, 2.15f, round_stroke_);

    brush_->SetColor(RgbaHex(0x06070A, 1.0f));
    d2d_rt_->FillGeometry(mac_arrow_geom_, brush_);

    d2d_rt_->SetTransform(D2D1::Matrix3x2F::Identity());
}

}  // namespace mac_mirror
#endif
