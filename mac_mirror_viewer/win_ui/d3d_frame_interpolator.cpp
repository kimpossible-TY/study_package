#include "d3d_frame_interpolator.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef _WIN32
#include <dxgi.h>
#include <d3dcommon.h>
#if __has_include("neural_flow_weights.h")
#include "neural_flow_weights.h"
#define MAC_MIRROR_HAS_NEURAL_FLOW_WEIGHTS 1
#else
#define MAC_MIRROR_HAS_NEURAL_FLOW_WEIGHTS 0
#endif
#endif

namespace mac_mirror {

// ============================================================================
// FrameGenPacer (cross-platform pure C++17)
// ============================================================================

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

// Dynamically resolved D3DCompile signature from system32\d3dcompiler_47.dll (zero PE DLL imports!).
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

struct alignas(16) PassParamsCBData {
    float inv_sizes[4];  // xy = 1/full_size, zw = 1/quarter_size
    float params[4];     // x = alpha, y = full_w, z = full_h, w = reserved
};

// 219 float4 vectors = 3,504 bytes (matches `cbuffer NeuralWeights : register(b1)`).
struct alignas(16) NeuralWeightsCBData {
    float conv1_w[144][4];
    float conv1_b[2][4];
    float conv2_w[72][4];
    float conv2_b[1][4];
};

#if !MAC_MIRROR_HAS_NEURAL_FLOW_WEIGHTS
// Standalone fallback weights if compiled before `neural_flow_weights.h` is merged:
// Preserves coarse flow (`dv = 0`), balanced occlusion (`blend_w = 0.5`), and crisp static gating.
void FillFallbackNeuralWeights(float (&conv1_w)[8 * 8 * 9],
                               float (&conv1_b)[8],
                               float (&conv2_w)[4 * 8 * 9],
                               float (&conv2_b)[4]) noexcept {
    std::memset(conv1_w, 0, sizeof(conv1_w));
    std::memset(conv1_b, 0, sizeof(conv1_b));
    std::memset(conv2_w, 0, sizeof(conv2_w));
    std::memset(conv2_b, 0, sizeof(conv2_b));

    // Center tap index within 3x3 kernel: ky=1, kx=1 -> 4
    constexpr int kCenter = 4;
    // Hidden 0: motion_prior (ch7) at center
    conv1_w[0 * 72 + 7 * 9 + kCenter] = 1.0f;
    // Hidden 1..4: |vx_c/12| and |vy_c/12| via +/- ReLU pairs
    conv1_w[1 * 72 + 4 * 9 + kCenter] = 1.0f;
    conv1_w[2 * 72 + 4 * 9 + kCenter] = -1.0f;
    conv1_w[3 * 72 + 5 * 9 + kCenter] = 1.0f;
    conv1_w[4 * 72 + 5 * 9 + kCenter] = -1.0f;

    // Output 3 (motion_conf logit): negative on static pixels, positive on moving pixels
    conv2_b[3] = -3.5f;
    conv2_w[3 * 72 + 0 * 9 + kCenter] = 6.5f;
    conv2_w[3 * 72 + 1 * 9 + kCenter] = 5.0f;
    conv2_w[3 * 72 + 2 * 9 + kCenter] = 5.0f;
    conv2_w[3 * 72 + 3 * 9 + kCenter] = 5.0f;
    conv2_w[3 * 72 + 4 * 9 + kCenter] = 5.0f;
}
#endif

void PackNeuralWeightsCB(const float* c1_w,
                         const float* c1_b,
                         const float* c2_w,
                         const float* c2_b,
                         NeuralWeightsCBData* out) noexcept {
    std::memset(out, 0, sizeof(*out));

    // Source layout from `neural_flow_weights.h`: [oc * (ic_count * 9) + ic * 9 + ky * 3 + kx].
    // GPU layout: spatial-major [s = ky * 3 + kx] so unrolled 3x3 shader taps use immediate CB offsets.
    for (int ky = 0; ky < 3; ++ky) {
        for (int kx = 0; kx < 3; ++kx) {
            const int s = ky * 3 + kx;
            for (int oc = 0; oc < 8; ++oc) {
                const int dst = (s * 8 + oc) * 2;
                for (int k = 0; k < 4; ++k) {
                    out->conv1_w[dst + 0][k] = c1_w[oc * 72 + (k + 0) * 9 + s];
                    out->conv1_w[dst + 1][k] = c1_w[oc * 72 + (k + 4) * 9 + s];
                }
            }
            for (int oc = 0; oc < 4; ++oc) {
                const int dst = (s * 4 + oc) * 2;
                for (int k = 0; k < 4; ++k) {
                    out->conv2_w[dst + 0][k] = c2_w[oc * 72 + (k + 0) * 9 + s];
                    out->conv2_w[dst + 1][k] = c2_w[oc * 72 + (k + 4) * 9 + s];
                }
            }
        }
    }

    for (int k = 0; k < 4; ++k) {
        out->conv1_b[0][k] = c1_b[k];
        out->conv1_b[1][k] = c1_b[k + 4];
        out->conv2_b[0][k] = c2_b[k];
    }
}

// Saves & restores immediate D3D11DeviceContext pipeline bindings around our offscreen passes,
// and strictly unbinds all custom SRVs/RTVs before `d2d_rt->BeginDraw()` runs on the shared context.
class ScopedD3D11State {
public:
    explicit ScopedD3D11State(ID3D11DeviceContext* ctx) noexcept : ctx_(ctx) {
        if (!ctx_) return;
        ctx_->OMGetRenderTargets(2, rtvs_, &dsv_);
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
        ctx_->PSGetShaderResources(0, 5, ps_srvs_);
    }

