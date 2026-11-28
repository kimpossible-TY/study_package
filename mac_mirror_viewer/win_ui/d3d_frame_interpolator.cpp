#include "d3d_frame_interpolator.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#ifdef _WIN32
#include <dxgi.h>
#include <d3dcommon.h>
#if __has_include(<neural_flow_weights.h>)
#include <neural_flow_weights.h>
#define MAC_MIRROR_HAS_NEURAL_FLOW_WEIGHTS 1
#elif __has_include("neural_flow_weights.h")
#include "neural_flow_weights.h"
#define MAC_MIRROR_HAS_NEURAL_FLOW_WEIGHTS 1
#else
#define MAC_MIRROR_HAS_NEURAL_FLOW_WEIGHTS 0
#endif

#if MAC_MIRROR_HAS_NEURAL_FLOW_WEIGHTS
static_assert(mac_mirror::neural_flow::kVersion >= 2,
              "D3DFrameInterpolator v2 requires neural_flow_weights.h v2");
static_assert(mac_mirror::neural_flow::kInChannels == 8,
              "Expected 8 input channels in neural_flow_weights.h");
static_assert(mac_mirror::neural_flow::kHiddenChannels == 16,
              "Expected 16 hidden channels in neural_flow_weights.h");
static_assert(mac_mirror::neural_flow::kOutChannels == 7,
              "Expected 7 output channels in neural_flow_weights.h");
#endif
#endif

