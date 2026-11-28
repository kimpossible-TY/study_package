#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace mac_mirror {

struct AnimRoi {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t w = 0;
    uint32_t h = 0;
};

// AirPods-ANC-inspired Phase-Locked Periodic Animation Learner & Residual Predictor.
// Learns repeating bounded UI animations (GIFs, loading spinners, pulsing indicators,
// looping shaders), locks onto their period & phase via a jitter-filtering PLL, synthesizes
// smooth 60/120Hz intermediate frames across TCP/DERP stalls, and cancels prediction in 0ms
// when the residual error ("feedback microphone") spikes or user input arrives.
class PeriodicAnimLearner {
public:
    static constexpr uint32_t kMinDim = 24u;
    static constexpr uint32_t kMaxDim = 512u;
    static constexpr size_t kMaxHistory = 24u;
    static constexpr size_t kMinPeriodFrames = 3u;
    static constexpr size_t kMaxPeriodFrames = 16u;

    void Reset() noexcept {
        locked_ = false;
        has_tracked_roi_ = false;
        tracked_roi_ = AnimRoi{};
        fb_w_ = 0;
        fb_h_ = 0;
        hist_head_ = 0;
        hist_count_ = 0;
        cycle_frames_.clear();
        slot_last_seen_ms_.clear();
        period_ms_ = 0.0;
        anchor_time_ms_ = 0.0;
        anchor_phase_ = 0.0;
        last_valid_ms_ = 0;
        has_last_observed_ = false;
    }

    void NotifyUserInteraction() noexcept {
        locked_ = false;
        hist_head_ = 0;
        hist_count_ = 0;
        cycle_frames_.clear();
        slot_last_seen_ms_.clear();
        period_ms_ = 0.0;
        anchor_time_ms_ = 0.0;
        anchor_phase_ = 0.0;
        last_valid_ms_ = 0;
        has_last_observed_ = false;
    }