    ~ScopedD3D11State() {
        if (!ctx_) return;

        ID3D11ShaderResourceView* null_srvs[5] = {};
        ctx_->PSSetShaderResources(0, 5, null_srvs);
        ID3D11RenderTargetView* null_rtvs[2] = {};
        ctx_->OMSetRenderTargets(2, null_rtvs, nullptr);

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
        ctx_->PSSetShaderResources(0, 5, ps_srvs_);
        ctx_->OMSetRenderTargets(2, rtvs_, dsv_);

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
    ID3D11RenderTargetView* rtvs_[2] = {};
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
    ID3D11ShaderResourceView* ps_srvs_[5] = {};
};

// ----------------------------------------------------------------------------
// Embedded SM4.0 (`vs_4_0` / `ps_4_0`) HLSL Shaders
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

// Pass 1: Quarter-Res Feature Pyramid -> (luma, dx, dy, grad_mag)
constexpr const char* kPyramidPS = R"HLSL(
cbuffer PassParams : register(b0) {
    float4 g_inv_sizes; // xy = 1 / full_size, zw = 1 / quarter_size
    float4 g_params;    // x = alpha
};

Texture2D<float4> t_src : register(t0);
SamplerState s_linear : register(s0);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

float SampleBox4x4Luma(float2 center_uv) {
    float2 f = g_inv_sizes.xy;
    float3 c0 = t_src.SampleLevel(s_linear, center_uv + float2(-f.x, -f.y), 0.0).rgb;
    float3 c1 = t_src.SampleLevel(s_linear, center_uv + float2( f.x, -f.y), 0.0).rgb;
    float3 c2 = t_src.SampleLevel(s_linear, center_uv + float2(-f.x,  f.y), 0.0).rgb;
    float3 c3 = t_src.SampleLevel(s_linear, center_uv + float2( f.x,  f.y), 0.0).rgb;
    float3 avg_rgb = (c0 + c1 + c2 + c3) * 0.25;
    return dot(avg_rgb, float3(0.2126, 0.7152, 0.0722));
}

float4 main(VSOut pin) : SV_Target0 {
    float2 uv = pin.uv;
    float2 q = g_inv_sizes.zw;
    float l_c = SampleBox4x4Luma(uv);
    float l_l = SampleBox4x4Luma(uv - float2(q.x, 0.0));
    float l_r = SampleBox4x4Luma(uv + float2(q.x, 0.0));
    float l_u = SampleBox4x4Luma(uv - float2(0.0, q.y));
    float l_d = SampleBox4x4Luma(uv + float2(0.0, q.y));
    float dx = 0.5 * (l_r - l_l);
    float dy = 0.5 * (l_d - l_u);
    float grad_mag = saturate(sqrt(dx * dx + dy * dy) * 4.0);
    return float4(l_c, dx, dy, grad_mag);
}
)HLSL";

// Pass 2: Hierarchical Diamond/Grid Block-Matching Optical Flow (+-12 quarter-px = +-48 full-res px)
constexpr const char* kCoarseFlowPS = R"HLSL(
cbuffer PassParams : register(b0) {
    float4 g_inv_sizes; // xy = 1 / full_size, zw = 1 / quarter_size
    float4 g_params;    // x = alpha
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
    float2 q = g_inv_sizes.zw;
    float2 half_duv = 0.5 * v * q;
    float2 uv0 = uv - half_duv;
    float2 uv1 = uv + half_duv;
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
    return cost * (1.0 / 5.5) + 0.0012 * length(v);
}

float TempDiffAt(float2 p_uv) {
    return abs(t_feat1.SampleLevel(s_linear, p_uv, 0.0).x -
               t_feat0.SampleLevel(s_linear, p_uv, 0.0).x);
}

float4 main(VSOut pin) : SV_Target0 {
    float2 uv = pin.uv;
    float2 q = g_inv_sizes.zw;
    float4 f0_c = t_feat0.SampleLevel(s_linear, uv, 0.0);
    float4 f1_c = t_feat1.SampleLevel(s_linear, uv, 0.0);

    float diff_c = abs(f1_c.x - f0_c.x);
    float max_nb = diff_c;
    max_nb = max(max_nb, max(TempDiffAt(uv - float2(2.0 * q.x, 0.0)), TempDiffAt(uv + float2(2.0 * q.x, 0.0))));
    max_nb = max(max_nb, max(TempDiffAt(uv - float2(0.0, 2.0 * q.y)), TempDiffAt(uv + float2(0.0, 2.0 * q.y))));
    max_nb = max(max_nb, max(TempDiffAt(uv - float2(5.0 * q.x, 0.0)), TempDiffAt(uv + float2(5.0 * q.x, 0.0))));
    max_nb = max(max_nb, max(TempDiffAt(uv - float2(0.0, 5.0 * q.y)), TempDiffAt(uv + float2(0.0, 5.0 * q.y))));
    if (max_nb < 0.010) {
        return float4(0.0, 0.0, 0.5, 0.0);
    }

    // Stage 1: Coarse 5x5 grid over [-12, +12] in steps of 6 quarter-px
    float2 best_v = float2(0.0, 0.0);
    float best_cost = EvalCandidate(uv, best_v, 2.0);
    [loop]
    for (int gy = -2; gy <= 2; ++gy) {
        [loop]
        for (int gx = -2; gx <= 2; ++gx) {
            if (gx != 0 || gy != 0) {
                float2 cand = float2((float)gx * 6.0, (float)gy * 6.0);
                float c = EvalCandidate(uv, cand, 2.0);
                if (c < best_cost) {
                    best_cost = c;
                    best_v = cand;
                }
            }
        }
    }

    // Stage 2: Medium 3x3 grid around Stage 1 winner (step = 3.0 quarter-px)
    float2 center2 = best_v;
    [loop]
    for (int my = -1; my <= 1; ++my) {
        [loop]
        for (int mx = -1; mx <= 1; ++mx) {
            if (mx != 0 || my != 0) {
                float2 cand = clamp(center2 + float2((float)mx * 3.0, (float)my * 3.0), -12.0, 12.0);
                float c = EvalCandidate(uv, cand, 2.0);
                if (c < best_cost) {
                    best_cost = c;
                    best_v = cand;
                }
            }
        }
    }

    // Stage 3: Fine 3x3 grid around Stage 2 winner (step = 1.0 quarter-px)
    float2 center3 = best_v;
    float best_fine = EvalCandidate(uv, center3, 1.0);
    [loop]
    for (int fy = -1; fy <= 1; ++fy) {
        [loop]
        for (int fx = -1; fx <= 1; ++fx) {
            if (fx != 0 || fy != 0) {
                float2 cand = clamp(center3 + float2((float)fx, (float)fy), -12.0, 12.0);
                float c = EvalCandidate(uv, cand, 1.0);
                if (c < best_fine) {
                    best_fine = c;
                    best_v = cand;
                }
            }
        }
    }

    // Stage 4: Sub-quarter-pixel diamond refinement (step = 0.5 quarter-px = 2 full-res px)
    float2 center4 = best_v;
    static const float2 kDiamond[4] = {
        float2(-0.5, 0.0), float2(0.5, 0.0), float2(0.0, -0.5), float2(0.0, 0.5)
    };
    [unroll]
    for (int di = 0; di < 4; ++di) {
        float2 cand = clamp(center4 + kDiamond[di], -12.0, 12.0);
        float c = EvalCandidate(uv, cand, 1.0);
        if (c < best_fine) {
            best_fine = c;
            best_v = cand;
        }
    }

    // Forward/backward error asymmetry for Stage Manager window occlusion
    float4 f0_bwd = t_feat0.SampleLevel(s_linear, uv - best_v * q, 0.0);
    float4 f1_fwd = t_feat1.SampleLevel(s_linear, uv + best_v * q, 0.0);
    float err_from_0 = FeatureDiff(f0_c, f1_fwd);
    float err_from_1 = FeatureDiff(f1_c, f0_bwd);
    float blend_w = clamp(0.5 + (err_from_0 - err_from_1) * 1.5, 0.15, 0.85);

    // Also detect objects cleanly crossing midpoint uv when both endpoints happened to be background,
    // while strictly keeping motion_conf == 0.0 on any static pixel (where best_v == 0).
    float f0_mid = t_feat0.SampleLevel(s_linear, uv - 0.5 * best_v * q, 0.0).x;
    float f1_mid = t_feat1.SampleLevel(s_linear, uv + 0.5 * best_v * q, 0.0).x;
    float cross_diff = (best_fine < 0.045) ? min(abs(f0_mid - f0_c.x), abs(f1_mid - f1_c.x)) : 0.0;
    float effective_diff = max(diff_c, cross_diff);
    float motion_conf = saturate((effective_diff - 0.012) * 20.0);
    if (length(best_v) < 0.20) {
        best_v = float2(0.0, 0.0);
        motion_conf = 0.0;
    }

    return float4(best_v.x, best_v.y, blend_w, motion_conf);
}
)HLSL";