namespace mac_mirror {

// ============================================================================
// FrameGenPacer (cross-platform pure C++17)
// ============================================================================

bool FrameGenPacer::ShouldSkipFrameGenForDirtyRegion(uint32_t fb_w,
                                                     uint32_t fb_h,
                                                     uint32_t dirty_w,
                                                     uint32_t dirty_h) noexcept {
    if (fb_w == 0u || fb_h == 0u || dirty_w == 0u || dirty_h == 0u) {
        return true;
    }
    const uint64_t fb_area = static_cast<uint64_t>(fb_w) * static_cast<uint64_t>(fb_h);
    const uint64_t dirty_area = static_cast<uint64_t>(dirty_w) * static_cast<uint64_t>(dirty_h);
    if (dirty_area < 64u * 64u) {
        return true;
    }
    return (dirty_area * 100u < fb_area * 2u) && dirty_w < 192u && dirty_h < 192u;
}

bool FrameGenPacer::ShouldReuseCachedSynthesis(bool cached_valid,
                                               float alpha,
                                               float cached_alpha,
                                               FrameGenMode mode,
                                               FrameGenMode cached_mode) noexcept {
    return cached_valid && (mode == cached_mode) && (std::fabs(alpha - cached_alpha) < 0.03f);
}

void FrameGenPacer::Reset() noexcept {
    last_frame_ms_ = 0;
    interval_ms_ = static_cast<double>(kDefaultIntervalMs);
    frame_count_ = 0;
}

void FrameGenPacer::OnNewFrame(uint64_t now_ms) noexcept {
    if (frame_count_ > 0 && now_ms > last_frame_ms_) {
        const uint64_t raw_dt = now_ms - last_frame_ms_;
        const double clamped_dt =
            static_cast<double>(std::clamp<uint64_t>(raw_dt, kMinIntervalMs, kMaxIntervalMs));
        interval_ms_ = std::clamp(interval_ms_ * 0.65 + clamped_dt * 0.35,
                                  static_cast<double>(kMinIntervalMs),
                                  static_cast<double>(kMaxIntervalMs));
    }
    last_frame_ms_ = now_ms;
    if (frame_count_ < UINT32_MAX) {
        ++frame_count_;
    }
}

bool FrameGenPacer::SampleAlpha(uint64_t now_ms, FrameGenMode mode, float* out_alpha) const noexcept {
    if (mode == FrameGenMode::Off || frame_count_ == 0 || interval_ms_ <= 0.0) {
        if (out_alpha) {
            *out_alpha = 1.0f;
        }
        return false;
    }

    const double elapsed_ms =
        (now_ms >= last_frame_ms_) ? static_cast<double>(now_ms - last_frame_ms_) : 0.0;
    if (elapsed_ms >= interval_ms_) {
        if (out_alpha) {
            *out_alpha = 1.0f;
        }
        return false;
    }

    // Small half-display-tick lead (4ms) ensures alpha > 0 immediately on frame arrival
    // (avoiding a 1-frame hold on prev) while progressing monotonically across (0, 1).
    constexpr double kLeadMs = 4.0;
    const double raw_alpha = (elapsed_ms + kLeadMs) / (interval_ms_ + kLeadMs);
    const float clamped = static_cast<float>(std::clamp(raw_alpha, 0.05, 0.99));
    if (out_alpha) {
        *out_alpha = clamped;
    }
    return true;
}

bool FrameGenPacer::IsAnimating(uint64_t now_ms, FrameGenMode mode) const noexcept {
    float alpha = 1.0f;
    return SampleAlpha(now_ms, mode, &alpha);
}

#ifdef _WIN32

namespace {

template <typename T>
void SafeRelease(T*& ptr) noexcept {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

// Dynamically resolved D3DCompile signature from System32\d3dcompiler_47.dll (zero PE DLL imports!).
using PfnD3DCompile = HRESULT(WINAPI*)(LPCVOID pSrcData,
                                       SIZE_T SrcDataSize,
                                       LPCSTR pSourceName,
                                       const D3D_SHADER_MACRO* pDefines,
                                       ID3DInclude* pInclude,
                                       LPCSTR pEntrypoint,
                                       LPCSTR pTarget,
                                       UINT Flags1,
                                       UINT Flags2,
                                       ID3DBlob** ppCode,
                                       ID3DBlob** ppErrorMsgs);

constexpr UINT kD3DCompileOptimizationLevel3 = 0x00008000u;

#if MAC_MIRROR_HAS_NEURAL_FLOW_WEIGHTS
constexpr float kFlowNormScale = neural_flow::kFlowNormScale;
constexpr float kResidualFlowScale = neural_flow::kResidualFlowScale;
constexpr float kRgbResidualScale = neural_flow::kRgbResidualScale;
#else
constexpr float kFlowNormScale = 24.0f;
constexpr float kResidualFlowScale = 6.0f;
constexpr float kRgbResidualScale = 0.25f;
#endif

// Shared per-pass parameters bound to register(b0): 5 float4s = 80 bytes.
struct alignas(16) PassParamsCBData {
    float inv_full_half[4];  // xy = 1 / full_size, zw = 1 / half_size
    float inv_qtr_eig[4];    // xy = 1 / qtr_size,  zw = 1 / eig_size
    float pass_inv[4];       // xy = 1 / src_size,  zw = 1 / dst_size
    float params[4];         // x = alpha, y = use_rgb_residual, z = 1 / kFlowNormScale, w = kResidualFlowScale
    float extra[4];          // x = kRgbResidualScale, y = full_w, z = full_h, w = 0.0f
};

// Conv1 (8 -> 16, 3x3): 288 float4 weights + 4 float4 biases = 292 float4s (4,672 bytes).
struct alignas(16) Conv1WeightsCBData {
    float w[288][4];
    float b[4][4];
};

// Conv2 (16 -> 16, 3x3): 576 float4 weights + 4 float4 biases = 580 float4s (9,280 bytes).
struct alignas(16) Conv2WeightsCBData {
    float w[576][4];
    float b[4][4];
};

// Conv3 (16 -> 7, 3x3): 252 float4 weights + 2 float4 biases = 254 float4s (4,064 bytes).
struct alignas(16) Conv3WeightsCBData {
    float w[252][4];
    float b[2][4];
};

static_assert(sizeof(PassParamsCBData) == 5 * 16, "PassParamsCBData must be 5 float4s");
static_assert(sizeof(Conv1WeightsCBData) == 292 * 16, "Conv1WeightsCBData must be 292 float4s");
static_assert(sizeof(Conv2WeightsCBData) == 580 * 16, "Conv2WeightsCBData must be 580 float4s");
static_assert(sizeof(Conv3WeightsCBData) == 254 * 16, "Conv3WeightsCBData must be 254 float4s");

#if !MAC_MIRROR_HAS_NEURAL_FLOW_WEIGHTS
// Standalone fallback weights when compiled without `neural_flow_weights.h`:
// Preserves coarse flow (dv = 0), balanced occlusion (blend_w = 0.5), zero RGB residual,
// and razor-sharp static text protection.
void FillFallbackNeuralWeightsV2(float (&c1_w)[16 * 8 * 9],
                                 float (&c1_b)[16],
                                 float (&c2_w)[16 * 16 * 9],
                                 float (&c2_b)[16],
                                 float (&c3_w)[7 * 16 * 9],
                                 float (&c3_b)[7]) noexcept {
    std::memset(c1_w, 0, sizeof(c1_w));
    std::memset(c1_b, 0, sizeof(c1_b));
    std::memset(c2_w, 0, sizeof(c2_w));
    std::memset(c2_b, 0, sizeof(c2_b));
    std::memset(c3_w, 0, sizeof(c3_w));
    std::memset(c3_b, 0, sizeof(c3_b));

    constexpr int kCenter = 4;  // ky = 1, kx = 1
    // Hidden 0: motion_prior (ch7) at center
    c1_w[0 * 72 + 7 * 9 + kCenter] = 1.0f;
    // Hidden 1..4: |vx_c / 24| and |vy_c / 24| via +/- ReLU pairs
    c1_w[1 * 72 + 4 * 9 + kCenter] = 1.0f;
    c1_w[2 * 72 + 4 * 9 + kCenter] = -1.0f;
    c1_w[3 * 72 + 5 * 9 + kCenter] = 1.0f;
    c1_w[4 * 72 + 5 * 9 + kCenter] = -1.0f;

    // Conv2 has a residual skip (`h2 = ReLU(Conv2(h1) + h1)`), so zero Conv2 weights pass h1 through.

    // Output 3 (motion_conf logit): negative on static pixels, positive on moving pixels.
    c3_b[3] = -3.5f;
    c3_w[3 * 144 + 0 * 9 + kCenter] = 6.5f;
    c3_w[3 * 144 + 1 * 9 + kCenter] = 6.0f;
    c3_w[3 * 144 + 2 * 9 + kCenter] = 6.0f;
    c3_w[3 * 144 + 3 * 9 + kCenter] = 6.0f;
    c3_w[3 * 144 + 4 * 9 + kCenter] = 6.0f;
}
#endif

// Source layout from `neural_flow_weights.h`: [out_c * (in_c * 9) + in_c * 9 + ky * 3 + kx].
// GPU layout: spatial-major [s = ky * 3 + kx] so unrolled 3x3 shader taps use immediate CB offsets.
void PackConv1WeightsCB(const float* c1_w, const float* c1_b, Conv1WeightsCBData* out) noexcept {
    std::memset(out, 0, sizeof(*out));
    for (int ky = 0; ky < 3; ++ky) {
        for (int kx = 0; kx < 3; ++kx) {
            const int s = ky * 3 + kx;
            for (int oc = 0; oc < 16; ++oc) {
                const int dst = (s * 16 + oc) * 2;
                for (int k = 0; k < 4; ++k) {
                    out->w[dst + 0][k] = c1_w[oc * 72 + (k + 0) * 9 + s];
                    out->w[dst + 1][k] = c1_w[oc * 72 + (k + 4) * 9 + s];
                }
            }
        }
    }
    for (int g = 0; g < 4; ++g) {
        for (int k = 0; k < 4; ++k) {
            out->b[g][k] = c1_b[g * 4 + k];
        }
    }
}

void PackConv2WeightsCB(const float* c2_w, const float* c2_b, Conv2WeightsCBData* out) noexcept {
    std::memset(out, 0, sizeof(*out));
    for (int ky = 0; ky < 3; ++ky) {
        for (int kx = 0; kx < 3; ++kx) {
            const int s = ky * 3 + kx;
            for (int oc = 0; oc < 16; ++oc) {
                const int dst = (s * 16 + oc) * 4;
                for (int k = 0; k < 4; ++k) {
                    out->w[dst + 0][k] = c2_w[oc * 144 + (k +  0) * 9 + s];
                    out->w[dst + 1][k] = c2_w[oc * 144 + (k +  4) * 9 + s];
                    out->w[dst + 2][k] = c2_w[oc * 144 + (k +  8) * 9 + s];
                    out->w[dst + 3][k] = c2_w[oc * 144 + (k + 12) * 9 + s];
                }
            }
        }
    }
    for (int g = 0; g < 4; ++g) {
        for (int k = 0; k < 4; ++k) {
            out->b[g][k] = c2_b[g * 4 + k];
        }
    }
}

void PackConv3WeightsCB(const float* c3_w, const float* c3_b, Conv3WeightsCBData* out) noexcept {
    std::memset(out, 0, sizeof(*out));
    for (int ky = 0; ky < 3; ++ky) {
        for (int kx = 0; kx < 3; ++kx) {
            const int s = ky * 3 + kx;
            for (int oc = 0; oc < 7; ++oc) {
                const int dst = (s * 7 + oc) * 4;
                for (int k = 0; k < 4; ++k) {
                    out->w[dst + 0][k] = c3_w[oc * 144 + (k +  0) * 9 + s];
                    out->w[dst + 1][k] = c3_w[oc * 144 + (k +  4) * 9 + s];
                    out->w[dst + 2][k] = c3_w[oc * 144 + (k +  8) * 9 + s];
                    out->w[dst + 3][k] = c3_w[oc * 144 + (k + 12) * 9 + s];
                }
            }
        }
    }
    for (int k = 0; k < 4; ++k) {
        out->b[0][k] = c3_b[k];
    }
    for (int k = 0; k < 3; ++k) {
        out->b[1][k] = c3_b[4 + k];
    }
    out->b[1][3] = 0.0f;
}

// Saves & restores immediate D3D11DeviceContext pipeline bindings around our offscreen passes,
// and strictly unbinds all custom SRVs (8 slots) and MRT RTVs (4 slots) before Direct2D runs.
class ScopedD3D11State {
public:
    static constexpr UINT kMaxRtvs = 4u;
    static constexpr UINT kMaxSrvs = 8u;

    explicit ScopedD3D11State(ID3D11DeviceContext* ctx) noexcept : ctx_(ctx) {
        if (!ctx_) return;
        ctx_->OMGetRenderTargets(kMaxRtvs, rtvs_, &dsv_);
        ctx_->OMGetBlendState(&blend_, blend_factor_, &sample_mask_);
        ctx_->OMGetDepthStencilState(&dss_, &stencil_ref_);
        ctx_->RSGetState(&rs_);
        ctx_->RSGetViewports(&num_vps_, vps_);
        ctx_->IAGetInputLayout(&input_layout_);
        ctx_->IAGetPrimitiveTopology(&topology_);
        ctx_->VSGetShader(&vs_, nullptr, nullptr);
        ctx_->PSGetShader(&ps_, nullptr, nullptr);
        ctx_->PSGetConstantBuffers(0, 2, ps_cbs_);
        ctx_->PSGetSamplers(0, 2, ps_samplers_);
        ctx_->PSGetShaderResources(0, kMaxSrvs, ps_srvs_);
    }

    ~ScopedD3D11State() {
        if (!ctx_) return;

        ID3D11ShaderResourceView* null_srvs[kMaxSrvs] = {};
        ctx_->PSSetShaderResources(0, kMaxSrvs, null_srvs);
        ID3D11RenderTargetView* null_rtvs[kMaxRtvs] = {};
        ctx_->OMSetRenderTargets(kMaxRtvs, null_rtvs, nullptr);

        ctx_->VSSetShader(vs_, nullptr, 0);
        ctx_->PSSetShader(ps_, nullptr, 0);
        ctx_->IASetInputLayout(input_layout_);
        if (topology_ != D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED) {
            ctx_->IASetPrimitiveTopology(topology_);
        }
        ctx_->RSSetState(rs_);
        if (num_vps_ > 0) {
            ctx_->RSSetViewports(num_vps_, vps_);
        }
        ctx_->OMSetBlendState(blend_, blend_factor_, sample_mask_);
        ctx_->OMSetDepthStencilState(dss_, stencil_ref_);
        ctx_->PSSetConstantBuffers(0, 2, ps_cbs_);
        ctx_->PSSetSamplers(0, 2, ps_samplers_);
        ctx_->PSSetShaderResources(0, kMaxSrvs, ps_srvs_);
        ctx_->OMSetRenderTargets(kMaxRtvs, rtvs_, dsv_);

        for (auto*& p : ps_srvs_) SafeRelease(p);
        for (auto*& p : ps_samplers_) SafeRelease(p);
        for (auto*& p : ps_cbs_) SafeRelease(p);
        SafeRelease(ps_);
        SafeRelease(vs_);
        SafeRelease(input_layout_);
        SafeRelease(rs_);
        SafeRelease(dss_);
        SafeRelease(blend_);
        SafeRelease(dsv_);
        for (auto*& p : rtvs_) SafeRelease(p);
    }

    ScopedD3D11State(const ScopedD3D11State&) = delete;
    ScopedD3D11State& operator=(const ScopedD3D11State&) = delete;

private:
    ID3D11DeviceContext* ctx_ = nullptr;
    ID3D11RenderTargetView* rtvs_[kMaxRtvs] = {};
    ID3D11DepthStencilView* dsv_ = nullptr;
    ID3D11BlendState* blend_ = nullptr;
    FLOAT blend_factor_[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    UINT sample_mask_ = 0xFFFFFFFFu;
    ID3D11DepthStencilState* dss_ = nullptr;
    UINT stencil_ref_ = 0;
    ID3D11RasterizerState* rs_ = nullptr;
    UINT num_vps_ = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT vps_[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    ID3D11InputLayout* input_layout_ = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topology_ = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11VertexShader* vs_ = nullptr;
    ID3D11PixelShader* ps_ = nullptr;
    ID3D11Buffer* ps_cbs_[2] = {};
    ID3D11SamplerState* ps_samplers_[2] = {};
    ID3D11ShaderResourceView* ps_srvs_[kMaxSrvs] = {};
};

// ----------------------------------------------------------------------------
// Embedded SM4.0 (`vs_4_0` / `ps_4_0`) HLSL Shaders — v2 High-Quality Pipeline
// Multi-Scale (1/8 -> 1/4 -> 1/2) Coarse-to-Fine Optical Flow +
// 1/2-Res 3-Layer (8 -> 16 -> 16 -> 7) Residual CNN with RGB Disocclusion Repair.
// Compatible with every D3D10.0 / D3D11 iGPU and Windows Server 2022 WARP.
// ----------------------------------------------------------------------------

constexpr const char* kFullscreenVS = R"HLSL(
struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut main(uint vid : SV_VertexID) {
    VSOut o;
    float2 uv = float2((vid == 1u) ? 2.0 : 0.0, (vid == 2u) ? 2.0 : 0.0);
    o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    o.uv = uv;
    return o;
}
)HLSL";

// Pass 1a: 2x2 Box Downsample Full-Res BGRA -> Half-Res (1/2) Feature Pyramid (luma, dx, dy, grad_mag)
constexpr const char* kDownsampleHalfPS = R"HLSL(
cbuffer PassParams : register(b0) {
    float4 g_inv_full_half; // xy = 1 / full_size, zw = 1 / half_size
    float4 g_inv_qtr_eig;
    float4 g_pass_inv;
    float4 g_params;
    float4 g_extra;
};

Texture2D<float4> t_src : register(t0);
SamplerState s_linear : register(s0);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

float SampleBox2x2Luma(float2 center_uv) {
    // At half-res pixel centers, a single bilinear sample on full-res computes the exact 2x2 box average.
    float3 avg_rgb = t_src.SampleLevel(s_linear, center_uv, 0.0).rgb;
    return dot(avg_rgb, float3(0.2126, 0.7152, 0.0722));
}

float4 main(VSOut pin) : SV_Target0 {
    float2 uv = pin.uv;
    float2 h = g_inv_full_half.zw;
    float l_c = SampleBox2x2Luma(uv);
    float l_l = SampleBox2x2Luma(uv - float2(h.x, 0.0));
    float l_r = SampleBox2x2Luma(uv + float2(h.x, 0.0));
    float l_u = SampleBox2x2Luma(uv - float2(0.0, h.y));
    float l_d = SampleBox2x2Luma(uv + float2(0.0, h.y));
    float dx = 0.5 * (l_r - l_l);
    float dy = 0.5 * (l_d - l_u);
    float grad_mag = saturate(sqrt(dx * dx + dy * dy) * 4.0);
    return float4(l_c, dx, dy, grad_mag);
}
)HLSL";

// Pass 1b: 2x2 Box Downsample Pyramid Level (Half -> Quarter, Quarter -> Eighth), recomputing spatial gradients.
constexpr const char* kDownsamplePyrPS = R"HLSL(
cbuffer PassParams : register(b0) {
    float4 g_inv_full_half;
    float4 g_inv_qtr_eig;
    float4 g_pass_inv; // xy = 1 / src_size, zw = 1 / dst_size
    float4 g_params;
    float4 g_extra;
};

Texture2D<float4> t_src : register(t0);
SamplerState s_linear : register(s0);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

float SamplePyrLuma(float2 center_uv) {
    return t_src.SampleLevel(s_linear, center_uv, 0.0).x;
}

float4 main(VSOut pin) : SV_Target0 {
    float2 uv = pin.uv;
    float2 d = g_pass_inv.zw;
    float l_c = SamplePyrLuma(uv);
    float l_l = SamplePyrLuma(uv - float2(d.x, 0.0));
    float l_r = SamplePyrLuma(uv + float2(d.x, 0.0));
    float l_u = SamplePyrLuma(uv - float2(0.0, d.y));
    float l_d = SamplePyrLuma(uv + float2(0.0, d.y));
    float dx = 0.5 * (l_r - l_l);
    float dy = 0.5 * (l_d - l_u);
    float grad_mag = saturate(sqrt(dx * dx + dy * dy) * 4.0);
    return float4(l_c, dx, dy, grad_mag);
}
)HLSL";

// Pass 2a: Coarse Optical Flow at 1/8 Resolution -> +-16 eighth-px (+-128 full-res px!)
// Fast hierarchical stride-4 grid + 2-stage diamond/3x3 refinement + subpixel polish.
constexpr const char* kFlowCoarseEighthPS = R"HLSL(
cbuffer PassParams : register(b0) {
    float4 g_inv_full_half;
    float4 g_inv_qtr_eig; // xy = 1 / qtr_size, zw = 1 / eig_size
    float4 g_pass_inv;
    float4 g_params;
    float4 g_extra;
};

Texture2D<float4> t_feat0 : register(t0);
Texture2D<float4> t_feat1 : register(t1);
SamplerState s_linear : register(s0);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

float FeatureDiff(float4 a, float4 b) {
    return abs(a.x - b.x) + 0.5 * (abs(a.y - b.y) + abs(a.z - b.z));
}

float EvalCandidate(float2 uv, float2 v, float radius_px) {
    float2 e = g_inv_qtr_eig.zw;
    float2 half_duv = 0.5 * v * e;
    float2 uv0 = uv - half_duv;
    float2 uv1 = uv + half_duv;

    float oob = 0.0;
    if (uv0.x < 0.0 || uv0.x > 1.0 || uv0.y < 0.0 || uv0.y > 1.0 ||
        uv1.x < 0.0 || uv1.x > 1.0 || uv1.y < 0.0 || uv1.y > 1.0) {
        oob = 0.15;
    }

    float2 rx = float2(radius_px * e.x, 0.0);
    float2 ry = float2(0.0, radius_px * e.y);

    float cost = FeatureDiff(t_feat0.SampleLevel(s_linear, uv0, 0.0),
                             t_feat1.SampleLevel(s_linear, uv1, 0.0)) * 1.5;
    cost += FeatureDiff(t_feat0.SampleLevel(s_linear, uv0 - rx, 0.0),
                        t_feat1.SampleLevel(s_linear, uv1 - rx, 0.0));
    cost += FeatureDiff(t_feat0.SampleLevel(s_linear, uv0 + rx, 0.0),
                        t_feat1.SampleLevel(s_linear, uv1 + rx, 0.0));
    cost += FeatureDiff(t_feat0.SampleLevel(s_linear, uv0 - ry, 0.0),
                        t_feat1.SampleLevel(s_linear, uv1 - ry, 0.0));
    cost += FeatureDiff(t_feat0.SampleLevel(s_linear, uv0 + ry, 0.0),
                        t_feat1.SampleLevel(s_linear, uv1 + ry, 0.0));
    return cost * (1.0 / 5.5) + 0.0008 * length(v) + oob;
}

float TempDiffAt(float2 p_uv) {
    return abs(t_feat1.SampleLevel(s_linear, p_uv, 0.0).x -
               t_feat0.SampleLevel(s_linear, p_uv, 0.0).x);
}

float4 main(VSOut pin) : SV_Target0 {
    float2 uv = pin.uv;
    float2 e = g_inv_qtr_eig.zw;

    // Wide static early-out spanning +-16 eighth-px (+-128 full-res px) so fast Stage Manager swoops are never skipped.
    float max_nb = TempDiffAt(uv);
    max_nb = max(max_nb, max(TempDiffAt(uv - float2( 4.0 * e.x, 0.0)), TempDiffAt(uv + float2( 4.0 * e.x, 0.0))));
    max_nb = max(max_nb, max(TempDiffAt(uv - float2(0.0,  4.0 * e.y)), TempDiffAt(uv + float2(0.0,  4.0 * e.y))));
    max_nb = max(max_nb, max(TempDiffAt(uv - float2(10.0 * e.x, 0.0)), TempDiffAt(uv + float2(10.0 * e.x, 0.0))));
    max_nb = max(max_nb, max(TempDiffAt(uv - float2(0.0, 10.0 * e.y)), TempDiffAt(uv + float2(0.0, 10.0 * e.y))));
    max_nb = max(max_nb, max(TempDiffAt(uv - float2(16.0 * e.x, 0.0)), TempDiffAt(uv + float2(16.0 * e.x, 0.0))));
    max_nb = max(max_nb, max(TempDiffAt(uv - float2(0.0, 16.0 * e.y)), TempDiffAt(uv + float2(0.0, 16.0 * e.y))));
    if (max_nb < 0.008) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    // Stage 1: Coarse stride-4 grid over [-16, +16] eighth-px (+-128 full-res px)
    float2 best_v = float2(0.0, 0.0);
    float best_cost = EvalCandidate(uv, best_v, 2.0);
    [loop]
    for (int gy = -4; gy <= 4; ++gy) {
        [loop]
        for (int gx = -4; gx <= 4; ++gx) {
            if (gx != 0 || gy != 0) {
                float2 cand = float2((float)gx * 4.0, (float)gy * 4.0);
                float c = EvalCandidate(uv, cand, 2.0);
                if (c < best_cost) {
                    best_cost = c;
                    best_v = cand;
                }
            }
        }
    }

    // Stage 2: Refinement step = 2.0 eighth-px (3x3 around Stage 1 winner)
    float2 center2 = best_v;
    [loop]
    for (int ry = -1; ry <= 1; ++ry) {
        [loop]
        for (int rx = -1; rx <= 1; ++rx) {
            if (rx != 0 || ry != 0) {
                float2 cand = clamp(center2 + float2((float)rx * 2.0, (float)ry * 2.0), -16.0, 16.0);
                float c = EvalCandidate(uv, cand, 1.5);
                if (c < best_cost) {
                    best_cost = c;
                    best_v = cand;
                }
            }
        }
    }

    // Stage 3: Refinement step = 1.0 eighth-px (3x3 around Stage 2 winner)
    float2 center3 = best_v;
    float best_fine = EvalCandidate(uv, center3, 1.0);
    [loop]
    for (int fy = -1; fy <= 1; ++fy) {
        [loop]
        for (int fx = -1; fx <= 1; ++fx) {
            if (fx != 0 || fy != 0) {
                float2 cand = clamp(center3 + float2((float)fx, (float)fy), -16.0, 16.0);
                float c = EvalCandidate(uv, cand, 1.0);
                if (c < best_fine) {
                    best_fine = c;
                    best_v = cand;
                }
            }
        }
    }

    // Stage 4: Sub-eighth-pixel diamond refinement (step = 0.5 eighth-px)
    static const float2 kDiamond[4] = {
        float2(-0.5, 0.0), float2(0.5, 0.0), float2(0.0, -0.5), float2(0.0, 0.5)
    };
    float2 center4 = best_v;
    [unroll]
    for (int di = 0; di < 4; ++di) {
        float2 cand = clamp(center4 + kDiamond[di], -16.0, 16.0);
        float c = EvalCandidate(uv, cand, 1.0);
        if (c < best_fine) {
            best_fine = c;
            best_v = cand;
        }
    }

    return float4(best_v.x, best_v.y, best_fine, 1.0);
}
)HLSL";

// Pass 2b: Optical Flow Refinement at 1/4 Resolution -> upsamples 1/8 prior * 2.0,
// tests zero-motion candidate + neighbor priors + +-4 quarter-px (+-16 full-res px) refinement.
constexpr const char* kFlowRefineQuarterPS = R"HLSL(
cbuffer PassParams : register(b0) {
    float4 g_inv_full_half;
    float4 g_inv_qtr_eig; // xy = 1 / qtr_size, zw = 1 / eig_size
    float4 g_pass_inv;
    float4 g_params;
    float4 g_extra;
};

Texture2D<float4> t_feat0    : register(t0);
Texture2D<float4> t_feat1    : register(t1);
Texture2D<float4> t_flow_eig : register(t2);
SamplerState s_linear : register(s0);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

float FeatureDiff(float4 a, float4 b) {
    return abs(a.x - b.x) + 0.5 * (abs(a.y - b.y) + abs(a.z - b.z));
}

float EvalCandidate(float2 uv, float2 v, float radius_px) {
    float2 q = g_inv_qtr_eig.xy;
    float2 half_duv = 0.5 * v * q;
    float2 uv0 = uv - half_duv;
    float2 uv1 = uv + half_duv;

    float oob = 0.0;
    if (uv0.x < 0.0 || uv0.x > 1.0 || uv0.y < 0.0 || uv0.y > 1.0 ||
        uv1.x < 0.0 || uv1.x > 1.0 || uv1.y < 0.0 || uv1.y > 1.0) {
        oob = 0.15;
    }

    float2 rx = float2(radius_px * q.x, 0.0);
    float2 ry = float2(0.0, radius_px * q.y);

    float cost = FeatureDiff(t_feat0.SampleLevel(s_linear, uv0, 0.0),
                             t_feat1.SampleLevel(s_linear, uv1, 0.0)) * 1.5;
    cost += FeatureDiff(t_feat0.SampleLevel(s_linear, uv0 - rx, 0.0),
                        t_feat1.SampleLevel(s_linear, uv1 - rx, 0.0));
    cost += FeatureDiff(t_feat0.SampleLevel(s_linear, uv0 + rx, 0.0),
                        t_feat1.SampleLevel(s_linear, uv1 + rx, 0.0));
    cost += FeatureDiff(t_feat0.SampleLevel(s_linear, uv0 - ry, 0.0),
                        t_feat1.SampleLevel(s_linear, uv1 - ry, 0.0));
    cost += FeatureDiff(t_feat0.SampleLevel(s_linear, uv0 + ry, 0.0),
                        t_feat1.SampleLevel(s_linear, uv1 + ry, 0.0));
    return cost * (1.0 / 5.5) + 0.0006 * length(v) + oob;
}

float4 main(VSOut pin) : SV_Target0 {
    float2 uv = pin.uv;
    float2 e = g_inv_qtr_eig.zw;

    float4 f0_c = t_feat0.SampleLevel(s_linear, uv, 0.0);
    float4 f1_c = t_feat1.SampleLevel(s_linear, uv, 0.0);
    float diff_c = abs(f1_c.x - f0_c.x);

    float2 p_c = t_flow_eig.SampleLevel(s_linear, uv, 0.0).xy * 2.0;
    float2 p_l = t_flow_eig.SampleLevel(s_linear, uv - float2(e.x, 0.0), 0.0).xy * 2.0;
    float2 p_r = t_flow_eig.SampleLevel(s_linear, uv + float2(e.x, 0.0), 0.0).xy * 2.0;
    float2 p_u = t_flow_eig.SampleLevel(s_linear, uv - float2(0.0, e.y), 0.0).xy * 2.0;
    float2 p_d = t_flow_eig.SampleLevel(s_linear, uv + float2(0.0, e.y), 0.0).xy * 2.0;

    float max_prior = max(length(p_c), max(max(length(p_l), length(p_r)), max(length(p_u), length(p_d))));
    if (diff_c < 0.008 && max_prior < 0.25) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    // Start from upsampled prior, zero-motion candidate (protects stationary borders), and 4 spatial neighbor priors.
    float2 best_v = clamp(p_c, -36.0, 36.0);
    float best_cost = EvalCandidate(uv, best_v, 1.5);

    float c_zero = EvalCandidate(uv, float2(0.0, 0.0), 1.5);
    if (c_zero < best_cost) {
        best_cost = c_zero;
        best_v = float2(0.0, 0.0);
    }

    float2 cand_l = clamp(p_l, -36.0, 36.0);
    float c_l = EvalCandidate(uv, cand_l, 1.5);
    if (c_l < best_cost) { best_cost = c_l; best_v = cand_l; }
    float2 cand_r = clamp(p_r, -36.0, 36.0);
    float c_r = EvalCandidate(uv, cand_r, 1.5);
    if (c_r < best_cost) { best_cost = c_r; best_v = cand_r; }
    float2 cand_u = clamp(p_u, -36.0, 36.0);
    float c_u = EvalCandidate(uv, cand_u, 1.5);
    if (c_u < best_cost) { best_cost = c_u; best_v = cand_u; }
    float2 cand_d = clamp(p_d, -36.0, 36.0);
    float c_d = EvalCandidate(uv, cand_d, 1.5);
    if (c_d < best_cost) { best_cost = c_d; best_v = cand_d; }

    // Stage 1: +-4 quarter-px coarse search around winner (3x3 step-2 + axial +-4)
    float2 center1 = best_v;
    [loop]
    for (int ry = -1; ry <= 1; ++ry) {
        [loop]
        for (int rx = -1; rx <= 1; ++rx) {
            if (rx != 0 || ry != 0) {
                float2 cand = clamp(center1 + float2((float)rx * 2.0, (float)ry * 2.0), -36.0, 36.0);
                float c = EvalCandidate(uv, cand, 1.5);
                if (c < best_cost) {
                    best_cost = c;
                    best_v = cand;
                }
            }
        }
    }
    static const float2 kAxial4[4] = {
        float2(-4.0, 0.0), float2(4.0, 0.0), float2(0.0, -4.0), float2(0.0, 4.0)
    };
    [unroll]
    for (int ai = 0; ai < 4; ++ai) {
        float2 cand = clamp(center1 + kAxial4[ai], -36.0, 36.0);
        float c = EvalCandidate(uv, cand, 1.5);
        if (c < best_cost) {
            best_cost = c;
            best_v = cand;
        }
    }

    // Stage 2: Fine 3x3 step = 1.0 quarter-px
    float2 center2 = best_v;
    float best_fine = EvalCandidate(uv, center2, 1.0);
    [loop]
    for (int fy = -1; fy <= 1; ++fy) {
        [loop]
        for (int fx = -1; fx <= 1; ++fx) {
            if (fx != 0 || fy != 0) {
                float2 cand = clamp(center2 + float2((float)fx, (float)fy), -36.0, 36.0);
                float c = EvalCandidate(uv, cand, 1.0);
                if (c < best_fine) {
                    best_fine = c;
                    best_v = cand;
                }
            }
        }
    }

    // Stage 3: Sub-quarter-pixel diamond refinement (step = 0.5 quarter-px)
    static const float2 kDiamond[4] = {
        float2(-0.5, 0.0), float2(0.5, 0.0), float2(0.0, -0.5), float2(0.0, 0.5)
    };
    float2 center3 = best_v;
    [unroll]
    for (int di = 0; di < 4; ++di) {
        float2 cand = clamp(center3 + kDiamond[di], -36.0, 36.0);
        float c = EvalCandidate(uv, cand, 1.0);
        if (c < best_fine) {
            best_fine = c;
            best_v = cand;
        }
    }

    return float4(best_v.x, best_v.y, best_fine, 1.0);
}
)HLSL";