    void ObserveFrame(uint64_t now_ms,
                      const uint8_t* curr_bgra,
                      uint32_t fb_w,
                      uint32_t fb_h,
                      uint32_t stride_bytes,
                      const AnimRoi& dirty_roi) {
        if (!curr_bgra || fb_w == 0 || fb_h == 0 || stride_bytes < fb_w * 4u) {
            return;
        }
        if (has_tracked_roi_ && (fb_w != fb_w_ || fb_h != fb_h_)) {
            Reset();
        }
        if (dirty_roi.w == 0 || dirty_roi.h == 0 || dirty_roi.x >= fb_w || dirty_roi.y >= fb_h) {
            return;
        }

        const uint32_t rx = dirty_roi.x;
        const uint32_t ry = dirty_roi.y;
        const uint32_t rw = (std::min)(dirty_roi.w, fb_w - rx);
        const uint32_t rh = (std::min)(dirty_roi.h, fb_h - ry);
        if (rw == 0 || rh == 0) {
            return;
        }
        const AnimRoi clamped{rx, ry, rw, rh};

        const uint64_t fb_area = static_cast<uint64_t>(fb_w) * static_cast<uint64_t>(fb_h);
        const uint64_t roi_area = static_cast<uint64_t>(rw) * static_cast<uint64_t>(rh);
        if (roi_area > fb_area / 2u) {
            Reset();
            return;
        }

        // Expire stale lock cleanly if packets stopped arriving beyond the hold window.
        if (locked_ && !IsLocked(now_ms)) {
            BreakLockKeepRoi();
        }

        // 4. ANC Residual Error Feedback ("Feedback Microphone") when locked:
        if (locked_ && has_tracked_roi_) {
            if (!RoisOverlap(clamped, tracked_roi_)) {
                return;
            }

            ExtractTightBgra(curr_bgra, stride_bytes, tracked_roi_, &scratch_bgra_);
            uint64_t curr_dhash = 0;
            std::array<uint8_t, 16> curr_sketch{};
            ComputeSignature(
                scratch_bgra_.data(), tracked_roi_.w, tracked_roi_.h, &curr_dhash, curr_sketch.data());

            const size_t P = cycle_frames_.size();
            const double Pf = static_cast<double>(P);
            const double pred_phase = PredictPhaseAt(now_ms);

            float min_raw_mad = 255.0f;
            std::array<float, kMaxPeriodFrames> slot_mads{};
            for (size_t s = 0; s < P; ++s) {
                const float mad = ComputeFrameMad(
                    scratch_bgra_.data(), cycle_frames_[s].bgra.data(), tracked_roi_.w, tracked_roi_.h);
                slot_mads[s] = mad;
                if (mad < min_raw_mad) {
                    min_raw_mad = mad;
                }
            }

            if (min_raw_mad > kResidualBreakMad) {
                // Prediction residual spiked (GIF changed/stopped or UI updated): break lock in 0ms!
                BreakLockKeepRoi();
                if (!IsEligibleRoi(clamped, fb_area)) {
                    Reset();
                    return;
                }
                UpdateTrackedRoiUnlocked(clamped, curr_bgra, fb_w, fb_h, stride_bytes, fb_area);
                ExtractTightBgra(curr_bgra, stride_bytes, tracked_roi_, &scratch_bgra_);
                ComputeSignature(scratch_bgra_.data(),
                                 tracked_roi_.w,
                                 tracked_roi_.h,
                                 &curr_dhash,
                                 curr_sketch.data());
                PushHistoryFrame(now_ms, scratch_bgra_, curr_dhash, curr_sketch);
                return;
            }

            // Ignore duplicate zero-change packet so frozen widgets or redundant updates
            // neither skew the PLL nor extend last_valid_ms_ forever.
            if (has_last_observed_ && SketchMad(curr_sketch.data(), last_observed_sketch_.data()) < 1.0f &&
                min_raw_mad < 1.0f) {
                return;
            }
            last_observed_sketch_ = curr_sketch;
            has_last_observed_ = true;

            // Choose best matching slot, breaking near-ties (e.g. symmetric pulsing animations)
            // toward the predicted phase.
            size_t best_m = 0;
            float best_score = 1e9f;
            const float tie_band = (std::min)(kResidualBreakMad, min_raw_mad + 1.5f);
            for (size_t s = 0; s < P; ++s) {
                if (slot_mads[s] > tie_band) {
                    continue;
                }
                const float dist = static_cast<float>(
                    std::fabs(ShortestPhaseDelta(static_cast<double>(s), pred_phase, Pf)));
                const float score = slot_mads[s] + 0.5f * dist;
                if (score < best_score) {
                    best_score = score;
                    best_m = s;
                }
            }

            // Refresh matched cycle slot with fresh BGRA pixels and perceptual signature.
            cycle_frames_[best_m].bgra = scratch_bgra_;
            cycle_frames_[best_m].dhash = curr_dhash;
            cycle_frames_[best_m].sketch = curr_sketch;

            // PLL period update on full cycle wrap of slot `best_m`.
            if (best_m < slot_last_seen_ms_.size() && now_ms > slot_last_seen_ms_[best_m]) {
                const double dt_slot = static_cast<double>(now_ms - slot_last_seen_ms_[best_m]);
                const double wraps = std::round(dt_slot / period_ms_);
                if (wraps >= 1.0) {
                    const double implied_period = dt_slot / wraps;
                    if (implied_period >= kMinPeriodMs && implied_period <= kMaxPeriodMs &&
                        std::fabs(implied_period - period_ms_) <= 0.40 * period_ms_) {
                        period_ms_ = std::clamp(
                            0.85 * period_ms_ + 0.15 * implied_period, kMinPeriodMs, kMaxPeriodMs);
                    }
                    slot_last_seen_ms_[best_m] = now_ms;
                }
            }

            // Smooth PLL phase update (0.7 * predicted + 0.3 * observed_m along shortest wrap).
            const double pred_now = PredictPhaseAt(now_ms);
            const double phase_err = ShortestPhaseDelta(static_cast<double>(best_m), pred_now, Pf);
            double smoothed_phase = std::fmod(pred_now + 0.3 * phase_err, Pf);
            if (smoothed_phase < 0.0) {
                smoothed_phase += Pf;
            }
            anchor_time_ms_ = static_cast<double>(now_ms);
            anchor_phase_ = smoothed_phase;
            last_valid_ms_ = now_ms;
            return;
        }

        // 1. Unlocked ROI Eligibility & Geometry Stability:
        if (!IsEligibleRoi(clamped, fb_area)) {
            if (has_tracked_roi_ && RoisOverlap(clamped, tracked_roi_) &&
                (clamped.w > kMaxDim || clamped.h > kMaxDim || roi_area > (fb_area * 45u) / 100u)) {
                Reset();
            }
            return;
        }

        if (hist_count_ > 0 && now_ms < HistoryAt(hist_count_ - 1u).time_ms) {
            Reset();
        }

        UpdateTrackedRoiUnlocked(clamped, curr_bgra, fb_w, fb_h, stride_bytes, fb_area);

        // 2. Perceptual Signature (64-bit dHash + 16-sample Luma Sketch):
        ExtractTightBgra(curr_bgra, stride_bytes, tracked_roi_, &scratch_bgra_);
        uint64_t curr_dhash = 0;
        std::array<uint8_t, 16> curr_sketch{};
        ComputeSignature(
            scratch_bgra_.data(), tracked_roi_.w, tracked_roi_.h, &curr_dhash, curr_sketch.data());

        if (hist_count_ > 0) {
            const FrameSample& last = HistoryAt(hist_count_ - 1u);
            if (SketchMad(curr_sketch.data(), last.sketch.data()) < 1.0f &&
                ComputeFrameMad(scratch_bgra_.data(),
                                last.bgra.data(),
                                tracked_roi_.w,
                                tracked_roi_.h) < 1.5f) {
                return;
            }
        }

        PushHistoryFrame(now_ms, scratch_bgra_, curr_dhash, curr_sketch);
        last_observed_sketch_ = curr_sketch;
        has_last_observed_ = true;

        // 3. Cycle Closure Detection:
        TryDetectCycleClosure(now_ms);
    }