// Pass 3a: Neural 8->8 3x3 Conv1 + ReLU (MRT writing 8 hidden channels into two float4 textures)
constexpr const char* kNeuralConv1PS = R"HLSL(
cbuffer PassParams : register(b0) {
    float4 g_inv_sizes; // xy = 1 / full_size, zw = 1 / quarter_size
    float4 g_params;    // x = alpha
};

cbuffer NeuralWeights : register(b1) {
    float4 g_conv1_w[144];
    float4 g_conv1_b[2];
    float4 g_conv2_w[72];
    float4 g_conv2_b[1];
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

struct Conv1Out {
    float4 h0 : SV_Target0;
    float4 h1 : SV_Target1;
};

void GatherChannels8(float2 q_uv, out float4 ch0_3, out float4 ch4_7) {
    float2 q = g_inv_sizes.zw;
    float alpha = g_params.x;
    float4 f0 = t_feat0.SampleLevel(s_point, q_uv, 0.0);
    float4 f1 = t_feat1.SampleLevel(s_point, q_uv, 0.0);
    float4 fc = t_flow_coarse.SampleLevel(s_point, q_uv, 0.0);

    float i0 = f0.x;
    float i1 = f1.x;
    float2 v_c = fc.xy;
    float w0 = t_feat0.SampleLevel(s_linear, q_uv - alpha * v_c * q, 0.0).x;
    float w1 = t_feat1.SampleLevel(s_linear, q_uv + (1.0 - alpha) * v_c * q, 0.0).x;
    float warped_diff = clamp(w1 - w0, -1.0, 1.0);
    float grad_mag = saturate(f0.w);
    float vx_norm = clamp(v_c.x * (1.0 / 12.0), -1.0, 1.0);
    float vy_norm = clamp(v_c.y * (1.0 / 12.0), -1.0, 1.0);
    float motion_prior = saturate(abs(i1 - i0) * 6.0);

    ch0_3 = float4(i0, i1, warped_diff, grad_mag);
    ch4_7 = float4(vx_norm, vy_norm, alpha, motion_prior);
}

Conv1Out main(VSOut pin) {
    float2 uv = pin.uv;
    float2 q = g_inv_sizes.zw;
    float4 h0 = g_conv1_b[0];
    float4 h1 = g_conv1_b[1];

    [unroll]
    for (int ky = 0; ky < 3; ++ky) {
        [unroll]
        for (int kx = 0; kx < 3; ++kx) {
            int s = ky * 3 + kx;
            int base = s * 16;
            float2 sample_uv = uv + float2((float)(kx - 1) * q.x, (float)(ky - 1) * q.y);
            float4 x0;
            float4 x1;
            GatherChannels8(sample_uv, x0, x1);

            h0.x += dot(x0, g_conv1_w[base +  0]) + dot(x1, g_conv1_w[base +  1]);
            h0.y += dot(x0, g_conv1_w[base +  2]) + dot(x1, g_conv1_w[base +  3]);
            h0.z += dot(x0, g_conv1_w[base +  4]) + dot(x1, g_conv1_w[base +  5]);
            h0.w += dot(x0, g_conv1_w[base +  6]) + dot(x1, g_conv1_w[base +  7]);
            h1.x += dot(x0, g_conv1_w[base +  8]) + dot(x1, g_conv1_w[base +  9]);
            h1.y += dot(x0, g_conv1_w[base + 10]) + dot(x1, g_conv1_w[base + 11]);
            h1.z += dot(x0, g_conv1_w[base + 12]) + dot(x1, g_conv1_w[base + 13]);
            h1.w += dot(x0, g_conv1_w[base + 14]) + dot(x1, g_conv1_w[base + 15]);
        }
    }

    Conv1Out outp;
    outp.h0 = max(h0, 0.0);
    outp.h1 = max(h1, 0.0);
    return outp;
}
)HLSL";