// Pass 2c: Optical Flow Refinement at 1/2 Resolution -> upsamples 1/4 prior * 2.0,
// refines +-3 half-px (+ zero-motion + 0.5-half-px subpixel diamond -> +-150 full-res px total!),
// computes occlusion blend_w and crisp static-text protection motion_conf.
constexpr const char* kFlowRefineHalfPS = R"HLSL(
cbuffer PassParams : register(b0) {
    float4 g_inv_full_half; // xy = 1 / full_size, zw = 1 / half_size
    float4 g_inv_qtr_eig;   // xy = 1 / qtr_size,  zw = 1 / eig_size
    float4 g_pass_inv;
    float4 g_params;
    float4 g_extra;
};

Texture2D<float4> t_feat0    : register(t0);
Texture2D<float4> t_feat1    : register(t1);
Texture2D<float4> t_flow_qtr : register(t2);
SamplerState s_linear : register(s0);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

float FeatureDiff(float4 a, float4 b) {
    return abs(a.x - b.x) + 0.5 * (abs(a.y - b.y) + abs(a.z - b.z));
}

float EvalCandidate(float2 uv, float2 v, float radius_px) {
    float2 h = g_inv_full_half.zw;
    float2 half_duv = 0.5 * v * h;
    float2 uv0 = uv - half_duv;
    float2 uv1 = uv + half_duv;

    float oob = 0.0;
    if (uv0.x < 0.0 || uv0.x > 1.0 || uv0.y < 0.0 || uv0.y > 1.0 ||
        uv1.x < 0.0 || uv1.x > 1.0 || uv1.y < 0.0 || uv1.y > 1.0) {
        oob = 0.15;
    }

    float2 rx = float2(radius_px * h.x, 0.0);
    float2 ry = float2(0.0, radius_px * h.y);

    float cost = FeatureDiff(t_feat0.SampleLevel(s_linear, uv0, 0.0),
                             t_feat1.SampleLevel(s_linear, uv1, 0.0)) * 1.5;
    cost += FeatureDiff(t_feat0.SampleLevel(s_linear, uv0 - rx, 0.0),
                        t_feat1.SampleLevel(s_linear, uv1 - rx, 0.0));
    cost += FeatureDiff(t_feat0.SampleLevel(s_linear, uv0 + rx, 0.0),
                        t_feat1.SampleLevel(s_linear, uv1 + rx, 0.0));
    cost += FeatureDiff(t_feat0.SampleLevel(s_linear, uv0 - ry, 0.0),
                        t_feat1.SampleLevel(s_linear, uv1 - ry, 0.0));
    cost += FeatureDiff(t_feat0.SampleLevel(s_linear, uv0 + ry, 0.0),
                        t_feat1.SampleLevel(s_linear, uv1 + ry, 0.0));
    return cost * (1.0 / 5.5) + 0.0004 * length(v) + oob;
}