    bool SynthesizeAt(uint64_t now_ms,
                      AnimRoi* out_roi,
                      std::vector<uint8_t>* out_roi_bgra) const {
        if (!out_roi || !out_roi_bgra || !IsLocked(now_ms)) {
            return false;
        }
        const size_t P = cycle_frames_.size();
        const size_t expected_bytes =
            static_cast<size_t>(tracked_roi_.w) * static_cast<size_t>(tracked_roi_.h) * 4u;
        if (P < kMinPeriodFrames || expected_bytes == 0) {
            return false;
        }

        const double Pf = static_cast<double>(P);
        const double elapsed = static_cast<double>(now_ms) - anchor_time_ms_;
        const double cycles = elapsed / period_ms_;
        const double frac_cycle = cycles - std::floor(cycles);
        double u = anchor_phase_ + frac_cycle * Pf;
        u -= std::floor(u / Pf) * Pf;
        if (u < 0.0 || u >= Pf) {
            u = 0.0;
        }

        const size_t k0 = static_cast<size_t>(u) % P;
        const size_t k1 = (k0 + 1u) % P;
        if (cycle_frames_[k0].bgra.size() != expected_bytes ||
            cycle_frames_[k1].bgra.size() != expected_bytes) {
            return false;
        }

        const float beta = std::clamp(static_cast<float>(u - std::floor(u)), 0.0f, 1.0f);
        const uint32_t w1 = static_cast<uint32_t>(beta * 256.0f + 0.5f);
        const uint32_t w0 = 256u - w1;

        *out_roi = tracked_roi_;
        out_roi_bgra->resize(expected_bytes);
        uint8_t* dst = out_roi_bgra->data();
        const uint8_t* src0 = cycle_frames_[k0].bgra.data();
        const uint8_t* src1 = cycle_frames_[k1].bgra.data();

        if (w1 == 0u) {
            std::memcpy(dst, src0, expected_bytes);
            return true;
        }
        if (w1 >= 256u) {
            std::memcpy(dst, src1, expected_bytes);
            return true;
        }

        for (size_t off = 0; off < expected_bytes; off += 4u) {
            dst[off + 0] = static_cast<uint8_t>(
                (static_cast<uint32_t>(src0[off + 0]) * w0 + static_cast<uint32_t>(src1[off + 0]) * w1 + 128u) >> 8);
            dst[off + 1] = static_cast<uint8_t>(
                (static_cast<uint32_t>(src0[off + 1]) * w0 + static_cast<uint32_t>(src1[off + 1]) * w1 + 128u) >> 8);
            dst[off + 2] = static_cast<uint8_t>(
                (static_cast<uint32_t>(src0[off + 2]) * w0 + static_cast<uint32_t>(src1[off + 2]) * w1 + 128u) >> 8);
            dst[off + 3] = 0xFFu;
        }
        return true;
    }