// Pass 3b: Neural 8->4 3x3 Conv2 + Residual Flow / Occlusion / Static-Crispness Gate
constexpr const char* kNeuralConv2PS = R"HLSL(
cbuffer PassParams : register(b0) {
    float4 g_inv_sizes; // xy = 1 / full_size, zw = 1 / quarter_size
    float4 g_params;    // x = alpha
};

cbuffer NeuralWeights : register(b1) {
    float4 g_conv1_w[144];
    float4 g_conv1_b[2];
    float4 g_conv2_w[72];
    float4 g_conv2_b[1];
};

Texture2D<float4> t_feat0       : register(t0);
Texture2D<float4> t_feat1       : register(t1);
Texture2D<float4> t_flow_coarse : register(t2);
Texture2D<float4> t_hidden0     : register(t3);
Texture2D<float4> t_hidden1     : register(t4);
SamplerState s_point : register(s1);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

float2 SafeTanh2(float2 x) {
    float2 c = clamp(x, -10.0, 10.0);
    float2 e2 = exp(-2.0 * c);
    return (1.0 - e2) / (1.0 + e2);
}

float SafeSigmoid(float x) {
    float c = clamp(x, -15.0, 15.0);
    return 1.0 / (1.0 + exp(-c));
}

float4 main(VSOut pin) : SV_Target0 {
    float2 uv = pin.uv;
    float2 q = g_inv_sizes.zw;

    float i0 = t_feat0.SampleLevel(s_point, uv, 0.0).x;
    float i1 = t_feat1.SampleLevel(s_point, uv, 0.0).x;
    float4 fc = t_flow_coarse.SampleLevel(s_point, uv, 0.0);
    float2 v_c = fc.xy;

    if (abs(i1 - i0) < 0.008 && length(v_c) < 0.25) {
        return float4(0.0, 0.0, 0.5, 0.0);
    }

    float4 o = g_conv2_b[0];
    [unroll]
    for (int ky = 0; ky < 3; ++ky) {
        [unroll]
        for (int kx = 0; kx < 3; ++kx) {
            int s = ky * 3 + kx;
            int base = s * 8;
            float2 sample_uv = uv + float2((float)(kx - 1) * q.x, (float)(ky - 1) * q.y);
            float4 h0 = t_hidden0.SampleLevel(s_point, sample_uv, 0.0);
            float4 h1 = t_hidden1.SampleLevel(s_point, sample_uv, 0.0);

            o.x += dot(h0, g_conv2_w[base + 0]) + dot(h1, g_conv2_w[base + 1]);
            o.y += dot(h0, g_conv2_w[base + 2]) + dot(h1, g_conv2_w[base + 3]);
            o.z += dot(h0, g_conv2_w[base + 4]) + dot(h1, g_conv2_w[base + 5]);
            o.w += dot(h0, g_conv2_w[base + 6]) + dot(h1, g_conv2_w[base + 7]);
        }
    }

    float2 dv = SafeTanh2(o.xy) * 3.0;
    float blend_w = SafeSigmoid(o.z);
    float motion_conf = SafeSigmoid(o.w);
    float2 v_ref = clamp(v_c + dv, -15.0, 15.0);
    return float4(v_ref.x, v_ref.y, blend_w, motion_conf);
}
)HLSL";