float4 main(VSOut pin) : SV_Target0 {
    float2 uv = pin.uv;
    float2 h = g_inv_full_half.zw;
    float2 q = g_inv_qtr_eig.xy;

    float4 f0_c = t_feat0.SampleLevel(s_linear, uv, 0.0);
    float4 f1_c = t_feat1.SampleLevel(s_linear, uv, 0.0);
    float diff_c = abs(f1_c.x - f0_c.x);

    float2 p_c = t_flow_qtr.SampleLevel(s_linear, uv, 0.0).xy * 2.0;
    float2 p_l = t_flow_qtr.SampleLevel(s_linear, uv - float2(q.x, 0.0), 0.0).xy * 2.0;
    float2 p_r = t_flow_qtr.SampleLevel(s_linear, uv + float2(q.x, 0.0), 0.0).xy * 2.0;
    float2 p_u = t_flow_qtr.SampleLevel(s_linear, uv - float2(0.0, q.y), 0.0).xy * 2.0;
    float2 p_d = t_flow_qtr.SampleLevel(s_linear, uv + float2(0.0, q.y), 0.0).xy * 2.0;

    float max_prior = max(length(p_c), max(max(length(p_l), length(p_r)), max(length(p_u), length(p_d))));
    if (diff_c < 0.008 && max_prior < 0.25) {
        return float4(0.0, 0.0, 0.5, 0.0);
    }

    // Evaluate upsampled prior, zero-motion candidate, and 4 spatial neighbor priors for sharp window boundaries.
    float2 best_v = clamp(p_c, -75.0, 75.0);
    float best_cost = EvalCandidate(uv, best_v, 1.5);

    float c_zero = EvalCandidate(uv, float2(0.0, 0.0), 1.5);
    if (c_zero < best_cost) {
        best_cost = c_zero;
        best_v = float2(0.0, 0.0);
    }

    float2 cand_l = clamp(p_l, -75.0, 75.0);
    float c_l = EvalCandidate(uv, cand_l, 1.5);
    if (c_l < best_cost) { best_cost = c_l; best_v = cand_l; }
    float2 cand_r = clamp(p_r, -75.0, 75.0);
    float c_r = EvalCandidate(uv, cand_r, 1.5);
    if (c_r < best_cost) { best_cost = c_r; best_v = cand_r; }
    float2 cand_u = clamp(p_u, -75.0, 75.0);
    float c_u = EvalCandidate(uv, cand_u, 1.5);
    if (c_u < best_cost) { best_cost = c_u; best_v = cand_u; }
    float2 cand_d = clamp(p_d, -75.0, 75.0);
    float c_d = EvalCandidate(uv, cand_d, 1.5);
    if (c_d < best_cost) { best_cost = c_d; best_v = cand_d; }

    // Stage 1: Step = 2.0 half-px 3x3 around winner
    float2 center1 = best_v;
    [loop]
    for (int ry = -1; ry <= 1; ++ry) {
        [loop]
        for (int rx = -1; rx <= 1; ++rx) {
            if (rx != 0 || ry != 0) {
                float2 cand = clamp(center1 + float2((float)rx * 2.0, (float)ry * 2.0), -75.0, 75.0);
                float c = EvalCandidate(uv, cand, 1.5);
                if (c < best_cost) {
                    best_cost = c;
                    best_v = cand;
                }
            }
        }
    }

    // Stage 2: Step = 1.0 half-px 3x3 (total +-3 half-px refinement around prior)
    float2 center2 = best_v;
    float best_fine = EvalCandidate(uv, center2, 1.0);
    [loop]
    for (int fy = -1; fy <= 1; ++fy) {
        [loop]
        for (int fx = -1; fx <= 1; ++fx) {
            if (fx != 0 || fy != 0) {
                float2 cand = clamp(center2 + float2((float)fx, (float)fy), -75.0, 75.0);
                float c = EvalCandidate(uv, cand, 1.0);
                if (c < best_fine) {
                    best_fine = c;
                    best_v = cand;
                }
            }
        }
    }

    // Stage 3: Sub-half-pixel diamond refinement (step = 0.5 half-px = 1 full-res px precision!)
    static const float2 kDiamond[4] = {
        float2(-0.5, 0.0), float2(0.5, 0.0), float2(0.0, -0.5), float2(0.0, 0.5)
    };
    float2 center3 = best_v;
    [unroll]
    for (int di = 0; di < 4; ++di) {
        float2 cand = clamp(center3 + kDiamond[di], -75.0, 75.0);
        float c = EvalCandidate(uv, cand, 1.0);
        if (c < best_fine) {
            best_fine = c;
            best_v = cand;
        }
    }

    // Forward/backward error asymmetry for Stage Manager window occlusion
    float4 f0_bwd = t_feat0.SampleLevel(s_linear, uv - best_v * h, 0.0);
    float4 f1_fwd = t_feat1.SampleLevel(s_linear, uv + best_v * h, 0.0);
    float err_from_0 = FeatureDiff(f0_c, f1_fwd);
    float err_from_1 = FeatureDiff(f1_c, f0_bwd);
    float blend_w = clamp(0.5 + (err_from_0 - err_from_1) * 1.5, 0.15, 0.85);

    // Crisp static-text protection + clean detection of fast objects crossing midpoint uv
    float f0_mid = t_feat0.SampleLevel(s_linear, uv - 0.5 * best_v * h, 0.0).x;
    float f1_mid = t_feat1.SampleLevel(s_linear, uv + 0.5 * best_v * h, 0.0).x;
    float cross_diff = (best_fine < 0.045) ? min(abs(f0_mid - f0_c.x), abs(f1_mid - f1_c.x)) : 0.0;
    float effective_diff = max(diff_c, cross_diff);
    float motion_conf = saturate((effective_diff - 0.012) * 20.0);
    if (length(best_v) < 0.25 || effective_diff < 0.012) {
        best_v = float2(0.0, 0.0);
        motion_conf = 0.0;
    }

    return float4(best_v.x, best_v.y, blend_w, motion_conf);
}
)HLSL";

// Pass 3a: Neural Conv1 (8 -> 16, 3x3 + ReLU) at 1/2 Resolution -> 4 MRTs (`hidden1_[0..3]`)
constexpr const char* kNeuralConv1PS = R"HLSL(
cbuffer PassParams : register(b0) {
    float4 g_inv_full_half; // xy = 1 / full_size, zw = 1 / half_size
    float4 g_inv_qtr_eig;
    float4 g_pass_inv;
    float4 g_params;        // x = alpha, z = 1 / kFlowNormScale
    float4 g_extra;
};

cbuffer Conv1Weights : register(b1) {
    float4 g_w[288];
    float4 g_b[4];
};

Texture2D<float4> t_feat0       : register(t0);
Texture2D<float4> t_feat1       : register(t1);
Texture2D<float4> t_flow_coarse : register(t2);
SamplerState s_linear : register(s0);
SamplerState s_point  : register(s1);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

struct Conv4MRT {
    float4 h0 : SV_Target0;
    float4 h1 : SV_Target1;
    float4 h2 : SV_Target2;
    float4 h3 : SV_Target3;
};

void GatherChannels8(float2 h_uv, out float4 ch0_3, out float4 ch4_7) {
    float2 h = g_inv_full_half.zw;
    float alpha = g_params.x;
    float flow_norm_inv = g_params.z;

    float4 f0 = t_feat0.SampleLevel(s_point, h_uv, 0.0);
    float4 f1 = t_feat1.SampleLevel(s_point, h_uv, 0.0);
    float4 fc = t_flow_coarse.SampleLevel(s_point, h_uv, 0.0);

    float i0 = f0.x;
    float i1 = f1.x;
    float2 v_c = fc.xy;
    float w0 = t_feat0.SampleLevel(s_linear, h_uv - alpha * v_c * h, 0.0).x;
    float w1 = t_feat1.SampleLevel(s_linear, h_uv + (1.0 - alpha) * v_c * h, 0.0).x;
    float warped_diff = clamp(w1 - w0, -1.0, 1.0);
    float grad_mag = saturate(f0.w);
    float vx_norm = clamp(v_c.x * flow_norm_inv, -1.0, 1.0);
    float vy_norm = clamp(v_c.y * flow_norm_inv, -1.0, 1.0);
    float motion_prior = saturate(abs(i1 - i0) * 6.0);

    ch0_3 = float4(i0, i1, warped_diff, grad_mag);
    ch4_7 = float4(vx_norm, vy_norm, alpha, motion_prior);
}

Conv4MRT main(VSOut pin) {
    float2 uv = pin.uv;
    float2 h = g_inv_full_half.zw;

    float4 h0 = g_b[0];
    float4 h1 = g_b[1];
    float4 h2 = g_b[2];
    float4 h3 = g_b[3];

    [unroll]
    for (int ky = 0; ky < 3; ++ky) {
        [unroll]
        for (int kx = 0; kx < 3; ++kx) {
            int s = ky * 3 + kx;
            int base = s * 32;
            float2 sample_uv = uv + float2((float)(kx - 1) * h.x, (float)(ky - 1) * h.y);
            float4 x0;
            float4 x1;
            GatherChannels8(sample_uv, x0, x1);

            h0.x += dot(x0, g_w[base +  0]) + dot(x1, g_w[base +  1]);
            h0.y += dot(x0, g_w[base +  2]) + dot(x1, g_w[base +  3]);
            h0.z += dot(x0, g_w[base +  4]) + dot(x1, g_w[base +  5]);
            h0.w += dot(x0, g_w[base +  6]) + dot(x1, g_w[base +  7]);

            h1.x += dot(x0, g_w[base +  8]) + dot(x1, g_w[base +  9]);
            h1.y += dot(x0, g_w[base + 10]) + dot(x1, g_w[base + 11]);
            h1.z += dot(x0, g_w[base + 12]) + dot(x1, g_w[base + 13]);
            h1.w += dot(x0, g_w[base + 14]) + dot(x1, g_w[base + 15]);

            h2.x += dot(x0, g_w[base + 16]) + dot(x1, g_w[base + 17]);
            h2.y += dot(x0, g_w[base + 18]) + dot(x1, g_w[base + 19]);
            h2.z += dot(x0, g_w[base + 20]) + dot(x1, g_w[base + 21]);
            h2.w += dot(x0, g_w[base + 22]) + dot(x1, g_w[base + 23]);

            h3.x += dot(x0, g_w[base + 24]) + dot(x1, g_w[base + 25]);
            h3.y += dot(x0, g_w[base + 26]) + dot(x1, g_w[base + 27]);
            h3.z += dot(x0, g_w[base + 28]) + dot(x1, g_w[base + 29]);
            h3.w += dot(x0, g_w[base + 30]) + dot(x1, g_w[base + 31]);
        }
    }

    Conv4MRT outp;
    outp.h0 = max(h0, 0.0);
    outp.h1 = max(h1, 0.0);
    outp.h2 = max(h2, 0.0);
    outp.h3 = max(h3, 0.0);
    return outp;
}
)HLSL";