    bool IsLocked(uint64_t now_ms) const noexcept {
        if (!locked_ || cycle_frames_.size() < kMinPeriodFrames || period_ms_ < kMinPeriodMs) {
            return false;
        }
        if (now_ms < last_valid_ms_) {
            return false;
        }
        const double max_gap_ms = (std::max)(600.0, period_ms_ * 2.5);
        return static_cast<double>(now_ms - last_valid_ms_) <= max_gap_ms;
    }

    size_t locked_period_frames() const noexcept {
        return locked_ ? cycle_frames_.size() : 0u;
    }

    double locked_period_ms() const noexcept {
        return locked_ ? period_ms_ : 0.0;
    }

private:
    static constexpr double kMinPeriodMs = 120.0;
    static constexpr double kMaxPeriodMs = 4000.0;
    static constexpr uint32_t kClosureMaxHamming = 6u;
    static constexpr float kClosureMaxSketchMad = 5.0f;
    static constexpr float kClosureMaxPixelMad = 6.0f;
    static constexpr float kMinMotionSketchMad = 6.0f;
    static constexpr float kResidualBreakMad = 7.5f;

    struct FrameSample {
        uint64_t time_ms = 0;
        uint64_t dhash = 0;
        std::array<uint8_t, 16> sketch{};
        std::vector<uint8_t> bgra;
    };

    static bool IsEligibleRoi(const AnimRoi& r, uint64_t fb_area) noexcept {
        if (r.w < kMinDim || r.h < kMinDim || r.w > kMaxDim || r.h > kMaxDim) {
            return false;
        }
        const uint64_t area = static_cast<uint64_t>(r.w) * static_cast<uint64_t>(r.h);
        return area <= (fb_area * 45u) / 100u;
    }

    static bool RoisOverlap(const AnimRoi& a, const AnimRoi& b) noexcept {
        const uint32_t x0 = (std::max)(a.x, b.x);
        const uint32_t y0 = (std::max)(a.y, b.y);
        const uint32_t x1 = (std::min)(a.x + a.w, b.x + b.w);
        const uint32_t y1 = (std::min)(a.y + a.h, b.y + b.h);
        return (x1 > x0) && (y1 > y0);
    }

    static float ComputeIoU(const AnimRoi& a, const AnimRoi& b) noexcept {
        const uint32_t x0 = (std::max)(a.x, b.x);
        const uint32_t y0 = (std::max)(a.y, b.y);
        const uint32_t x1 = (std::min)(a.x + a.w, b.x + b.w);
        const uint32_t y1 = (std::min)(a.y + a.h, b.y + b.h);
        if (x1 <= x0 || y1 <= y0) {
            return 0.0f;
        }
        const double inter = static_cast<double>(x1 - x0) * static_cast<double>(y1 - y0);
        const double area_a = static_cast<double>(a.w) * static_cast<double>(a.h);
        const double area_b = static_cast<double>(b.w) * static_cast<double>(b.h);
        const double uni = area_a + area_b - inter;
        return (uni > 0.0) ? static_cast<float>(inter / uni) : 0.0f;
    }

    static bool WithinEdgeTolerance(const AnimRoi& a, const AnimRoi& b, int64_t tol_px) noexcept {
        const int64_t dx0 = std::llabs(static_cast<int64_t>(a.x) - static_cast<int64_t>(b.x));
        const int64_t dy0 = std::llabs(static_cast<int64_t>(a.y) - static_cast<int64_t>(b.y));
        const int64_t dx1 =
            std::llabs(static_cast<int64_t>(a.x + a.w) - static_cast<int64_t>(b.x + b.w));
        const int64_t dy1 =
            std::llabs(static_cast<int64_t>(a.y + a.h) - static_cast<int64_t>(b.y + b.h));
        return dx0 <= tol_px && dy0 <= tol_px && dx1 <= tol_px && dy1 <= tol_px;
    }