// Pass 4: Full-Res Bidirectional Occlusion-Aware Warp -> B8G8R8A8_UNORM Shared Surface
constexpr const char* kWarpPS = R"HLSL(
cbuffer PassParams : register(b0) {
    float4 g_inv_sizes; // xy = 1 / full_size, zw = 1 / quarter_size
    float4 g_params;    // x = alpha
};

Texture2D<float4> t_prev : register(t0);
Texture2D<float4> t_curr : register(t1);
Texture2D<float4> t_flow : register(t2);
SamplerState s_linear : register(s0);
SamplerState s_point  : register(s1);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

float4 main(VSOut pin) : SV_Target0 {
    float2 uv = pin.uv;
    float4 flow = t_flow.SampleLevel(s_linear, uv, 0.0);
    float3 base_rgb = t_curr.SampleLevel(s_point, uv, 0.0).rgb;
    float motion_conf = saturate(flow.w);
    if (motion_conf <= 0.001) {
        return float4(base_rgb, 1.0);
    }

    float2 v_uv = flow.xy * g_inv_sizes.zw;
    float alpha = g_params.x;
    float blend_w = saturate(flow.z);

    float3 c0 = t_prev.SampleLevel(s_linear, uv - alpha * v_uv, 0.0).rgb;
    float3 c1 = t_curr.SampleLevel(s_linear, uv + (1.0 - alpha) * v_uv, 0.0).rgb;
    float3 warped = lerp(lerp(c0, c1, alpha), lerp(c0, c1, blend_w), 0.5);
    float3 out_rgb = lerp(base_rgb, warped, motion_conf);
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
    has_frame_pair_ = false;
    coarse_flow_dirty_ = false;
    pacer_.Reset();

    ReleaseRenderTargetInterop();
    ReleaseFrameResources();

    SafeRelease(weights_cb_);
    SafeRelease(params_cb_);
    SafeRelease(depth_state_);
    SafeRelease(blend_state_);
    SafeRelease(rasterizer_state_);
    SafeRelease(point_sampler_);
    SafeRelease(linear_sampler_);

    SafeRelease(ps_warp_);
    SafeRelease(ps_neural_conv2_);
    SafeRelease(ps_neural_conv1_);
    SafeRelease(ps_coarse_flow_);
    SafeRelease(ps_pyramid_);
    SafeRelease(vs_fullscreen_);

    SafeRelease(context_);
    SafeRelease(device_);
}