// Pass 3b: Neural Conv2 (16 -> 16, 3x3 + residual skip + ReLU) at 1/2 Resolution -> 4 MRTs (`hidden2_[0..3]`)
constexpr const char* kNeuralConv2PS = R"HLSL(
cbuffer PassParams : register(b0) {
    float4 g_inv_full_half; // zw = 1 / half_size
    float4 g_inv_qtr_eig;
    float4 g_pass_inv;
    float4 g_params;
    float4 g_extra;
};

cbuffer Conv2Weights : register(b1) {
    float4 g_w[576];
    float4 g_b[4];
};

Texture2D<float4> t_h0 : register(t0);
Texture2D<float4> t_h1 : register(t1);
Texture2D<float4> t_h2 : register(t2);
Texture2D<float4> t_h3 : register(t3);
SamplerState s_point : register(s1);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

struct Conv4MRT {
    float4 h0 : SV_Target0;
    float4 h1 : SV_Target1;
    float4 h2 : SV_Target2;
    float4 h3 : SV_Target3;
};

Conv4MRT main(VSOut pin) {
    float2 uv = pin.uv;
    float2 h = g_inv_full_half.zw;

    float4 o0 = g_b[0];
    float4 o1 = g_b[1];
    float4 o2 = g_b[2];
    float4 o3 = g_b[3];

    [unroll]
    for (int ky = 0; ky < 3; ++ky) {
        [unroll]
        for (int kx = 0; kx < 3; ++kx) {
            int s = ky * 3 + kx;
            int base = s * 64;
            float2 sample_uv = uv + float2((float)(kx - 1) * h.x, (float)(ky - 1) * h.y);
            float4 x0 = t_h0.SampleLevel(s_point, sample_uv, 0.0);
            float4 x1 = t_h1.SampleLevel(s_point, sample_uv, 0.0);
            float4 x2 = t_h2.SampleLevel(s_point, sample_uv, 0.0);
            float4 x3 = t_h3.SampleLevel(s_point, sample_uv, 0.0);

            if (ky == 1 && kx == 1) {
                o0 += x0;
                o1 += x1;
                o2 += x2;
                o3 += x3;
            }

            o0.x += dot(x0, g_w[base +  0]) + dot(x1, g_w[base +  1]) + dot(x2, g_w[base +  2]) + dot(x3, g_w[base +  3]);
            o0.y += dot(x0, g_w[base +  4]) + dot(x1, g_w[base +  5]) + dot(x2, g_w[base +  6]) + dot(x3, g_w[base +  7]);
            o0.z += dot(x0, g_w[base +  8]) + dot(x1, g_w[base +  9]) + dot(x2, g_w[base + 10]) + dot(x3, g_w[base + 11]);
            o0.w += dot(x0, g_w[base + 12]) + dot(x1, g_w[base + 13]) + dot(x2, g_w[base + 14]) + dot(x3, g_w[base + 15]);

            o1.x += dot(x0, g_w[base + 16]) + dot(x1, g_w[base + 17]) + dot(x2, g_w[base + 18]) + dot(x3, g_w[base + 19]);
            o1.y += dot(x0, g_w[base + 20]) + dot(x1, g_w[base + 21]) + dot(x2, g_w[base + 22]) + dot(x3, g_w[base + 23]);
            o1.z += dot(x0, g_w[base + 24]) + dot(x1, g_w[base + 25]) + dot(x2, g_w[base + 26]) + dot(x3, g_w[base + 27]);
            o1.w += dot(x0, g_w[base + 28]) + dot(x1, g_w[base + 29]) + dot(x2, g_w[base + 30]) + dot(x3, g_w[base + 31]);

            o2.x += dot(x0, g_w[base + 32]) + dot(x1, g_w[base + 33]) + dot(x2, g_w[base + 34]) + dot(x3, g_w[base + 35]);
            o2.y += dot(x0, g_w[base + 36]) + dot(x1, g_w[base + 37]) + dot(x2, g_w[base + 38]) + dot(x3, g_w[base + 39]);
            o2.z += dot(x0, g_w[base + 40]) + dot(x1, g_w[base + 41]) + dot(x2, g_w[base + 42]) + dot(x3, g_w[base + 43]);
            o2.w += dot(x0, g_w[base + 44]) + dot(x1, g_w[base + 45]) + dot(x2, g_w[base + 46]) + dot(x3, g_w[base + 47]);

            o3.x += dot(x0, g_w[base + 48]) + dot(x1, g_w[base + 49]) + dot(x2, g_w[base + 50]) + dot(x3, g_w[base + 51]);
            o3.y += dot(x0, g_w[base + 52]) + dot(x1, g_w[base + 53]) + dot(x2, g_w[base + 54]) + dot(x3, g_w[base + 55]);
            o3.z += dot(x0, g_w[base + 56]) + dot(x1, g_w[base + 57]) + dot(x2, g_w[base + 58]) + dot(x3, g_w[base + 59]);
            o3.w += dot(x0, g_w[base + 60]) + dot(x1, g_w[base + 61]) + dot(x2, g_w[base + 62]) + dot(x3, g_w[base + 63]);
        }
    }

    Conv4MRT outp;
    outp.h0 = max(o0, 0.0);
    outp.h1 = max(o1, 0.0);
    outp.h2 = max(o2, 0.0);
    outp.h3 = max(o3, 0.0);
    return outp;
}
)HLSL";

// Pass 3c: Neural Conv3 (16 -> 7, 3x3) at 1/2 Resolution -> 2 MRTs (`flow_half_refined_`, `rgb_residual_`)
constexpr const char* kNeuralConv3PS = R"HLSL(
cbuffer PassParams : register(b0) {
    float4 g_inv_full_half; // zw = 1 / half_size
    float4 g_inv_qtr_eig;
    float4 g_pass_inv;
    float4 g_params;        // w = kResidualFlowScale (6.0)
    float4 g_extra;         // x = kRgbResidualScale (0.25)
};

cbuffer Conv3Weights : register(b1) {
    float4 g_w[252];
    float4 g_b[2];
};

Texture2D<float4> t_h0          : register(t0);
Texture2D<float4> t_h1          : register(t1);
Texture2D<float4> t_h2          : register(t2);
Texture2D<float4> t_h3          : register(t3);
Texture2D<float4> t_flow_coarse : register(t4);
Texture2D<float4> t_feat0       : register(t5);
Texture2D<float4> t_feat1       : register(t6);
SamplerState s_point : register(s1);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

struct Conv3Out {
    float4 flow_refined : SV_Target0;
    float4 rgb_residual : SV_Target1;
};

float2 SafeTanh2(float2 x) {
    float2 c = clamp(x, -10.0, 10.0);
    float2 e2 = exp(-2.0 * c);
    return (1.0 - e2) / (1.0 + e2);
}

float3 SafeTanh3(float3 x) {
    float3 c = clamp(x, -10.0, 10.0);
    float3 e2 = exp(-2.0 * c);
    return (1.0 - e2) / (1.0 + e2);
}

float SafeSigmoid(float x) {
    float c = clamp(x, -15.0, 15.0);
    return 1.0 / (1.0 + exp(-c));
}

Conv3Out main(VSOut pin) {
    float2 uv = pin.uv;
    float2 h = g_inv_full_half.zw;

    float i0 = t_feat0.SampleLevel(s_point, uv, 0.0).x;
    float i1 = t_feat1.SampleLevel(s_point, uv, 0.0).x;
    float4 fc = t_flow_coarse.SampleLevel(s_point, uv, 0.0);
    float2 v_c = fc.xy;

    // Hard static-text protection gate: zero motion_conf and zero RGB residual on static pixels.
    if (abs(i1 - i0) < 0.008 && length(v_c) < 0.25) {
        Conv3Out static_out;
        static_out.flow_refined = float4(0.0, 0.0, 0.5, 0.0);
        static_out.rgb_residual = float4(0.0, 0.0, 0.0, 1.0);
        return static_out;
    }

    float4 o0 = g_b[0];
    float3 o1 = g_b[1].xyz;

    [unroll]
    for (int ky = 0; ky < 3; ++ky) {
        [unroll]
        for (int kx = 0; kx < 3; ++kx) {
            int s = ky * 3 + kx;
            int base = s * 28;
            float2 sample_uv = uv + float2((float)(kx - 1) * h.x, (float)(ky - 1) * h.y);
            float4 x0 = t_h0.SampleLevel(s_point, sample_uv, 0.0);
            float4 x1 = t_h1.SampleLevel(s_point, sample_uv, 0.0);
            float4 x2 = t_h2.SampleLevel(s_point, sample_uv, 0.0);
            float4 x3 = t_h3.SampleLevel(s_point, sample_uv, 0.0);

            o0.x += dot(x0, g_w[base +  0]) + dot(x1, g_w[base +  1]) + dot(x2, g_w[base +  2]) + dot(x3, g_w[base +  3]);
            o0.y += dot(x0, g_w[base +  4]) + dot(x1, g_w[base +  5]) + dot(x2, g_w[base +  6]) + dot(x3, g_w[base +  7]);
            o0.z += dot(x0, g_w[base +  8]) + dot(x1, g_w[base +  9]) + dot(x2, g_w[base + 10]) + dot(x3, g_w[base + 11]);
            o0.w += dot(x0, g_w[base + 12]) + dot(x1, g_w[base + 13]) + dot(x2, g_w[base + 14]) + dot(x3, g_w[base + 15]);

            o1.x += dot(x0, g_w[base + 16]) + dot(x1, g_w[base + 17]) + dot(x2, g_w[base + 18]) + dot(x3, g_w[base + 19]);
            o1.y += dot(x0, g_w[base + 20]) + dot(x1, g_w[base + 21]) + dot(x2, g_w[base + 22]) + dot(x3, g_w[base + 23]);
            o1.z += dot(x0, g_w[base + 24]) + dot(x1, g_w[base + 25]) + dot(x2, g_w[base + 26]) + dot(x3, g_w[base + 27]);
        }
    }

    float2 dv = SafeTanh2(o0.xy) * g_params.w;
    float blend_w = SafeSigmoid(o0.z);
    float motion_conf = SafeSigmoid(o0.w);
    float3 rgb_res = SafeTanh3(o1.xyz) * g_extra.x;

    float2 v_ref = clamp(v_c + dv, -81.0, 81.0);

    Conv3Out outp;
    outp.flow_refined = float4(v_ref.x, v_ref.y, blend_w, motion_conf);
    outp.rgb_residual = float4(rgb_res.r, rgb_res.g, rgb_res.b, 1.0);
    return outp;
}
)HLSL";

// Pass 4: Full-Res Bidirectional Occlusion-Aware Warp + Optional RGB Disocclusion Residual Repair
constexpr const char* kWarpPS = R"HLSL(
cbuffer PassParams : register(b0) {
    float4 g_inv_full_half; // xy = 1 / full_size, zw = 1 / half_size (== 2 / full_size for even dims)
    float4 g_inv_qtr_eig;
    float4 g_pass_inv;
    float4 g_params;        // x = alpha, y = use_rgb_residual (1.0 in Neural, 0.0 in Flow)
    float4 g_extra;
};

Texture2D<float4> t_prev    : register(t0);
Texture2D<float4> t_curr    : register(t1);
Texture2D<float4> t_flow    : register(t2);
Texture2D<float4> t_rgb_res : register(t3);
SamplerState s_linear : register(s0);
SamplerState s_point  : register(s1);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

