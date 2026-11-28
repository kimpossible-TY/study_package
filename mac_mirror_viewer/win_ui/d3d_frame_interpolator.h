#pragma once

#include <cstdint>
#include "frame_gen_mode.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d2d1.h>
#endif

namespace mac_mirror {

// Pure C++17 frame-generation pacer (cross-platform, unit-testable on macOS & Windows).
class FrameGenPacer {
public:
    static constexpr uint64_t kMinIntervalMs = 20u;
    static constexpr uint64_t kMaxIntervalMs = 140u;
    static constexpr uint64_t kDefaultIntervalMs = 45u;

    void Reset() noexcept;
    void OnNewFrame(uint64_t now_ms) noexcept;
    bool SampleAlpha(uint64_t now_ms, FrameGenMode mode, float* out_alpha) const noexcept;
    bool IsAnimating(uint64_t now_ms, FrameGenMode mode) const noexcept;
    double interval_ms() const noexcept { return interval_ms_; }

private:
    uint64_t last_frame_ms_ = 0;
    double interval_ms_ = static_cast<double>(kDefaultIntervalMs);
    uint32_t frame_count_ = 0;
};

#ifdef _WIN32
class D3DFrameInterpolator {
public:
    D3DFrameInterpolator() = default;
    ~D3DFrameInterpolator();
    D3DFrameInterpolator(const D3DFrameInterpolator&) = delete;
    D3DFrameInterpolator& operator=(const D3DFrameInterpolator&) = delete;

    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    void Shutdown();
    void ReleaseRenderTargetInterop();

    bool PushFramePair(ID2D1RenderTarget* d2d_rt,
                       const uint8_t* prev_bgra,
                       const uint8_t* curr_bgra,
                       uint32_t width,
                       uint32_t height,
                       uint32_t stride_bytes,
                       uint64_t now_ms);

    // Must be called BEFORE `d2d_rt->BeginDraw()`.
    // Synthesizes intermediate frame on GPU for `OpticalFlow` or `Neural` when `SampleAlpha` is active,
    // returning zero-copy shared `ID2D1Bitmap*` (or `nullptr` when settled/Off so caller draws native crisp bitmap).
    ID2D1Bitmap* SynthesizeBeforeD2D(ID2D1RenderTarget* d2d_rt,
                                     FrameGenMode mode,
                                     uint64_t now_ms);

    bool IsAnimating(FrameGenMode mode, uint64_t now_ms) const noexcept {
        return available_ && pacer_.IsAnimating(now_ms, mode);
    }
    bool is_available() const noexcept { return available_; }
    void ResetPacer() noexcept {
        pacer_.Reset();
        has_frame_pair_ = false;
        coarse_flow_dirty_ = false;
    }

private:
    bool CompilePipelineShaders();
    bool CreatePipelineStatesAndWeights();
    bool EnsureFrameResources(uint32_t width, uint32_t height);
    void ReleaseFrameResources();
    bool EnsureSharedBitmap(ID2D1RenderTarget* d2d_rt);
    void UpdatePassParams(float alpha);

    bool available_ = false;
    bool has_frame_pair_ = false;
    bool coarse_flow_dirty_ = false;
    FrameGenPacer pacer_;

    uint32_t tex_width_ = 0;
    uint32_t tex_height_ = 0;
    uint32_t q_width_ = 0;
    uint32_t q_height_ = 0;

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;

    ID3D11VertexShader* vs_fullscreen_ = nullptr;
    ID3D11PixelShader* ps_pyramid_ = nullptr;
    ID3D11PixelShader* ps_coarse_flow_ = nullptr;
    ID3D11PixelShader* ps_neural_conv1_ = nullptr;
    ID3D11PixelShader* ps_neural_conv2_ = nullptr;
    ID3D11PixelShader* ps_warp_ = nullptr;

    ID3D11SamplerState* linear_sampler_ = nullptr;
    ID3D11SamplerState* point_sampler_ = nullptr;
    ID3D11RasterizerState* rasterizer_state_ = nullptr;
    ID3D11BlendState* blend_state_ = nullptr;
    ID3D11DepthStencilState* depth_state_ = nullptr;

    ID3D11Buffer* params_cb_ = nullptr;
    ID3D11Buffer* weights_cb_ = nullptr;

    // Full-resolution input & synthesized output textures
    ID3D11Texture2D* prev_tex_ = nullptr;
    ID3D11ShaderResourceView* prev_srv_ = nullptr;
    ID3D11Texture2D* curr_tex_ = nullptr;
    ID3D11ShaderResourceView* curr_srv_ = nullptr;

    ID3D11Texture2D* out_tex_ = nullptr;
    ID3D11RenderTargetView* out_rtv_ = nullptr;
    ID3D11ShaderResourceView* out_srv_ = nullptr;

    // Quarter-resolution feature pyramid, optical flow, and neural CNN buffers
    ID3D11Texture2D* feat0_tex_ = nullptr;
    ID3D11RenderTargetView* feat0_rtv_ = nullptr;
    ID3D11ShaderResourceView* feat0_srv_ = nullptr;

    ID3D11Texture2D* feat1_tex_ = nullptr;
    ID3D11RenderTargetView* feat1_rtv_ = nullptr;
    ID3D11ShaderResourceView* feat1_srv_ = nullptr;

    ID3D11Texture2D* flow_coarse_tex_ = nullptr;
    ID3D11RenderTargetView* flow_coarse_rtv_ = nullptr;
    ID3D11ShaderResourceView* flow_coarse_srv_ = nullptr;

    ID3D11Texture2D* nn_hidden0_tex_ = nullptr;
    ID3D11RenderTargetView* nn_hidden0_rtv_ = nullptr;
    ID3D11ShaderResourceView* nn_hidden0_srv_ = nullptr;

    ID3D11Texture2D* nn_hidden1_tex_ = nullptr;
    ID3D11RenderTargetView* nn_hidden1_rtv_ = nullptr;
    ID3D11ShaderResourceView* nn_hidden1_srv_ = nullptr;

    ID3D11Texture2D* flow_refined_tex_ = nullptr;
    ID3D11RenderTargetView* flow_refined_rtv_ = nullptr;
    ID3D11ShaderResourceView* flow_refined_srv_ = nullptr;

    // Zero-copy IDXGISurface <-> Direct2D shared bitmap interop
    ID2D1Bitmap* out_d2d_bitmap_ = nullptr;
    ID2D1RenderTarget* bound_d2d_rt_ = nullptr;
};
#endif

}  // namespace mac_mirror