void D3DFrameInterpolator::ReleaseRenderTargetInterop() {
    SafeRelease(out_d2d_bitmap_);
    bound_d2d_rt_ = nullptr;
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
            ok = compile_ps(kPyramidPS, &ps_pyramid_) &&
                 compile_ps(kCoarseFlowPS, &ps_coarse_flow_) &&
                 compile_ps(kNeuralConv1PS, &ps_neural_conv1_) &&
                 compile_ps(kNeuralConv2PS, &ps_neural_conv2_) &&
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

    NeuralWeightsCBData packed_weights = {};
#if MAC_MIRROR_HAS_NEURAL_FLOW_WEIGHTS
    PackNeuralWeightsCB(neural_flow::kConv1Weight,
                        neural_flow::kConv1Bias,
                        neural_flow::kConv2Weight,
                        neural_flow::kConv2Bias,
                        &packed_weights);
#else
    float fallback_c1_w[8 * 8 * 9];
    float fallback_c1_b[8];
    float fallback_c2_w[4 * 8 * 9];
    float fallback_c2_b[4];
    FillFallbackNeuralWeights(fallback_c1_w, fallback_c1_b, fallback_c2_w, fallback_c2_b);
    PackNeuralWeightsCB(fallback_c1_w, fallback_c1_b, fallback_c2_w, fallback_c2_b, &packed_weights);
#endif

    D3D11_BUFFER_DESC wbd = {};
    wbd.ByteWidth = static_cast<UINT>(sizeof(NeuralWeightsCBData));
    wbd.Usage = D3D11_USAGE_IMMUTABLE;
    wbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA winit = {};
    winit.pSysMem = &packed_weights;
    if (FAILED(device_->CreateBuffer(&wbd, &winit, &weights_cb_)) || !weights_cb_) {
        return false;
    }

    return true;
}

void D3DFrameInterpolator::ReleaseFrameResources() {
    SafeRelease(flow_refined_srv_);
    SafeRelease(flow_refined_rtv_);
    SafeRelease(flow_refined_tex_);

    SafeRelease(nn_hidden1_srv_);
    SafeRelease(nn_hidden1_rtv_);
    SafeRelease(nn_hidden1_tex_);

    SafeRelease(nn_hidden0_srv_);
    SafeRelease(nn_hidden0_rtv_);
    SafeRelease(nn_hidden0_tex_);

    SafeRelease(flow_coarse_srv_);
    SafeRelease(flow_coarse_rtv_);
    SafeRelease(flow_coarse_tex_);

    SafeRelease(feat1_srv_);
    SafeRelease(feat1_rtv_);
    SafeRelease(feat1_tex_);

    SafeRelease(feat0_srv_);
    SafeRelease(feat0_rtv_);
    SafeRelease(feat0_tex_);

    SafeRelease(out_srv_);
    SafeRelease(out_rtv_);
    SafeRelease(out_tex_);

    SafeRelease(curr_srv_);
    SafeRelease(curr_tex_);
    SafeRelease(prev_srv_);
    SafeRelease(prev_tex_);

    tex_width_ = 0;
    tex_height_ = 0;
    q_width_ = 0;
    q_height_ = 0;
}

bool D3DFrameInterpolator::EnsureFrameResources(uint32_t width, uint32_t height) {
    if (tex_width_ == width && tex_height_ == height && prev_tex_ && curr_tex_ && out_tex_ &&
        feat0_tex_ && feat1_tex_ && flow_coarse_tex_ && nn_hidden0_tex_ && nn_hidden1_tex_ &&
        flow_refined_tex_) {
        return true;
    }

    ReleaseRenderTargetInterop();
    ReleaseFrameResources();

    const uint32_t qw = std::max<uint32_t>(4u, width / 4u);
    const uint32_t qh = std::max<uint32_t>(4u, height / 4u);
    const UINT rt_srv = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    const bool ok =
        CreateRenderableTex2D(
            device_, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, &prev_tex_, &prev_srv_, nullptr) &&
        CreateRenderableTex2D(
            device_, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, &curr_tex_, &curr_srv_, nullptr) &&
        CreateRenderableTex2D(
            device_, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, rt_srv, &out_tex_, &out_srv_, &out_rtv_) &&
        CreateRenderableTex2D(
            device_, qw, qh, DXGI_FORMAT_R16G16B16A16_FLOAT, rt_srv, &feat0_tex_, &feat0_srv_, &feat0_rtv_) &&
        CreateRenderableTex2D(
            device_, qw, qh, DXGI_FORMAT_R16G16B16A16_FLOAT, rt_srv, &feat1_tex_, &feat1_srv_, &feat1_rtv_) &&
        CreateRenderableTex2D(
            device_, qw, qh, DXGI_FORMAT_R16G16B16A16_FLOAT, rt_srv, &flow_coarse_tex_, &flow_coarse_srv_, &flow_coarse_rtv_) &&
        CreateRenderableTex2D(
            device_, qw, qh, DXGI_FORMAT_R16G16B16A16_FLOAT, rt_srv, &nn_hidden0_tex_, &nn_hidden0_srv_, &nn_hidden0_rtv_) &&
        CreateRenderableTex2D(
            device_, qw, qh, DXGI_FORMAT_R16G16B16A16_FLOAT, rt_srv, &nn_hidden1_tex_, &nn_hidden1_srv_, &nn_hidden1_rtv_) &&
        CreateRenderableTex2D(
            device_, qw, qh, DXGI_FORMAT_R16G16B16A16_FLOAT, rt_srv, &flow_refined_tex_, &flow_refined_srv_, &flow_refined_rtv_);

    if (!ok) {
        ReleaseFrameResources();
        return false;
    }

    tex_width_ = width;
    tex_height_ = height;
    q_width_ = qw;
    q_height_ = qh;
    return true;
}