float4 main(VSOut pin) : SV_Target0 {
    float2 uv = pin.uv;
    float alpha = g_params.x;
    float4 flow = t_flow.SampleLevel(s_linear, uv, 0.0);

    float3 base_rgb = (alpha < 0.5)
                          ? t_prev.SampleLevel(s_point, uv, 0.0).rgb
                          : t_curr.SampleLevel(s_point, uv, 0.0).rgb;

    float motion_conf = saturate(flow.w);
    if (motion_conf <= 0.001) {
        return float4(base_rgb, 1.0);
    }

    // Half-res flow pixels -> full-res UV shift (`1 / half_size == 2 / full_size` on even resolutions).
    float2 v_uv = flow.xy * g_inv_full_half.zw;
    float blend_w = saturate(flow.z);

    float3 c0 = t_prev.SampleLevel(s_linear, uv - alpha * v_uv, 0.0).rgb;
    float3 c1 = t_curr.SampleLevel(s_linear, uv + (1.0 - alpha) * v_uv, 0.0).rgb;
    float3 occ_mix = lerp(lerp(c0, c1, alpha), lerp(c0, c1, blend_w), 0.5);

    float3 rgb_res = float3(0.0, 0.0, 0.0);
    [branch]
    if (g_params.y > 0.5) {
        rgb_res = t_rgb_res.SampleLevel(s_linear, uv, 0.0).rgb;
    }
    float3 repaired = saturate(occ_mix + rgb_res);
    float3 out_rgb = lerp(base_rgb, repaired, motion_conf);
    return float4(out_rgb, 1.0);
}
)HLSL";

bool CreateRenderableTex2D(ID3D11Device* dev,
                           uint32_t w,
                           uint32_t h,
                           DXGI_FORMAT fmt,
                           UINT bind_flags,
                           ID3D11Texture2D** out_tex,
                           ID3D11ShaderResourceView** out_srv,
                           ID3D11RenderTargetView** out_rtv) {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.SampleDesc.Quality = 0;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = bind_flags;

    HRESULT hr = dev->CreateTexture2D(&td, nullptr, out_tex);
    if (FAILED(hr) || !*out_tex) {
        return false;
    }

    if (out_srv && (bind_flags & D3D11_BIND_SHADER_RESOURCE)) {
        hr = dev->CreateShaderResourceView(*out_tex, nullptr, out_srv);
        if (FAILED(hr) || !*out_srv) {
            return false;
        }
    }

    if (out_rtv && (bind_flags & D3D11_BIND_RENDER_TARGET)) {
        hr = dev->CreateRenderTargetView(*out_tex, nullptr, out_rtv);
        if (FAILED(hr) || !*out_rtv) {
            return false;
        }
    }
    return true;
}

template <typename T>
bool CreateImmutableConstantBuffer(ID3D11Device* dev, const T& data, ID3D11Buffer** out_cb) {
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = static_cast<UINT>(sizeof(T));
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = &data;
    return SUCCEEDED(dev->CreateBuffer(&bd, &init, out_cb)) && (*out_cb != nullptr);
}

D3D11_VIEWPORT MakeViewport(uint32_t w, uint32_t h) noexcept {
    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = static_cast<float>(w);
    vp.Height = static_cast<float>(h);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    return vp;
}

}  // namespace

// ============================================================================
// D3DFrameInterpolator (Windows D3D11 + Direct2D Zero-Copy Interop)
// ============================================================================

D3DFrameInterpolator::~D3DFrameInterpolator() {
    Shutdown();
}

bool D3DFrameInterpolator::Initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
    Shutdown();
    if (!device || !context) {
        return false;
    }

    device_ = device;
    device_->AddRef();
    context_ = context;
    context_->AddRef();

    if (!CompilePipelineShaders() || !CreatePipelineStatesAndWeights()) {
        Shutdown();
        return false;
    }

    available_ = true;
    return true;
}

void D3DFrameInterpolator::Shutdown() {
    available_ = false;
    has_gpu_frame_ = false;
    has_frame_pair_ = false;
    pyr_prev_valid_ = false;
    pyr_curr_valid_ = false;
    flow_ready_ = false;
    neural_ready_ = false;
    cached_out_valid_ = false;
    cached_out_alpha_ = -1.0f;
    cached_out_mode_ = FrameGenMode::Off;
    pacer_.Reset();

    ReleaseRenderTargetInterop();
    ReleaseFrameResources();

    SafeRelease(cb_conv3_);
    SafeRelease(cb_conv2_);
    SafeRelease(cb_conv1_);
    SafeRelease(params_cb_);
    SafeRelease(depth_state_);
    SafeRelease(blend_state_);
    SafeRelease(rasterizer_state_);
    SafeRelease(point_sampler_);
    SafeRelease(linear_sampler_);

    SafeRelease(ps_warp_);
    SafeRelease(ps_neural_conv3_);
    SafeRelease(ps_neural_conv2_);
    SafeRelease(ps_neural_conv1_);
    SafeRelease(ps_flow_refine_half_);
    SafeRelease(ps_flow_refine_qtr_);
    SafeRelease(ps_flow_coarse_eig_);
    SafeRelease(ps_downsample_pyr_);
    SafeRelease(ps_downsample_half_);
    SafeRelease(vs_fullscreen_);

    SafeRelease(context_);
    SafeRelease(device_);
}

void D3DFrameInterpolator::ReleaseRenderTargetInterop() {
    SafeRelease(out_d2d_bitmap_);
    bound_d2d_rt_ = nullptr;
    cached_out_valid_ = false;
}

bool D3DFrameInterpolator::CompilePipelineShaders() {
    HMODULE compiler_mod = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!compiler_mod) {
        compiler_mod = LoadLibraryW(L"d3dcompiler_47.dll");
    }
    if (!compiler_mod) {
        return false;
    }

    const auto fn_compile =
        reinterpret_cast<PfnD3DCompile>(reinterpret_cast<void*>(GetProcAddress(compiler_mod, "D3DCompile")));
    if (!fn_compile) {
        FreeLibrary(compiler_mod);
        return false;
    }

    auto compile_blob = [&](const char* src, const char* target, ID3DBlob** out_blob) -> bool {
        ID3DBlob* err_blob = nullptr;
        HRESULT hr = fn_compile(src,
                                std::strlen(src),
                                nullptr,
                                nullptr,
                                nullptr,
                                "main",
                                target,
                                kD3DCompileOptimizationLevel3,
                                0,
                                out_blob,
                                &err_blob);
        SafeRelease(err_blob);
        return SUCCEEDED(hr) && (*out_blob != nullptr);
    };

    auto compile_ps = [&](const char* src, ID3D11PixelShader** out_ps) -> bool {
        ID3DBlob* blob = nullptr;
        if (!compile_blob(src, "ps_4_0", &blob)) {
            return false;
        }
        HRESULT hr = device_->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, out_ps);
        blob->Release();
        return SUCCEEDED(hr) && (*out_ps != nullptr);
    };

    bool ok = false;
    ID3DBlob* vs_blob = nullptr;
    if (compile_blob(kFullscreenVS, "vs_4_0", &vs_blob)) {
        HRESULT hr = device_->CreateVertexShader(
            vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &vs_fullscreen_);
        vs_blob->Release();
        if (SUCCEEDED(hr) && vs_fullscreen_) {
            ok = compile_ps(kDownsampleHalfPS, &ps_downsample_half_) &&
                 compile_ps(kDownsamplePyrPS, &ps_downsample_pyr_) &&
                 compile_ps(kFlowCoarseEighthPS, &ps_flow_coarse_eig_) &&
                 compile_ps(kFlowRefineQuarterPS, &ps_flow_refine_qtr_) &&
                 compile_ps(kFlowRefineHalfPS, &ps_flow_refine_half_) &&
                 compile_ps(kNeuralConv1PS, &ps_neural_conv1_) &&
                 compile_ps(kNeuralConv2PS, &ps_neural_conv2_) &&
                 compile_ps(kNeuralConv3PS, &ps_neural_conv3_) &&
                 compile_ps(kWarpPS, &ps_warp_);
        }
    }

    FreeLibrary(compiler_mod);
    return ok;
}

bool D3DFrameInterpolator::CreatePipelineStatesAndWeights() {
    D3D11_SAMPLER_DESC samp = {};
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samp.MinLOD = 0.0f;
    samp.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device_->CreateSamplerState(&samp, &linear_sampler_)) || !linear_sampler_) {
        return false;
    }

    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (FAILED(device_->CreateSamplerState(&samp, &point_sampler_)) || !point_sampler_) {
        return false;
    }

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(device_->CreateRasterizerState(&rd, &rasterizer_state_)) || !rasterizer_state_) {
        return false;
    }

    D3D11_BLEND_DESC bd = {};
    bd.AlphaToCoverageEnable = FALSE;
    bd.IndependentBlendEnable = FALSE;
    bd.RenderTarget[0].BlendEnable = FALSE;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device_->CreateBlendState(&bd, &blend_state_)) || !blend_state_) {
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = FALSE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    dd.StencilEnable = FALSE;
    if (FAILED(device_->CreateDepthStencilState(&dd, &depth_state_)) || !depth_state_) {
        return false;
    }

    D3D11_BUFFER_DESC pbd = {};
    pbd.ByteWidth = static_cast<UINT>(sizeof(PassParamsCBData));
    pbd.Usage = D3D11_USAGE_DEFAULT;
    pbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device_->CreateBuffer(&pbd, nullptr, &params_cb_)) || !params_cb_) {
        return false;
    }

    Conv1WeightsCBData packed_c1 = {};
    Conv2WeightsCBData packed_c2 = {};
    Conv3WeightsCBData packed_c3 = {};
#if MAC_MIRROR_HAS_NEURAL_FLOW_WEIGHTS
    PackConv1WeightsCB(neural_flow::kConv1Weight, neural_flow::kConv1Bias, &packed_c1);
    PackConv2WeightsCB(neural_flow::kConv2Weight, neural_flow::kConv2Bias, &packed_c2);
    PackConv3WeightsCB(neural_flow::kConv3Weight, neural_flow::kConv3Bias, &packed_c3);
#else
    float fallback_c1_w[16 * 8 * 9];
    float fallback_c1_b[16];
    float fallback_c2_w[16 * 16 * 9];
    float fallback_c2_b[16];
    float fallback_c3_w[7 * 16 * 9];
    float fallback_c3_b[7];
    FillFallbackNeuralWeightsV2(
        fallback_c1_w, fallback_c1_b, fallback_c2_w, fallback_c2_b, fallback_c3_w, fallback_c3_b);
    PackConv1WeightsCB(fallback_c1_w, fallback_c1_b, &packed_c1);
    PackConv2WeightsCB(fallback_c2_w, fallback_c2_b, &packed_c2);
    PackConv3WeightsCB(fallback_c3_w, fallback_c3_b, &packed_c3);
#endif

    if (!CreateImmutableConstantBuffer(device_, packed_c1, &cb_conv1_) ||
        !CreateImmutableConstantBuffer(device_, packed_c2, &cb_conv2_) ||
        !CreateImmutableConstantBuffer(device_, packed_c3, &cb_conv3_)) {
        return false;
    }

    return true;
}

void D3DFrameInterpolator::ReleaseFrameResources() {
    auto release_rt = [](RenderTarget2D& rt) noexcept {
        SafeRelease(rt.srv);
        SafeRelease(rt.rtv);
        SafeRelease(rt.tex);
    };

    release_rt(rgb_residual_);
    release_rt(flow_half_refined_);
    for (int i = 3; i >= 0; --i) {
        release_rt(hidden2_[i]);
    }
    for (int i = 3; i >= 0; --i) {
        release_rt(hidden1_[i]);
    }

    release_rt(flow_half_coarse_);
    release_rt(flow_qtr_);
    release_rt(flow_eig_);

    release_rt(pyr_curr_eig_);
    release_rt(pyr_prev_eig_);
    release_rt(pyr_curr_qtr_);
    release_rt(pyr_prev_qtr_);
    release_rt(pyr_curr_half_);
    release_rt(pyr_prev_half_);

    release_rt(out_rt_);

    SafeRelease(curr_srv_);
    SafeRelease(curr_tex_);
    SafeRelease(prev_srv_);
    SafeRelease(prev_tex_);

    tex_width_ = 0;
    tex_height_ = 0;
    half_width_ = 0;
    half_height_ = 0;
    qtr_width_ = 0;
    qtr_height_ = 0;
    eig_width_ = 0;
    eig_height_ = 0;

    has_gpu_frame_ = false;
    has_frame_pair_ = false;
    pyr_prev_valid_ = false;
    pyr_curr_valid_ = false;
    flow_ready_ = false;
    neural_ready_ = false;
    cached_out_valid_ = false;
}