    static uint32_t Popcount64(uint64_t x) noexcept {
        x = x - ((x >> 1) & 0x5555555555555555ULL);
        x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
        return static_cast<uint32_t>(
            (((x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL) * 0x0101010101010101ULL) >> 56);
    }

    static float SketchMad(const uint8_t a[16], const uint8_t b[16]) noexcept {
        uint32_t sum = 0;
        for (size_t i = 0; i < 16u; ++i) {
            sum += static_cast<uint32_t>(
                std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])));
        }
        return static_cast<float>(sum) * (1.0f / 16.0f);
    }

    static float SampleBoxLuma(const uint8_t* tight_bgra,
                               uint32_t w,
                               uint32_t x0,
                               uint32_t y0,
                               uint32_t x1,
                               uint32_t y1,
                               uint32_t max_sub_samples) noexcept {
        const uint32_t span_x = (std::max)(1u, x1 - x0);
        const uint32_t span_y = (std::max)(1u, y1 - y0);
        const uint32_t step_x = (std::max)(1u, span_x / max_sub_samples);
        const uint32_t step_y = (std::max)(1u, span_y / max_sub_samples);
        uint64_t sum = 0;
        uint64_t count = 0;
        for (uint32_t y = y0; y < y1; y += step_y) {
            const uint8_t* row = tight_bgra + static_cast<size_t>(y) * w * 4u;
            for (uint32_t x = x0; x < x1; x += step_x) {
                const size_t off = static_cast<size_t>(x) * 4u;
                const uint32_t luma = (static_cast<uint32_t>(row[off + 0]) * 29u +
                                       static_cast<uint32_t>(row[off + 1]) * 150u +
                                       static_cast<uint32_t>(row[off + 2]) * 77u + 128u) >>
                                      8;
                sum += luma;
                ++count;
            }
        }
        return (count > 0) ? (static_cast<float>(sum) / static_cast<float>(count)) : 0.0f;
    }

    static void ComputeSignature(const uint8_t* tight_bgra,
                                 uint32_t w,
                                 uint32_t h,
                                 uint64_t* out_dhash,
                                 uint8_t out_sketch[16]) noexcept {
        float luma_grid[8][9]{};
        for (uint32_t gy = 0; gy < 8u; ++gy) {
            const uint32_t y0 = (gy * h) / 8u;
            const uint32_t y1 = (std::max)(y0 + 1u, ((gy + 1u) * h) / 8u);
            for (uint32_t gx = 0; gx < 9u; ++gx) {
                const uint32_t x0 = (gx * w) / 9u;
                const uint32_t x1 = (std::max)(x0 + 1u, ((gx + 1u) * w) / 9u);
                luma_grid[gy][gx] = SampleBoxLuma(tight_bgra, w, x0, y0, x1, y1, 4u);
            }
        }

        uint64_t dhash = 0;
        for (uint32_t gy = 0; gy < 8u; ++gy) {
            for (uint32_t gx = 0; gx < 8u; ++gx) {
                if (luma_grid[gy][gx + 1u] > luma_grid[gy][gx]) {
                    dhash |= (uint64_t{1} << (gy * 8u + gx));
                }
            }
        }
        *out_dhash = dhash;

        for (uint32_t sy = 0; sy < 4u; ++sy) {
            const uint32_t y0 = (sy * h) / 4u;
            const uint32_t y1 = (std::max)(y0 + 1u, ((sy + 1u) * h) / 4u);
            for (uint32_t sx = 0; sx < 4u; ++sx) {
                const uint32_t x0 = (sx * w) / 4u;
                const uint32_t x1 = (std::max)(x0 + 1u, ((sx + 1u) * w) / 4u);
                const float cell_luma = SampleBoxLuma(tight_bgra, w, x0, y0, x1, y1, 8u);
                const int q = std::clamp(static_cast<int>(cell_luma + 0.5f), 0, 255);
                out_sketch[sy * 4u + sx] = static_cast<uint8_t>(q);
            }
        }
    }

    static float ComputeFrameMad(const uint8_t* a_bgra,
                                 const uint8_t* b_bgra,
                                 uint32_t w,
                                 uint32_t h) noexcept {
        if (!a_bgra || !b_bgra || w == 0 || h == 0) {
            return 255.0f;
        }
        const uint32_t step_x = (std::max)(1u, w / 64u);
        const uint32_t step_y = (std::max)(1u, h / 64u);
        uint64_t sum_abs = 0;
        uint64_t samples = 0;
        for (uint32_t y = 0; y < h; y += step_y) {
            const size_t row_off = static_cast<size_t>(y) * w * 4u;
            for (uint32_t x = 0; x < w; x += step_x) {
                const size_t off = row_off + static_cast<size_t>(x) * 4u;
                sum_abs += static_cast<uint64_t>(
                    std::abs(static_cast<int>(a_bgra[off + 0]) - static_cast<int>(b_bgra[off + 0])) +
                    std::abs(static_cast<int>(a_bgra[off + 1]) - static_cast<int>(b_bgra[off + 1])) +
                    std::abs(static_cast<int>(a_bgra[off + 2]) - static_cast<int>(b_bgra[off + 2])));
                ++samples;
            }
        }
        return (samples > 0)
                   ? (static_cast<float>(sum_abs) / static_cast<float>(samples * 3u))
                   : 255.0f;
    }

    static void ExtractTightBgra(const uint8_t* fb_bgra,
                                 uint32_t stride_bytes,
                                 const AnimRoi& roi,
                                 std::vector<uint8_t>* out_tight) {
        const size_t row_bytes = static_cast<size_t>(roi.w) * 4u;
        const size_t total_bytes = row_bytes * static_cast<size_t>(roi.h);
        out_tight->resize(total_bytes);
        uint8_t* dst = out_tight->data();
        for (uint32_t r = 0; r < roi.h; ++r) {
            const uint8_t* src = fb_bgra + static_cast<size_t>(roi.y + r) * stride_bytes +
                                 static_cast<size_t>(roi.x) * 4u;
            uint8_t* dst_row = dst + static_cast<size_t>(r) * row_bytes;
            std::memcpy(dst_row, src, row_bytes);
            for (size_t off = 3u; off < row_bytes; off += 4u) {
                dst_row[off] = 0xFFu;
            }
        }
    }

    static double ShortestPhaseDelta(double target, double current, double period) noexcept {
        double d = std::fmod(target - current, period);
        if (d > 0.5 * period) {
            d -= period;
        } else if (d < -0.5 * period) {
            d += period;
        }
        return d;
    }

    double PredictPhaseAt(uint64_t now_ms) const noexcept {
        const size_t P = cycle_frames_.size();
        if (P == 0 || period_ms_ <= 0.0) {
            return 0.0;
        }
        const double Pf = static_cast<double>(P);
        const double elapsed = static_cast<double>(now_ms) - anchor_time_ms_;
        const double cycles = elapsed / period_ms_;
        const double frac = cycles - std::floor(cycles);
        double u = anchor_phase_ + frac * Pf;
        u -= std::floor(u / Pf) * Pf;
        return (u >= 0.0 && u < Pf) ? u : 0.0;
    }

    void BreakLockKeepRoi() noexcept {
        locked_ = false;
        hist_head_ = 0;
        hist_count_ = 0;
        cycle_frames_.clear();
        slot_last_seen_ms_.clear();
        period_ms_ = 0.0;
        anchor_time_ms_ = 0.0;
        anchor_phase_ = 0.0;
        last_valid_ms_ = 0;
        has_last_observed_ = false;
    }

    FrameSample& HistoryAt(size_t logical_idx) noexcept {
        return history_[(hist_head_ + logical_idx) % kMaxHistory];
    }

    const FrameSample& HistoryAt(size_t logical_idx) const noexcept {
        return history_[(hist_head_ + logical_idx) % kMaxHistory];
    }

    void PushHistoryFrame(uint64_t now_ms,
                          const std::vector<uint8_t>& tight_bgra,
                          uint64_t dhash,
                          const std::array<uint8_t, 16>& sketch) {
        size_t phys_idx = 0;
        if (hist_count_ < kMaxHistory) {
            phys_idx = (hist_head_ + hist_count_) % kMaxHistory;
            ++hist_count_;
        } else {
            phys_idx = hist_head_;
            hist_head_ = (hist_head_ + 1u) % kMaxHistory;
        }
        FrameSample& dst = history_[phys_idx];
        dst.time_ms = now_ms;
        dst.dhash = dhash;
        dst.sketch = sketch;
        dst.bgra = tight_bgra;
    }

    void RemapHistoryToExpandedRoi(const AnimRoi& old_roi,
                                   const AnimRoi& new_roi,
                                   const uint8_t* curr_fb_bgra,
                                   uint32_t stride_bytes) {
        std::vector<uint8_t> remapped;
        ExtractTightBgra(curr_fb_bgra, stride_bytes, new_roi, &remapped);

        const uint32_t off_x = old_roi.x - new_roi.x;
        const uint32_t off_y = old_roi.y - new_roi.y;
        const size_t old_row_bytes = static_cast<size_t>(old_roi.w) * 4u;
        const size_t new_row_bytes = static_cast<size_t>(new_roi.w) * 4u;

        for (size_t k = 0; k < hist_count_; ++k) {
            FrameSample& entry = HistoryAt(k);
            std::vector<uint8_t> expanded = remapped;
            for (uint32_t r = 0; r < old_roi.h; ++r) {
                std::memcpy(expanded.data() + static_cast<size_t>(off_y + r) * new_row_bytes +
                                static_cast<size_t>(off_x) * 4u,
                            entry.bgra.data() + static_cast<size_t>(r) * old_row_bytes,
                            old_row_bytes);
            }
            entry.bgra.swap(expanded);
            ComputeSignature(
                entry.bgra.data(), new_roi.w, new_roi.h, &entry.dhash, entry.sketch.data());
        }
    }

    void UpdateTrackedRoiUnlocked(const AnimRoi& clamped,
                                  const uint8_t* curr_bgra,
                                  uint32_t fb_w,
                                  uint32_t fb_h,
                                  uint32_t stride_bytes,
                                  uint64_t fb_area) {
        if (!has_tracked_roi_) {
            tracked_roi_ = clamped;
            has_tracked_roi_ = true;
            fb_w_ = fb_w;
            fb_h_ = fb_h;
            return;
        }

        const float iou = ComputeIoU(clamped, tracked_roi_);
        const bool edge_close = WithinEdgeTolerance(clamped, tracked_roi_, 12);
        if (iou >= 0.70f || (edge_close && iou >= 0.50f)) {
            const uint32_t ux0 = (std::min)(tracked_roi_.x, clamped.x);
            const uint32_t uy0 = (std::min)(tracked_roi_.y, clamped.y);
            const uint32_t ux1 = (std::max)(tracked_roi_.x + tracked_roi_.w, clamped.x + clamped.w);
            const uint32_t uy1 = (std::max)(tracked_roi_.y + tracked_roi_.h, clamped.y + clamped.h);
            const AnimRoi union_roi{ux0, uy0, ux1 - ux0, uy1 - uy0};

            if (union_roi.x == tracked_roi_.x && union_roi.y == tracked_roi_.y &&
                union_roi.w == tracked_roi_.w && union_roi.h == tracked_roi_.h) {
                return;
            }
            if (IsEligibleRoi(union_roi, fb_area) &&
                (union_roi.w - tracked_roi_.w) <= 24u &&
                (union_roi.h - tracked_roi_.h) <= 24u) {
                const AnimRoi old_roi = tracked_roi_;
                tracked_roi_ = union_roi;
                RemapHistoryToExpandedRoi(old_roi, union_roi, curr_bgra, stride_bytes);
                return;
            }
        }

        // ROI jumped to a new location or changed geometry substantially: reset history cleanly.
        hist_head_ = 0;
        hist_count_ = 0;
        has_last_observed_ = false;
        tracked_roi_ = clamped;
        has_tracked_roi_ = true;
        fb_w_ = fb_w;
        fb_h_ = fb_h;
    }

    bool FramesMatchClosure(const FrameSample& a, const FrameSample& b) const noexcept {
        if (Popcount64(a.dhash ^ b.dhash) > kClosureMaxHamming) {
            return false;
        }
        if (SketchMad(a.sketch.data(), b.sketch.data()) > kClosureMaxSketchMad) {
            return false;
        }
        return ComputeFrameMad(a.bgra.data(), b.bgra.data(), tracked_roi_.w, tracked_roi_.h) <=
               kClosureMaxPixelMad;
    }

    void TryDetectCycleClosure(uint64_t now_ms) {
        if (hist_count_ < kMinPeriodFrames + 2u) {
            return;
        }
        const size_t i = hist_count_ - 1u;
        const FrameSample& curr = HistoryAt(i);
        const FrameSample& prev = HistoryAt(i - 1u);

        for (size_t P = kMinPeriodFrames; P <= kMaxPeriodFrames; ++P) {
            if (hist_count_ < P + 2u) {
                break;
            }
            const FrameSample& curr_ref = HistoryAt(i - P);
            const FrameSample& prev_ref = HistoryAt(i - 1u - P);

            if (now_ms <= curr_ref.time_ms) {
                continue;
            }
            const uint64_t dur_ms = now_ms - curr_ref.time_ms;
            if (dur_ms < static_cast<uint64_t>(kMinPeriodMs) ||
                dur_ms > static_cast<uint64_t>(kMaxPeriodMs)) {
                continue;
            }

            if (!FramesMatchClosure(curr, curr_ref) || !FramesMatchClosure(prev, prev_ref)) {
                continue;
            }

            // Require genuine visual motion within the cycle so static regions never lock.
            bool has_motion = false;
            for (size_t k = i - P + 1u; k < i; ++k) {
                const FrameSample& mid = HistoryAt(k);
                if (SketchMad(mid.sketch.data(), curr.sketch.data()) >= kMinMotionSketchMad ||
                    ComputeFrameMad(
                        mid.bgra.data(), curr.bgra.data(), tracked_roi_.w, tracked_roi_.h) >=
                        12.0f) {
                    has_motion = true;
                    break;
                }
            }
            if (!has_motion) {
                continue;
            }

            // Copy P cycle frames [i - P + 1 .. i] into cycle_frames_[0 .. P - 1].
            cycle_frames_.resize(P);
            slot_last_seen_ms_.resize(P);
            for (size_t s = 0; s < P; ++s) {
                const FrameSample& src = HistoryAt(i - P + 1u + s);
                cycle_frames_[s] = src;
                slot_last_seen_ms_[s] = src.time_ms;
            }

            double est_period = static_cast<double>(dur_ms);
            if (prev.time_ms > prev_ref.time_ms) {
                const double prev_dur = static_cast<double>(prev.time_ms - prev_ref.time_ms);
                if (prev_dur >= 80.0 && prev_dur <= 4500.0) {
                    est_period = std::clamp(0.5 * (est_period + prev_dur), kMinPeriodMs, kMaxPeriodMs);
                }
            }

            period_ms_ = est_period;
            anchor_time_ms_ = static_cast<double>(now_ms);
            anchor_phase_ = static_cast<double>(P - 1u);
            last_valid_ms_ = now_ms;
            locked_ = true;
            return;
        }
    }

    bool locked_ = false;
    bool has_tracked_roi_ = false;
    AnimRoi tracked_roi_{};
    uint32_t fb_w_ = 0;
    uint32_t fb_h_ = 0;

    std::array<FrameSample, kMaxHistory> history_{};
    size_t hist_head_ = 0;
    size_t hist_count_ = 0;

    std::vector<FrameSample> cycle_frames_;
    std::vector<uint64_t> slot_last_seen_ms_;
    std::vector<uint8_t> scratch_bgra_;

    double period_ms_ = 0.0;
    double anchor_time_ms_ = 0.0;
    double anchor_phase_ = 0.0;
    uint64_t last_valid_ms_ = 0;

    std::array<uint8_t, 16> last_observed_sketch_{};
    bool has_last_observed_ = false;
};

}  // namespace mac_mirror