bool D3DFrameInterpolator::EnsureSharedBitmap(ID2D1RenderTarget* d2d_rt) {
    if (!d2d_rt || !out_tex_) {
        return false;
    }
    if (out_d2d_bitmap_ && bound_d2d_rt_ == d2d_rt) {
        return true;
    }

    ReleaseRenderTargetInterop();

    IDXGISurface* dxgi_surface = nullptr;
    HRESULT hr = out_tex_->QueryInterface(__uuidof(IDXGISurface), reinterpret_cast<void**>(&dxgi_surface));
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

void D3DFrameInterpolator::UpdatePassParams(float alpha) {
    PassParamsCBData cb = {};
    cb.inv_sizes[0] = 1.0f / static_cast<float>(std::max(1u, tex_width_));
    cb.inv_sizes[1] = 1.0f / static_cast<float>(std::max(1u, tex_height_));
    cb.inv_sizes[2] = 1.0f / static_cast<float>(std::max(1u, q_width_));
    cb.inv_sizes[3] = 1.0f / static_cast<float>(std::max(1u, q_height_));
    cb.params[0] = alpha;
    cb.params[1] = static_cast<float>(tex_width_);
    cb.params[2] = static_cast<float>(tex_height_);
    cb.params[3] = 0.0f;
    context_->UpdateSubresource(params_cb_, 0, nullptr, &cb, 0, 0);
}

bool D3DFrameInterpolator::PushFramePair(ID2D1RenderTarget* d2d_rt,
                                         const uint8_t* prev_bgra,
                                         const uint8_t* curr_bgra,
                                         uint32_t width,
                                         uint32_t height,
                                         uint32_t stride_bytes,
                                         uint64_t now_ms) {
    if (!available_ || !device_ || !context_ || !prev_bgra || !curr_bgra ||
        width < 16u || height < 16u || stride_bytes < width * 4u) {
        return false;
    }

    if (!EnsureFrameResources(width, height)) {
        ResetPacer();
        return false;
    }

    context_->UpdateSubresource(prev_tex_, 0, nullptr, prev_bgra, stride_bytes, 0);
    context_->UpdateSubresource(curr_tex_, 0, nullptr, curr_bgra, stride_bytes, 0);

    if (d2d_rt) {
        EnsureSharedBitmap(d2d_rt);
    }

    has_frame_pair_ = true;
    coarse_flow_dirty_ = true;
    pacer_.OnNewFrame(now_ms);
    return true;
}

ID2D1Bitmap* D3DFrameInterpolator::SynthesizeBeforeD2D(ID2D1RenderTarget* d2d_rt,
                                                       FrameGenMode mode,
                                                       uint64_t now_ms) {
    if (!available_ || !has_frame_pair_ || !d2d_rt || mode == FrameGenMode::Off) {
        return nullptr;
    }

    float alpha = 1.0f;
    if (!pacer_.SampleAlpha(now_ms, mode, &alpha)) {
        return nullptr;
    }

    if (!EnsureSharedBitmap(d2d_rt) || !out_d2d_bitmap_) {
        return nullptr;
    }

    ScopedD3D11State saved_state(context_);

    UpdatePassParams(alpha);

    const FLOAT blend_factor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vs_fullscreen_, nullptr, 0);
    context_->RSSetState(rasterizer_state_);
    context_->OMSetBlendState(blend_state_, blend_factor, 0xFFFFFFFFu);
    context_->OMSetDepthStencilState(depth_state_, 0);

    ID3D11Buffer* cbs[2] = {params_cb_, weights_cb_};
    context_->PSSetConstantBuffers(0, 2, cbs);
    ID3D11SamplerState* samplers[2] = {linear_sampler_, point_sampler_};
    context_->PSSetSamplers(0, 2, samplers);

    D3D11_VIEWPORT q_vp = {};
    q_vp.TopLeftX = 0.0f;
    q_vp.TopLeftY = 0.0f;
    q_vp.Width = static_cast<float>(q_width_);
    q_vp.Height = static_cast<float>(q_height_);
    q_vp.MinDepth = 0.0f;
    q_vp.MaxDepth = 1.0f;

    ID3D11ShaderResourceView* null_srvs[5] = {};
    ID3D11RenderTargetView* null_rtvs[2] = {};

    // Pass 1 & Pass 2 depend only on (prev_tex_, curr_tex_) and run once per RFB frame pair.
    if (coarse_flow_dirty_) {
        context_->RSSetViewports(1, &q_vp);

        // Pass 1a: prev_tex_ -> feat0_tex_
        context_->PSSetShaderResources(0, 5, null_srvs);
        context_->OMSetRenderTargets(1, &feat0_rtv_, nullptr);
        context_->PSSetShader(ps_pyramid_, nullptr, 0);
        context_->PSSetShaderResources(0, 1, &prev_srv_);
        context_->Draw(3, 0);

        // Pass 1b: curr_tex_ -> feat1_tex_
        context_->PSSetShaderResources(0, 1, null_srvs);
        context_->OMSetRenderTargets(1, &feat1_rtv_, nullptr);
        context_->PSSetShaderResources(0, 1, &curr_srv_);
        context_->Draw(3, 0);

        // Pass 2: (feat0_tex_, feat1_tex_) -> flow_coarse_tex_
        context_->OMSetRenderTargets(2, null_rtvs, nullptr);
        context_->OMSetRenderTargets(1, &flow_coarse_rtv_, nullptr);
        context_->PSSetShader(ps_coarse_flow_, nullptr, 0);
        ID3D11ShaderResourceView* p2_srvs[2] = {feat0_srv_, feat1_srv_};
        context_->PSSetShaderResources(0, 2, p2_srvs);
        context_->Draw(3, 0);

        context_->PSSetShaderResources(0, 2, null_srvs);
        context_->OMSetRenderTargets(2, null_rtvs, nullptr);
        coarse_flow_dirty_ = false;
    }

    // Pass 3 (Neural mode only): 2-layer 3x3 CNN refinement (8 -> 8 + ReLU -> 4) at current alpha.
    ID3D11ShaderResourceView* active_flow_srv = flow_coarse_srv_;
    if (mode == FrameGenMode::Neural) {
        context_->RSSetViewports(1, &q_vp);

        // Pass 3a: Conv1 (8 -> 8 + ReLU) via 2x float4 MRT
        ID3D11RenderTargetView* conv1_rtvs[2] = {nn_hidden0_rtv_, nn_hidden1_rtv_};
        context_->OMSetRenderTargets(2, conv1_rtvs, nullptr);
        context_->PSSetShader(ps_neural_conv1_, nullptr, 0);
        ID3D11ShaderResourceView* conv1_srvs[3] = {feat0_srv_, feat1_srv_, flow_coarse_srv_};
        context_->PSSetShaderResources(0, 3, conv1_srvs);
        context_->Draw(3, 0);

        context_->PSSetShaderResources(0, 3, null_srvs);
        context_->OMSetRenderTargets(2, null_rtvs, nullptr);

        // Pass 3b: Conv2 (8 -> 4) -> flow_refined_tex_
        context_->OMSetRenderTargets(1, &flow_refined_rtv_, nullptr);
        context_->PSSetShader(ps_neural_conv2_, nullptr, 0);
        ID3D11ShaderResourceView* conv2_srvs[5] = {
            feat0_srv_, feat1_srv_, flow_coarse_srv_, nn_hidden0_srv_, nn_hidden1_srv_};
        context_->PSSetShaderResources(0, 5, conv2_srvs);
        context_->Draw(3, 0);

        context_->PSSetShaderResources(0, 5, null_srvs);
        context_->OMSetRenderTargets(2, null_rtvs, nullptr);
        active_flow_srv = flow_refined_srv_;
    }

    // Pass 4: Full-resolution bidirectional occlusion-aware warp -> out_tex_
    D3D11_VIEWPORT full_vp = {};
    full_vp.TopLeftX = 0.0f;
    full_vp.TopLeftY = 0.0f;
    full_vp.Width = static_cast<float>(tex_width_);
    full_vp.Height = static_cast<float>(tex_height_);
    full_vp.MinDepth = 0.0f;
    full_vp.MaxDepth = 1.0f;

    context_->RSSetViewports(1, &full_vp);
    context_->OMSetRenderTargets(1, &out_rtv_, nullptr);
    context_->PSSetShader(ps_warp_, nullptr, 0);
    ID3D11ShaderResourceView* warp_srvs[3] = {prev_srv_, curr_srv_, active_flow_srv};
    context_->PSSetShaderResources(0, 3, warp_srvs);
    context_->Draw(3, 0);

    // `saved_state` destructor now unbinds SRVs & RTVs cleanly before D2D `BeginDraw()` reads `out_tex_`.
    return out_d2d_bitmap_;
}

#endif  // _WIN32

}  // namespace mac_mirror