bool D3DFrameInterpolator::EnsureFrameResources(uint32_t width, uint32_t height) {
    if (tex_width_ == width && tex_height_ == height &&
        prev_tex_ && curr_tex_ && out_rt_.tex &&
        pyr_prev_half_.tex && pyr_curr_half_.tex &&
        pyr_prev_qtr_.tex && pyr_curr_qtr_.tex &&
        pyr_prev_eig_.tex && pyr_curr_eig_.tex &&
        flow_eig_.tex && flow_qtr_.tex && flow_half_coarse_.tex &&
        hidden1_[0].tex && hidden2_[0].tex &&
        flow_half_refined_.tex && rgb_residual_.tex) {
        return true;
    }

    ReleaseRenderTargetInterop();
    ReleaseFrameResources();

    const uint32_t half_w = std::max<uint32_t>(4u, (width + 1u) / 2u);
    const uint32_t half_h = std::max<uint32_t>(4u, (height + 1u) / 2u);
    const uint32_t qtr_w = std::max<uint32_t>(4u, (half_w + 1u) / 2u);
    const uint32_t qtr_h = std::max<uint32_t>(4u, (half_h + 1u) / 2u);
    const uint32_t eig_w = std::max<uint32_t>(4u, (qtr_w + 1u) / 2u);
    const uint32_t eig_h = std::max<uint32_t>(4u, (qtr_h + 1u) / 2u);

    const UINT rt_srv = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    auto make_rt = [&](uint32_t w, uint32_t h, DXGI_FORMAT fmt, RenderTarget2D& rt) -> bool {
        return CreateRenderableTex2D(device_, w, h, fmt, rt_srv, &rt.tex, &rt.srv, &rt.rtv);
    };

    bool ok =
        CreateRenderableTex2D(
            device_, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, &prev_tex_, &prev_srv_, nullptr) &&
        CreateRenderableTex2D(
            device_, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, &curr_tex_, &curr_srv_, nullptr) &&
        make_rt(width, height, DXGI_FORMAT_B8G8R8A8_UNORM, out_rt_) &&
        make_rt(half_w, half_h, DXGI_FORMAT_R16G16B16A16_FLOAT, pyr_prev_half_) &&
        make_rt(half_w, half_h, DXGI_FORMAT_R16G16B16A16_FLOAT, pyr_curr_half_) &&
        make_rt(qtr_w, qtr_h, DXGI_FORMAT_R16G16B16A16_FLOAT, pyr_prev_qtr_) &&
        make_rt(qtr_w, qtr_h, DXGI_FORMAT_R16G16B16A16_FLOAT, pyr_curr_qtr_) &&
        make_rt(eig_w, eig_h, DXGI_FORMAT_R16G16B16A16_FLOAT, pyr_prev_eig_) &&
        make_rt(eig_w, eig_h, DXGI_FORMAT_R16G16B16A16_FLOAT, pyr_curr_eig_) &&
        make_rt(eig_w, eig_h, DXGI_FORMAT_R16G16B16A16_FLOAT, flow_eig_) &&
        make_rt(qtr_w, qtr_h, DXGI_FORMAT_R16G16B16A16_FLOAT, flow_qtr_) &&
        make_rt(half_w, half_h, DXGI_FORMAT_R16G16B16A16_FLOAT, flow_half_coarse_) &&
        make_rt(half_w, half_h, DXGI_FORMAT_R16G16B16A16_FLOAT, flow_half_refined_) &&
        make_rt(half_w, half_h, DXGI_FORMAT_R16G16B16A16_FLOAT, rgb_residual_);

    for (int i = 0; ok && i < 4; ++i) {
        ok = make_rt(half_w, half_h, DXGI_FORMAT_R16G16B16A16_FLOAT, hidden1_[i]) &&
             make_rt(half_w, half_h, DXGI_FORMAT_R16G16B16A16_FLOAT, hidden2_[i]);
    }

    if (!ok) {
        ReleaseFrameResources();
        return false;
    }

    tex_width_ = width;
    tex_height_ = height;
    half_width_ = half_w;
    half_height_ = half_h;
    qtr_width_ = qtr_w;
    qtr_height_ = qtr_h;
    eig_width_ = eig_w;
    eig_height_ = eig_h;
    return true;
}

bool D3DFrameInterpolator::EnsureSharedBitmap(ID2D1RenderTarget* d2d_rt) {
    if (!d2d_rt || !out_rt_.tex) {
        return false;
    }
    if (out_d2d_bitmap_ && bound_d2d_rt_ == d2d_rt) {
        return true;
    }

    ReleaseRenderTargetInterop();

    IDXGISurface* dxgi_surface = nullptr;
    HRESULT hr = out_rt_.tex->QueryInterface(__uuidof(IDXGISurface), reinterpret_cast<void**>(&dxgi_surface));
    if (FAILED(hr) || !dxgi_surface) {
        return false;
    }

    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
    hr = d2d_rt->CreateSharedBitmap(
        __uuidof(IDXGISurface), reinterpret_cast<void*>(dxgi_surface), &props, &out_d2d_bitmap_);
    if (FAILED(hr) || !out_d2d_bitmap_) {
        // Fallback to PREMULTIPLIED (identical visuals since Pass 4 always writes alpha = 1.0).
        props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        hr = d2d_rt->CreateSharedBitmap(
            __uuidof(IDXGISurface), reinterpret_cast<void*>(dxgi_surface), &props, &out_d2d_bitmap_);
    }
    dxgi_surface->Release();

    if (FAILED(hr) || !out_d2d_bitmap_) {
        out_d2d_bitmap_ = nullptr;
        return false;
    }

    bound_d2d_rt_ = d2d_rt;
    return true;
}

void D3DFrameInterpolator::UpdatePassParams(float alpha,
                                            bool use_rgb_residual,
                                            uint32_t src_w,
                                            uint32_t src_h,
                                            uint32_t dst_w,
                                            uint32_t dst_h) {
    PassParamsCBData cb = {};
    cb.inv_full_half[0] = 1.0f / static_cast<float>(std::max(1u, tex_width_));
    cb.inv_full_half[1] = 1.0f / static_cast<float>(std::max(1u, tex_height_));
    cb.inv_full_half[2] = 1.0f / static_cast<float>(std::max(1u, half_width_));
    cb.inv_full_half[3] = 1.0f / static_cast<float>(std::max(1u, half_height_));

    cb.inv_qtr_eig[0] = 1.0f / static_cast<float>(std::max(1u, qtr_width_));
    cb.inv_qtr_eig[1] = 1.0f / static_cast<float>(std::max(1u, qtr_height_));
    cb.inv_qtr_eig[2] = 1.0f / static_cast<float>(std::max(1u, eig_width_));
    cb.inv_qtr_eig[3] = 1.0f / static_cast<float>(std::max(1u, eig_height_));

    cb.pass_inv[0] = 1.0f / static_cast<float>(std::max(1u, src_w));
    cb.pass_inv[1] = 1.0f / static_cast<float>(std::max(1u, src_h));
    cb.pass_inv[2] = 1.0f / static_cast<float>(std::max(1u, dst_w));
    cb.pass_inv[3] = 1.0f / static_cast<float>(std::max(1u, dst_h));

    cb.params[0] = alpha;
    cb.params[1] = use_rgb_residual ? 1.0f : 0.0f;
    cb.params[2] = 1.0f / kFlowNormScale;
    cb.params[3] = kResidualFlowScale;

    cb.extra[0] = kRgbResidualScale;
    cb.extra[1] = static_cast<float>(tex_width_);
    cb.extra[2] = static_cast<float>(tex_height_);
    cb.extra[3] = 0.0f;

    context_->UpdateSubresource(params_cb_, 0, nullptr, &cb, 0, 0);
}

bool D3DFrameInterpolator::PushFramePair(ID2D1RenderTarget* d2d_rt,
                                         const uint8_t* curr_bgra,
                                         uint32_t width,
                                         uint32_t height,
                                         uint32_t stride_bytes,
                                         const std::vector<Rect>& dirty_rects,
                                         const Rect& dirty_bounds,
                                         uint64_t now_ms) {
    if (!available_ || !device_ || !context_ || !curr_bgra ||
        width < 16u || height < 16u || stride_bytes < width * 4u) {
        return false;
    }

    if (!EnsureFrameResources(width, height)) {
        ResetPacer();
        return false;
    }

    // First frame after creation or dimension change: seed GPU ping-pong pair cleanly.
    if (!has_gpu_frame_) {
        context_->UpdateSubresource(curr_tex_, 0, nullptr, curr_bgra, stride_bytes, 0);
        context_->CopyResource(prev_tex_, curr_tex_);
        if (d2d_rt) {
            EnsureSharedBitmap(d2d_rt);
        }
        has_gpu_frame_ = true;
        has_frame_pair_ = false;
        pyr_prev_valid_ = false;
        pyr_curr_valid_ = false;
        flow_ready_ = false;
        neural_ready_ = false;
        cached_out_valid_ = false;
        return true;
    }

    uint64_t dirty_area_sum = 0;
    bool has_valid_dirty_rect = false;
    for (const Rect& r : dirty_rects) {
        if (r.w == 0u || r.h == 0u || r.x >= width || r.y >= height) {
            continue;
        }
        const uint32_t right = std::min<uint32_t>(width, static_cast<uint32_t>(r.x) + r.w);
        const uint32_t bottom = std::min<uint32_t>(height, static_cast<uint32_t>(r.y) + r.h);
        if (right <= r.x || bottom <= r.y) {
            continue;
        }
        has_valid_dirty_rect = true;
        dirty_area_sum += static_cast<uint64_t>(right - r.x) * static_cast<uint64_t>(bottom - r.y);
    }

    auto upload_dirty_rects = [&](ID3D11Texture2D* dst_tex) {
        for (const Rect& r : dirty_rects) {
            if (r.w == 0u || r.h == 0u || r.x >= width || r.y >= height) {
                continue;
            }
            const uint32_t right = std::min<uint32_t>(width, static_cast<uint32_t>(r.x) + r.w);
            const uint32_t bottom = std::min<uint32_t>(height, static_cast<uint32_t>(r.y) + r.h);
            if (right <= r.x || bottom <= r.y) {
                continue;
            }
            D3D11_BOX box = {};
            box.left = r.x;
            box.top = r.y;
            box.front = 0u;
            box.right = right;
            box.bottom = bottom;
            box.back = 1u;
            const uint8_t* src_ptr =
                curr_bgra + static_cast<size_t>(r.y) * stride_bytes + static_cast<size_t>(r.x) * 4u;
            context_->UpdateSubresource(dst_tex, 0, &box, src_ptr, stride_bytes, 0);
        }
    };

    // Optimization 2: Skip FrameGen on tiny dirty-rect updates (cursor blink, clock tick, single-char typing).
    // Update `curr_tex_` IN PLACE so GPU textures stay 100% synced without swapping `prev_tex_` or triggering GPU passes.
    if (FrameGenPacer::ShouldSkipFrameGenForDirtyRegion(width, height, dirty_bounds.w, dirty_bounds.h)) {
        bool updated_curr = false;
        if (has_valid_dirty_rect) {
            upload_dirty_rects(curr_tex_);
            updated_curr = true;
        } else if (dirty_bounds.w > 0u && dirty_bounds.h > 0u &&
                   dirty_bounds.x < width && dirty_bounds.y < height) {
            const uint32_t right = std::min<uint32_t>(width, static_cast<uint32_t>(dirty_bounds.x) + dirty_bounds.w);
            const uint32_t bottom = std::min<uint32_t>(height, static_cast<uint32_t>(dirty_bounds.y) + dirty_bounds.h);
            if (right > dirty_bounds.x && bottom > dirty_bounds.y) {
                D3D11_BOX box = {};
                box.left = dirty_bounds.x;
                box.top = dirty_bounds.y;
                box.front = 0u;
                box.right = right;
                box.bottom = bottom;
                box.back = 1u;
                const uint8_t* src_ptr = curr_bgra + static_cast<size_t>(dirty_bounds.y) * stride_bytes +
                                         static_cast<size_t>(dirty_bounds.x) * 4u;
                context_->UpdateSubresource(curr_tex_, 0, &box, src_ptr, stride_bytes, 0);
                updated_curr = true;
            }
        }

        if (updated_curr) {
            pyr_curr_valid_ = false;
            cached_out_valid_ = false;
        }
        if (!pacer_.IsAnimating(now_ms, FrameGenMode::Neural)) {
            pacer_.Reset();
            has_frame_pair_ = false;
            flow_ready_ = false;
            neural_ready_ = false;
        }
        if (d2d_rt) {
            EnsureSharedBitmap(d2d_rt);
        }
        return true;
    }

    // Optimization 1: GPU Ping-Pong swap (`prev_tex_ <-> curr_tex_` and `pyr_prev_* <-> pyr_curr_*`).
    // `prev_tex_` now holds frame N-1 in VRAM with 0 bytes uploaded, and `pyr_prev_*` is preserved if valid!
    std::swap(prev_tex_, curr_tex_);
    std::swap(prev_srv_, curr_srv_);
    std::swap(pyr_prev_half_, pyr_curr_half_);
    std::swap(pyr_prev_qtr_, pyr_curr_qtr_);
    std::swap(pyr_prev_eig_, pyr_curr_eig_);

    pyr_prev_valid_ = pyr_curr_valid_;
    pyr_curr_valid_ = false;
    flow_ready_ = false;
    neural_ready_ = false;
    cached_out_valid_ = false;

    const uint64_t total_area = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    if (has_valid_dirty_rect && (dirty_area_sum * 100u < total_area * 65u)) {
        // Fast VRAM-to-VRAM copy preserves untouched pixels from frame N-1, then upload only dirty rects.
        context_->CopyResource(curr_tex_, prev_tex_);
        upload_dirty_rects(curr_tex_);
    } else {
        context_->UpdateSubresource(curr_tex_, 0, nullptr, curr_bgra, stride_bytes, 0);
    }

    if (d2d_rt) {
        EnsureSharedBitmap(d2d_rt);
    }

    has_frame_pair_ = true;
    pacer_.OnNewFrame(now_ms);
    return true;
}

ID2D1Bitmap* D3DFrameInterpolator::SynthesizeBeforeD2D(ID2D1RenderTarget* d2d_rt,
                                                       FrameGenMode mode,
                                                       uint64_t now_ms) {
    if (!available_ || !has_frame_pair_ || !has_gpu_frame_ || !d2d_rt || mode == FrameGenMode::Off) {
        return nullptr;
    }

    float alpha = 1.0f;
    if (!pacer_.SampleAlpha(now_ms, mode, &alpha)) {
        return nullptr;
    }

    if (!EnsureSharedBitmap(d2d_rt) || !out_d2d_bitmap_) {
        return nullptr;
    }

    // Optimization 3a: Fast zero-pass cursor redraw path (`WM_MOUSEMOVE` between display ticks).
    if (FrameGenPacer::ShouldReuseCachedSynthesis(
            cached_out_valid_, alpha, cached_out_alpha_, mode, cached_out_mode_) &&
        out_d2d_bitmap_ != nullptr) {
        return out_d2d_bitmap_;
    }

    ScopedD3D11State saved_state(context_);

    const bool use_neural = (mode == FrameGenMode::Neural);
    const FLOAT blend_factor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vs_fullscreen_, nullptr, 0);
    context_->RSSetState(rasterizer_state_);
    context_->OMSetBlendState(blend_state_, blend_factor, 0xFFFFFFFFu);
    context_->OMSetDepthStencilState(depth_state_, 0);

    ID3D11SamplerState* samplers[2] = {linear_sampler_, point_sampler_};
    context_->PSSetSamplers(0, 2, samplers);
    context_->PSSetConstantBuffers(0, 1, &params_cb_);

    ID3D11ShaderResourceView* null_srvs[8] = {};
    ID3D11RenderTargetView* null_rtvs[4] = {};
    auto unbind_all = [&]() {
        context_->PSSetShaderResources(0, 8, null_srvs);
        context_->OMSetRenderTargets(4, null_rtvs, nullptr);
    };

    const D3D11_VIEWPORT full_vp = MakeViewport(tex_width_, tex_height_);
    const D3D11_VIEWPORT half_vp = MakeViewport(half_width_, half_height_);
    const D3D11_VIEWPORT qtr_vp = MakeViewport(qtr_width_, qtr_height_);
    const D3D11_VIEWPORT eig_vp = MakeViewport(eig_width_, eig_height_);

    unbind_all();

    auto build_pyramid = [&](ID3D11ShaderResourceView* src_srv,
                             RenderTarget2D& half_rt,
                             RenderTarget2D& qtr_rt,
                             RenderTarget2D& eig_rt) {
        // Pass 1a: Full -> Half (1/2)
        UpdatePassParams(0.5f, use_neural, tex_width_, tex_height_, half_width_, half_height_);
        context_->RSSetViewports(1, &half_vp);
        context_->PSSetShader(ps_downsample_half_, nullptr, 0);
        context_->OMSetRenderTargets(1, &half_rt.rtv, nullptr);
        context_->PSSetShaderResources(0, 1, &src_srv);
        context_->Draw(3, 0);
        unbind_all();

        // Pass 1b (Level 1): Half -> Quarter (1/4)
        UpdatePassParams(0.5f, use_neural, half_width_, half_height_, qtr_width_, qtr_height_);
        context_->RSSetViewports(1, &qtr_vp);
        context_->PSSetShader(ps_downsample_pyr_, nullptr, 0);
        context_->OMSetRenderTargets(1, &qtr_rt.rtv, nullptr);
        context_->PSSetShaderResources(0, 1, &half_rt.srv);
        context_->Draw(3, 0);
        unbind_all();

        // Pass 1b (Level 2): Quarter -> Eighth (1/8)
        UpdatePassParams(0.5f, use_neural, qtr_width_, qtr_height_, eig_width_, eig_height_);
        context_->RSSetViewports(1, &eig_vp);
        context_->PSSetShader(ps_downsample_pyr_, nullptr, 0);
        context_->OMSetRenderTargets(1, &eig_rt.rtv, nullptr);
        context_->PSSetShaderResources(0, 1, &qtr_rt.srv);
        context_->Draw(3, 0);
        unbind_all();
    };

    // Pass 1 (3-Level Pyramid): Build `pyr_prev_*` ONLY if not already valid via ping-pong swap!
    if (!pyr_prev_valid_) {
        build_pyramid(prev_srv_, pyr_prev_half_, pyr_prev_qtr_, pyr_prev_eig_);
        pyr_prev_valid_ = true;
    }
    if (!pyr_curr_valid_) {
        build_pyramid(curr_srv_, pyr_curr_half_, pyr_curr_qtr_, pyr_curr_eig_);
        pyr_curr_valid_ = true;
    }

    // Pass 2 (Coarse-to-Fine 1/8 -> 1/4 -> 1/2 Flow): Cached ONCE per RFB frame pair!
    if (!flow_ready_) {
        UpdatePassParams(0.5f, use_neural, qtr_width_, qtr_height_, eig_width_, eig_height_);

        // Pass 2a: Coarse Optical Flow at 1/8 (+-16 eig-px = +-128 full-res px)
        context_->RSSetViewports(1, &eig_vp);
        context_->OMSetRenderTargets(1, &flow_eig_.rtv, nullptr);
        context_->PSSetShader(ps_flow_coarse_eig_, nullptr, 0);
        ID3D11ShaderResourceView* p2a_srvs[2] = {pyr_prev_eig_.srv, pyr_curr_eig_.srv};
        context_->PSSetShaderResources(0, 2, p2a_srvs);
        context_->Draw(3, 0);
        unbind_all();

        // Pass 2b: Flow Refinement at 1/4 (+-4 qtr-px around 2x upsampled 1/8 prior)
        context_->RSSetViewports(1, &qtr_vp);
        context_->OMSetRenderTargets(1, &flow_qtr_.rtv, nullptr);
        context_->PSSetShader(ps_flow_refine_qtr_, nullptr, 0);
        ID3D11ShaderResourceView* p2b_srvs[3] = {pyr_prev_qtr_.srv, pyr_curr_qtr_.srv, flow_eig_.srv};
        context_->PSSetShaderResources(0, 3, p2b_srvs);
        context_->Draw(3, 0);
        unbind_all();

        // Pass 2c: Flow Refinement at 1/2 (+-3 half-px + 0.5 subpixel -> +-150 full-res px total)
        context_->RSSetViewports(1, &half_vp);
        context_->OMSetRenderTargets(1, &flow_half_coarse_.rtv, nullptr);
        context_->PSSetShader(ps_flow_refine_half_, nullptr, 0);
        ID3D11ShaderResourceView* p2c_srvs[3] = {pyr_prev_half_.srv, pyr_curr_half_.srv, flow_qtr_.srv};
        context_->PSSetShaderResources(0, 3, p2c_srvs);
        context_->Draw(3, 0);
        unbind_all();

        flow_ready_ = true;
    }

    // Optimization 3b: Run 3-Layer 8 -> 16 -> 16 -> 7 Neural Residual CNN ONCE per RFB frame pair
    // at canonical `alpha = 0.5f` (preserving full v2 refined flow, occlusion, text protection & RGB repair).
    if (use_neural && !neural_ready_) {
        UpdatePassParams(0.5f, true, half_width_, half_height_, tex_width_, tex_height_);
        context_->RSSetViewports(1, &half_vp);

        // Pass 3a: Conv1 (8 -> 16 + ReLU) -> 4 MRTs (`hidden1_[0..3]`)
        ID3D11RenderTargetView* h1_rtvs[4] = {
            hidden1_[0].rtv, hidden1_[1].rtv, hidden1_[2].rtv, hidden1_[3].rtv};
        context_->OMSetRenderTargets(4, h1_rtvs, nullptr);
        context_->PSSetConstantBuffers(1, 1, &cb_conv1_);
        context_->PSSetShader(ps_neural_conv1_, nullptr, 0);
        ID3D11ShaderResourceView* c1_srvs[3] = {
            pyr_prev_half_.srv, pyr_curr_half_.srv, flow_half_coarse_.srv};
        context_->PSSetShaderResources(0, 3, c1_srvs);
        context_->Draw(3, 0);
        unbind_all();

        // Pass 3b: Conv2 (16 -> 16 + skip + ReLU) -> 4 MRTs (`hidden2_[0..3]`)
        ID3D11RenderTargetView* h2_rtvs[4] = {
            hidden2_[0].rtv, hidden2_[1].rtv, hidden2_[2].rtv, hidden2_[3].rtv};
        context_->OMSetRenderTargets(4, h2_rtvs, nullptr);
        context_->PSSetConstantBuffers(1, 1, &cb_conv2_);
        context_->PSSetShader(ps_neural_conv2_, nullptr, 0);
        ID3D11ShaderResourceView* c2_srvs[4] = {
            hidden1_[0].srv, hidden1_[1].srv, hidden1_[2].srv, hidden1_[3].srv};
        context_->PSSetShaderResources(0, 4, c2_srvs);
        context_->Draw(3, 0);
        unbind_all();

        // Pass 3c: Conv3 (16 -> 7) -> 2 MRTs (`flow_half_refined_`, `rgb_residual_`)
        ID3D11RenderTargetView* c3_rtvs[2] = {flow_half_refined_.rtv, rgb_residual_.rtv};
        context_->OMSetRenderTargets(2, c3_rtvs, nullptr);
        context_->PSSetConstantBuffers(1, 1, &cb_conv3_);
        context_->PSSetShader(ps_neural_conv3_, nullptr, 0);
        ID3D11ShaderResourceView* c3_srvs[7] = {
            hidden2_[0].srv,
            hidden2_[1].srv,
            hidden2_[2].srv,
            hidden2_[3].srv,
            flow_half_coarse_.srv,
            pyr_prev_half_.srv,
            pyr_curr_half_.srv};
        context_->PSSetShaderResources(0, 7, c3_srvs);
        context_->Draw(3, 0);
        unbind_all();

        neural_ready_ = true;
    }

    // Pass 4: Full-Resolution Bidirectional Occlusion-Aware Warp + RGB Disocclusion Repair -> `out_rt_`
    // Runs at exact current `alpha` using cached refined/coarse flow + cached RGB residual!
    UpdatePassParams(alpha, use_neural, half_width_, half_height_, tex_width_, tex_height_);
    ID3D11ShaderResourceView* active_flow_srv =
        use_neural ? flow_half_refined_.srv : flow_half_coarse_.srv;
    context_->RSSetViewports(1, &full_vp);
    context_->OMSetRenderTargets(1, &out_rt_.rtv, nullptr);
    context_->PSSetShader(ps_warp_, nullptr, 0);
    ID3D11ShaderResourceView* warp_srvs[4] = {
        prev_srv_,
        curr_srv_,
        active_flow_srv,
        use_neural ? rgb_residual_.srv : nullptr};
    context_->PSSetShaderResources(0, 4, warp_srvs);
    context_->Draw(3, 0);
    unbind_all();

    cached_out_valid_ = true;
    cached_out_alpha_ = alpha;
    cached_out_mode_ = mode;

    // `saved_state` destructor restores original D3D11 state cleanly before Direct2D `BeginDraw()` reads `out_rt_.tex`.
    return out_d2d_bitmap_;
}

#endif  // _WIN32

}  // namespace mac_mirror
