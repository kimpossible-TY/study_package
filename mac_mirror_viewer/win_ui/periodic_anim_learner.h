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

// AirPods-ANC-inspired Drop-Resilient Long-GIF Phase-Bucket Animation Learner & Residual Predictor.
// Learns repeating bounded UI animations (short spinners through 30-120+ frame GIFs up to 12s),
// stores ROI frames compactly in 16-bit RGB565 under a strict 64 MB memory budget, locks onto
// fundamental period & normalized phase via time-delta matching + jitter-filtering PLL, fills
// dropped-frame gaps progressively across laps, synthesizes smooth 60/120Hz frames across TCP/DERP
// stalls, and cancels prediction in 0ms when residual error ("feedback microphone") spikes or user
// input arrives.
class PeriodicAnimLearner {
public:
    static constexpr uint32_t kMinDim = 24u;
    static constexpr uint32_t kMaxDim = 512u;
    static constexpr size_t kMaxMemoryBytes = 64u * 1024u * 1024u;
    static constexpr size_t kMaxHistory = 160u;
    static constexpr size_t kMaxBuckets = 128u;
    static constexpr size_t kMinPeriodFrames = 3u;
    static constexpr double kMinPeriodMs = 120.0;
    static constexpr double kMaxPeriodMs = 12000.0;

    void Reset() noexcept {
        locked_ = false;
        has_tracked_roi_ = false;
        tracked_roi_ = AnimRoi{};
        fb_w_ = 0;
        fb_h_ = 0;
        ClearHistory();
        ClearBuckets();
        std::vector<uint16_t>().swap(scratch_rgb565_);
        period_ms_ = 0.0;
        anchor_time_ms_ = 0.0;
        anchor_phase_ = 0.0;
        last_valid_ms_ = 0;
        has_last_observed_ = false;
        has_last_matched_bucket_ = false;
        last_matched_bucket_ = 0;
    }

    void NotifyUserInteraction() noexcept {
        BreakLockKeepRoi();
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

        // 4. ANC Residual Feedback ("Feedback Microphone") + Progressive Bucket Fill when locked:
        if (locked_ && has_tracked_roi_) {
            if (!RoisOverlap(clamped, tracked_roi_)) {
                return;
            }

            ExtractTightRgb565(curr_bgra, stride_bytes, tracked_roi_, &scratch_rgb565_);
            uint64_t curr_dhash = 0;
            std::array<uint8_t, 16> curr_sketch{};
            ComputeSignatureFromRgb565(
                scratch_rgb565_.data(), tracked_roi_.w, tracked_roi_.h, &curr_dhash, curr_sketch.data());

            const size_t B = bucket_count_;
            const double Bf = static_cast<double>(B);
            const double phi_pred = PredictUnitPhaseAt(now_ms);
            const size_t b_pred = PhaseToBucket(phi_pred, B);
            const size_t window = (std::max)(size_t{2u}, B / 12u);

            // Search populated buckets within cyclic window +-max(2, B / 12) around b_pred.
            float min_pop_mad = 255.0f;
            float pred_bucket_mad = 255.0f;
            bool has_pop_candidate = false;

            const size_t max_steps = (std::min)(window, B / 2u);
            struct WindowCandidate {
                size_t bucket = 0;
                float mad = 255.0f;
            };
            std::array<WindowCandidate, kMaxBuckets> candidates{};
            size_t cand_count = 0;

            auto eval_bucket = [&](size_t b) {
                if (!buckets_[b].populated || buckets_[b].rgb565.size() != scratch_rgb565_.size()) {
                    return;
                }
                const float mad = ComputeRgb565Mad(
                    scratch_rgb565_.data(), buckets_[b].rgb565.data(), tracked_roi_.w, tracked_roi_.h);
                if (b == b_pred) {
                    pred_bucket_mad = mad;
                }
                if (cand_count < candidates.size()) {
                    candidates[cand_count++] = WindowCandidate{b, mad};
                }
                if (mad < min_pop_mad) {
                    min_pop_mad = mad;
                }
                has_pop_candidate = true;
            };

            eval_bucket(b_pred);
            for (size_t step = 1u; step <= max_steps; ++step) {
                const size_t b_plus = (b_pred + step) % B;
                if (b_plus != b_pred) {
                    eval_bucket(b_plus);
                }
                const size_t b_minus = (b_pred + B - step) % B;
                if (b_minus != b_pred && b_minus != b_plus) {
                    eval_bucket(b_minus);
                }
            }

            // Pick best matching populated bucket, breaking near-ties toward predicted phase.
            size_t best_b = b_pred;
            float best_mad = min_pop_mad;
            if (has_pop_candidate) {
                float best_score = 1e9f;
                const float tie_band = (std::min)(kResidualBreakMad, min_pop_mad + 1.5f);
                for (size_t idx = 0; idx < cand_count; ++idx) {
                    if (candidates[idx].mad > tie_band) {
                        continue;
                    }
                    const double b_phi = static_cast<double>(candidates[idx].bucket) / Bf;
                    const float phase_dist =
                        static_cast<float>(std::fabs(ShortestUnitDelta(b_phi, phi_pred)) * Bf);
                    const float score = candidates[idx].mad + 0.5f * phase_dist;
                    if (score < best_score) {
                        best_score = score;
                        best_b = candidates[idx].bucket;
                        best_mad = candidates[idx].mad;
                    }
                }
            }

            // If b_pred is unpopulated (gap from an earlier lap), also check MAD against
            // the interpolated prediction at phi_pred so legitimate gap frames are accepted.
            const bool pred_is_gap = !buckets_[b_pred].populated;
            float interp_mad = 255.0f;
            if (pred_is_gap) {
                size_t b0 = 0;
                size_t b1 = 0;
                double d0 = 0.0;
                double d1 = 0.0;
                const double u = phi_pred * Bf;
                if (FindBracketingBuckets(u, &b0, &d0, &b1, &d1)) {
                    const float beta =
                        std::clamp(static_cast<float>(d0 / (std::max)(1e-6, d0 + d1)), 0.0f, 1.0f);
                    const uint32_t w1 = static_cast<uint32_t>(beta * 256.0f + 0.5f);
                    const uint32_t w0 = 256u - w1;
                    interp_mad = ComputeRgb565InterpMad(scratch_rgb565_.data(),
                                                        buckets_[b0].rgb565.data(),
                                                        buckets_[b1].rgb565.data(),
                                                        w0,
                                                        w1,
                                                        tracked_roi_.w,
                                                        tracked_roi_.h);
                }
            }

            const float effective_mad = (std::min)(min_pop_mad, interp_mad);
            if (effective_mad > kResidualBreakMad) {
                // Prediction residual spiked (GIF changed/stopped or UI updated): break lock in 0ms!
                BreakLockKeepRoi();
                if (!IsEligibleRoi(clamped, fb_area)) {
                    Reset();
                    return;
                }
                UpdateTrackedRoiUnlocked(clamped, curr_bgra, fb_w, fb_h, stride_bytes, fb_area);
                ExtractTightRgb565(curr_bgra, stride_bytes, tracked_roi_, &scratch_rgb565_);
                ComputeSignatureFromRgb565(scratch_rgb565_.data(),
                                           tracked_roi_.w,
                                           tracked_roi_.h,
                                           &curr_dhash,
                                           curr_sketch.data());
                PushHistoryFrame(now_ms, scratch_rgb565_, curr_dhash, curr_sketch);
                last_observed_sketch_ = curr_sketch;
                has_last_observed_ = true;
                return;
            }

            // Ignore duplicate zero-change packets on an already-populated matched bucket so
            // frozen widgets neither skew the PLL nor extend last_valid_ms_ indefinitely.
            if (!pred_is_gap && has_last_observed_ && has_last_matched_bucket_ &&
                best_b == last_matched_bucket_ &&
                SketchMad(curr_sketch.data(), last_observed_sketch_.data()) < 1.0f &&
                min_pop_mad < 1.0f) {
                return;
            }
            last_observed_sketch_ = curr_sketch;
            has_last_observed_ = true;

            // Choose target bucket to write:
            // - Always fill b_pred when b_pred is an empty gap (unless it's a near-exact duplicate
            //   of an already-populated neighbor bucket that arrived with jitter).
            // - When b_pred is populated, refresh b_pred if it matches closely; otherwise refresh
            //   best_b so jitter doesn't overwrite an adjacent distinct frame.
            size_t write_b = b_pred;
            bool matched_existing_cleanly = false;
            if (pred_is_gap) {
                if (has_pop_candidate && best_mad < 1.0f && best_mad + 0.5f < interp_mad) {
                    write_b = best_b;
                    matched_existing_cleanly = true;
                } else {
                    write_b = b_pred;
                    matched_existing_cleanly = false;
                }
            } else {
                if (has_pop_candidate && best_b != b_pred &&
                    pred_bucket_mad > (std::min)(kResidualBreakMad, best_mad + 2.0f)) {
                    write_b = best_b;
                } else {
                    write_b = b_pred;
                }
                matched_existing_cleanly = has_pop_candidate && (best_mad <= kResidualBreakMad);
            }

            const bool was_populated = buckets_[write_b].populated;
            const uint64_t prev_seen_ms = buckets_[write_b].last_seen_ms;

            buckets_[write_b].rgb565 = scratch_rgb565_;
            buckets_[write_b].dhash = curr_dhash;
            buckets_[write_b].sketch = curr_sketch;
            buckets_[write_b].populated = true;
            if (!was_populated) {
                ++populated_buckets_;
                buckets_[write_b].last_seen_ms = now_ms;
            }

            // PLL period EMA (0.85/0.15) when cleanly matching a populated bucket across a wrap.
            if (matched_existing_cleanly && was_populated && prev_seen_ms > 0 && now_ms > prev_seen_ms) {
                const double dt_slot = static_cast<double>(now_ms - prev_seen_ms);
                const double wraps = std::round(dt_slot / period_ms_);
                if (wraps >= 1.0) {
                    const double implied_period = dt_slot / wraps;
                    if (implied_period >= kMinPeriodMs && implied_period <= kMaxPeriodMs &&
                        std::fabs(implied_period - period_ms_) <= 0.40 * period_ms_) {
                        period_ms_ = std::clamp(
                            0.85 * period_ms_ + 0.15 * implied_period, kMinPeriodMs, kMaxPeriodMs);
                    }
                    buckets_[write_b].last_seen_ms = now_ms;
                }
            } else if (was_populated) {
                buckets_[write_b].last_seen_ms = now_ms;
            }

            // Smooth PLL phase update (0.75 * predicted + 0.25 * observed along shortest unit arc).
            const double phi_now = PredictUnitPhaseAt(now_ms);
            const size_t phase_ref_b = matched_existing_cleanly ? best_b : write_b;
            const double phi_obs = static_cast<double>(phase_ref_b) / Bf;
            const double phase_err = ShortestUnitDelta(phi_obs, phi_now);
            const double smoothed_phi = Wrap01(phi_now + 0.25 * phase_err);

            anchor_time_ms_ = static_cast<double>(now_ms);
            anchor_phase_ = smoothed_phi;
            last_valid_ms_ = now_ms;
            last_matched_bucket_ = write_b;
            has_last_matched_bucket_ = true;
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

        // 2. Compact RGB565 Extraction + Perceptual Signature (64-bit dHash + 16-sample Luma Sketch):
        ExtractTightRgb565(curr_bgra, stride_bytes, tracked_roi_, &scratch_rgb565_);
        uint64_t curr_dhash = 0;
        std::array<uint8_t, 16> curr_sketch{};
        ComputeSignatureFromRgb565(
            scratch_rgb565_.data(), tracked_roi_.w, tracked_roi_.h, &curr_dhash, curr_sketch.data());

        if (hist_count_ > 0) {
            const FrameSample& last = HistoryAt(hist_count_ - 1u);
            if (SketchMad(curr_sketch.data(), last.sketch.data()) < 1.0f &&
                ComputeRgb565Mad(
                    scratch_rgb565_.data(), last.rgb565.data(), tracked_roi_.w, tracked_roi_.h) <
                    1.5f) {
                return;
            }
        }

        PushHistoryFrame(now_ms, scratch_rgb565_, curr_dhash, curr_sketch);
        last_observed_sketch_ = curr_sketch;
        has_last_observed_ = true;

        // 3. Time-Delta Drop-Resilient Cycle Closure Detection:
        TryDetectCycleClosure(now_ms);
    }

    bool SynthesizeAt(uint64_t now_ms,
                      AnimRoi* out_roi,
                      std::vector<uint8_t>* out_roi_bgra) const {
        if (!out_roi || !out_roi_bgra || !IsLocked(now_ms)) {
            return false;
        }
        const size_t B = bucket_count_;
        const size_t expected_pixels =
            static_cast<size_t>(tracked_roi_.w) * static_cast<size_t>(tracked_roi_.h);
        if (B < kMinPeriodFrames || populated_buckets_ == 0u || expected_pixels == 0u) {
            return false;
        }

        const double phi = PredictUnitPhaseAt(now_ms);
        const double u = phi * static_cast<double>(B);

        size_t b0 = 0;
        size_t b1 = 0;
        double d0 = 0.0;
        double d1 = 0.0;
        if (!FindBracketingBuckets(u, &b0, &d0, &b1, &d1)) {
            return false;
        }
        if (buckets_[b0].rgb565.size() != expected_pixels ||
            buckets_[b1].rgb565.size() != expected_pixels) {
            return false;
        }

        const float beta =
            (b0 == b1) ? 0.0f : std::clamp(static_cast<float>(d0 / (std::max)(1e-6, d0 + d1)), 0.0f, 1.0f);
        const uint32_t w1 = static_cast<uint32_t>(beta * 256.0f + 0.5f);
        const uint32_t w0 = 256u - w1;

        *out_roi = tracked_roi_;
        out_roi_bgra->resize(expected_pixels * 4u);
        uint8_t* dst = out_roi_bgra->data();
        const uint16_t* src0 = buckets_[b0].rgb565.data();
        const uint16_t* src1 = buckets_[b1].rgb565.data();

        if (w1 == 0u || b0 == b1) {
            DecodeRgb565ToBgra(src0, expected_pixels, dst);
            return true;
        }
        if (w1 >= 256u) {
            DecodeRgb565ToBgra(src1, expected_pixels, dst);
            return true;
        }

        for (size_t i = 0; i < expected_pixels; ++i) {
            const uint16_t p0 = src0[i];
            const uint16_t p1 = src1[i];

            const uint32_t r5_0 = (p0 >> 11) & 0x1Fu;
            const uint32_t g6_0 = (p0 >> 5) & 0x3Fu;
            const uint32_t b5_0 = p0 & 0x1Fu;
            const uint32_t r0 = (r5_0 << 3) | (r5_0 >> 2);
            const uint32_t g0 = (g6_0 << 2) | (g6_0 >> 4);
            const uint32_t b0_c = (b5_0 << 3) | (b5_0 >> 2);

            const uint32_t r5_1 = (p1 >> 11) & 0x1Fu;
            const uint32_t g6_1 = (p1 >> 5) & 0x3Fu;
            const uint32_t b5_1 = p1 & 0x1Fu;
            const uint32_t r1 = (r5_1 << 3) | (r5_1 >> 2);
            const uint32_t g1 = (g6_1 << 2) | (g6_1 >> 4);
            const uint32_t b1_c = (b5_1 << 3) | (b5_1 >> 2);

            const size_t off = i * 4u;
            dst[off + 0u] = static_cast<uint8_t>((b0_c * w0 + b1_c * w1 + 128u) >> 8);
            dst[off + 1u] = static_cast<uint8_t>((g0 * w0 + g1 * w1 + 128u) >> 8);
            dst[off + 2u] = static_cast<uint8_t>((r0 * w0 + r1 * w1 + 128u) >> 8);
            dst[off + 3u] = 0xFFu;
        }
        return true;
    }

    bool IsLocked(uint64_t now_ms) const noexcept {
        if (!locked_ || bucket_count_ < kMinPeriodFrames || populated_buckets_ == 0u ||
            period_ms_ < kMinPeriodMs) {
            return false;
        }
        if (now_ms < last_valid_ms_) {
            return false;
        }
        const double max_gap_ms = (std::max)(800.0, period_ms_ * 2.5);
        return static_cast<double>(now_ms - last_valid_ms_) <= max_gap_ms;
    }

    size_t locked_period_frames() const noexcept {
        return locked_ ? bucket_count_ : 0u;
    }

    double locked_period_ms() const noexcept {
        return locked_ ? period_ms_ : 0.0;
    }

    size_t memory_bytes() const noexcept {
        size_t total = scratch_rgb565_.size() * sizeof(uint16_t);
        for (const auto& entry : history_) {
            total += entry.rgb565.size() * sizeof(uint16_t);
        }
        for (const auto& bucket : buckets_) {
            if (bucket.populated) {
                total += bucket.rgb565.size() * sizeof(uint16_t);
            }
        }
        return total;
    }

    size_t populated_buckets() const noexcept {
        return locked_ ? populated_buckets_ : 0u;
    }

private:
    static constexpr uint32_t kClosureMaxHamming = 6u;
    static constexpr float kClosureMaxSketchMad = 5.5f;
    static constexpr float kClosureMaxLumaMad = 6.5f;
    static constexpr float kMinMotionSketchMad = 6.0f;
    static constexpr float kResidualBreakMad = 8.0f;

    struct FrameSample {
        uint64_t time_ms = 0;
        uint64_t dhash = 0;
        std::array<uint8_t, 16> sketch{};
        std::vector<uint16_t> rgb565;
    };

    struct PhaseBucket {
        bool populated = false;
        uint64_t last_seen_ms = 0;
        uint64_t dhash = 0;
        std::array<uint8_t, 16> sketch{};
        std::vector<uint16_t> rgb565;
    };

    static size_t MaxSlotsForRoi(uint32_t w, uint32_t h) noexcept {
        const size_t frame_bytes =
            static_cast<size_t>(w) * static_cast<size_t>(h) * sizeof(uint16_t);
        if (frame_bytes == 0u || kMaxMemoryBytes <= frame_bytes) {
            return 0u;
        }
        // Reserve one frame for scratch_rgb565_ and split remaining 64 MB budget between
        // history and phase buckets so combined memory NEVER exceeds kMaxMemoryBytes.
        const size_t usable_bytes = kMaxMemoryBytes - frame_bytes;
        return (std::min)(kMaxHistory, usable_bytes / (std::max<size_t>)(1u, frame_bytes * 2u));
    }

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

    static uint16_t BgraToRgb565(uint8_t b, uint8_t g, uint8_t r) noexcept {
        const uint16_t r5 = static_cast<uint16_t>((r >> 3) & 0x1Fu);
        const uint16_t g6 = static_cast<uint16_t>((g >> 2) & 0x3Fu);
        const uint16_t b5 = static_cast<uint16_t>((b >> 3) & 0x1Fu);
        return static_cast<uint16_t>((r5 << 11) | (g6 << 5) | b5);
    }

    static uint32_t Rgb565ToLuma(uint16_t p) noexcept {
        const uint32_t r5 = (p >> 11) & 0x1Fu;
        const uint32_t g6 = (p >> 5) & 0x3Fu;
        const uint32_t b5 = p & 0x1Fu;
        const uint32_t r = (r5 << 3) | (r5 >> 2);
        const uint32_t g = (g6 << 2) | (g6 >> 4);
        const uint32_t b = (b5 << 3) | (b5 >> 2);
        return (b * 29u + g * 150u + r * 77u + 128u) >> 8;
    }

    static void DecodeRgb565ToBgra(const uint16_t* src, size_t count, uint8_t* dst) noexcept {
        for (size_t i = 0; i < count; ++i) {
            const uint16_t p = src[i];
            const uint32_t r5 = (p >> 11) & 0x1Fu;
            const uint32_t g6 = (p >> 5) & 0x3Fu;
            const uint32_t b5 = p & 0x1Fu;
            const size_t off = i * 4u;
            dst[off + 0u] = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
            dst[off + 1u] = static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
            dst[off + 2u] = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
            dst[off + 3u] = 0xFFu;
        }
    }

    static float SketchMad(const uint8_t a[16], const uint8_t b[16]) noexcept {
        uint32_t sum = 0;
        for (size_t i = 0; i < 16u; ++i) {
            sum += static_cast<uint32_t>(
                std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])));
        }
        return static_cast<float>(sum) * (1.0f / 16.0f);
    }

    static float SampleBoxLumaRgb565(const uint16_t* tight_rgb565,
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
            const uint16_t* row = tight_rgb565 + static_cast<size_t>(y) * w;
            for (uint32_t x = x0; x < x1; x += step_x) {
                sum += Rgb565ToLuma(row[x]);
                ++count;
            }
        }
        return (count > 0) ? (static_cast<float>(sum) / static_cast<float>(count)) : 0.0f;
    }

    static void ComputeSignatureFromRgb565(const uint16_t* tight_rgb565,
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
                luma_grid[gy][gx] = SampleBoxLumaRgb565(tight_rgb565, w, x0, y0, x1, y1, 4u);
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
                const float cell_luma = SampleBoxLumaRgb565(tight_rgb565, w, x0, y0, x1, y1, 8u);
                const int q = std::clamp(static_cast<int>(cell_luma + 0.5f), 0, 255);
                out_sketch[sy * 4u + sx] = static_cast<uint8_t>(q);
            }
        }
    }

    static float ComputeRgb565LumaMad(const uint16_t* a,
                                      const uint16_t* b,
                                      uint32_t w,
                                      uint32_t h) noexcept {
        if (!a || !b || w == 0 || h == 0) {
            return 255.0f;
        }
        const uint32_t step_x = (std::max)(1u, w / 64u);
        const uint32_t step_y = (std::max)(1u, h / 64u);
        uint64_t sum_abs = 0;
        uint64_t samples = 0;
        for (uint32_t y = 0; y < h; y += step_y) {
            const size_t row_off = static_cast<size_t>(y) * w;
            for (uint32_t x = 0; x < w; x += step_x) {
                const uint16_t pa = a[row_off + x];
                const uint16_t pb = b[row_off + x];
                if (pa != pb) {
                    const int la = static_cast<int>(Rgb565ToLuma(pa));
                    const int lb = static_cast<int>(Rgb565ToLuma(pb));
                    sum_abs += static_cast<uint64_t>(std::abs(la - lb));
                }
                ++samples;
            }
        }
        return (samples > 0) ? (static_cast<float>(sum_abs) / static_cast<float>(samples)) : 255.0f;
    }

    static float ComputeRgb565Mad(const uint16_t* a,
                                  const uint16_t* b,
                                  uint32_t w,
                                  uint32_t h) noexcept {
        if (!a || !b || w == 0 || h == 0) {
            return 255.0f;
        }
        const uint32_t step_x = (std::max)(1u, w / 64u);
        const uint32_t step_y = (std::max)(1u, h / 64u);
        uint64_t sum_abs = 0;
        uint64_t samples = 0;
        for (uint32_t y = 0; y < h; y += step_y) {
            const size_t row_off = static_cast<size_t>(y) * w;
            for (uint32_t x = 0; x < w; x += step_x) {
                const uint16_t pa = a[row_off + x];
                const uint16_t pb = b[row_off + x];
                if (pa != pb) {
                    const int r5a = (pa >> 11) & 0x1F;
                    const int g6a = (pa >> 5) & 0x3F;
                    const int b5a = pa & 0x1F;
                    const int ra = (r5a << 3) | (r5a >> 2);
                    const int ga = (g6a << 2) | (g6a >> 4);
                    const int ba = (b5a << 3) | (b5a >> 2);

                    const int r5b = (pb >> 11) & 0x1F;
                    const int g6b = (pb >> 5) & 0x3F;
                    const int b5b = pb & 0x1F;
                    const int rb = (r5b << 3) | (r5b >> 2);
                    const int gb = (g6b << 2) | (g6b >> 4);
                    const int bb = (b5b << 3) | (b5b >> 2);

                    sum_abs += static_cast<uint64_t>(
                        std::abs(ra - rb) + std::abs(ga - gb) + std::abs(ba - bb));
                }
                ++samples;
            }
        }
        return (samples > 0)
                   ? (static_cast<float>(sum_abs) / static_cast<float>(samples * 3u))
                   : 255.0f;
    }

    static float ComputeRgb565InterpMad(const uint16_t* curr,
                                        const uint16_t* b0,
                                        const uint16_t* b1,
                                        uint32_t w0,
                                        uint32_t w1,
                                        uint32_t w,
                                        uint32_t h) noexcept {
        if (!curr || !b0 || !b1 || w == 0 || h == 0) {
            return 255.0f;
        }
        const uint32_t step_x = (std::max)(1u, w / 64u);
        const uint32_t step_y = (std::max)(1u, h / 64u);
        uint64_t sum_abs = 0;
        uint64_t samples = 0;
        for (uint32_t y = 0; y < h; y += step_y) {
            const size_t row_off = static_cast<size_t>(y) * w;
            for (uint32_t x = 0; x < w; x += step_x) {
                const uint16_t pc = curr[row_off + x];
                const uint16_t p0 = b0[row_off + x];
                const uint16_t p1 = b1[row_off + x];

                const int r5c = (pc >> 11) & 0x1F;
                const int g6c = (pc >> 5) & 0x3F;
                const int b5c = pc & 0x1F;
                const int rc = (r5c << 3) | (r5c >> 2);
                const int gc = (g6c << 2) | (g6c >> 4);
                const int bc = (b5c << 3) | (b5c >> 2);

                const uint32_t r5_0 = (p0 >> 11) & 0x1Fu;
                const uint32_t g6_0 = (p0 >> 5) & 0x3Fu;
                const uint32_t b5_0 = p0 & 0x1Fu;
                const uint32_t r0 = (r5_0 << 3) | (r5_0 >> 2);
                const uint32_t g0 = (g6_0 << 2) | (g6_0 >> 4);
                const uint32_t b0_c = (b5_0 << 3) | (b5_0 >> 2);

                const uint32_t r5_1 = (p1 >> 11) & 0x1Fu;
                const uint32_t g6_1 = (p1 >> 5) & 0x3Fu;
                const uint32_t b5_1 = p1 & 0x1Fu;
                const uint32_t r1 = (r5_1 << 3) | (r5_1 >> 2);
                const uint32_t g1 = (g6_1 << 2) | (g6_1 >> 4);
                const uint32_t b1_c = (b5_1 << 3) | (b5_1 >> 2);

                const int rp = static_cast<int>((r0 * w0 + r1 * w1 + 128u) >> 8);
                const int gp = static_cast<int>((g0 * w0 + g1 * w1 + 128u) >> 8);
                const int bp = static_cast<int>((b0_c * w0 + b1_c * w1 + 128u) >> 8);

                sum_abs += static_cast<uint64_t>(
                    std::abs(rc - rp) + std::abs(gc - gp) + std::abs(bc - bp));
                ++samples;
            }
        }
        return (samples > 0)
                   ? (static_cast<float>(sum_abs) / static_cast<float>(samples * 3u))
                   : 255.0f;
    }

    static void ExtractTightRgb565(const uint8_t* fb_bgra,
                                   uint32_t stride_bytes,
                                   const AnimRoi& roi,
                                   std::vector<uint16_t>* out_tight) {
        const size_t total_pixels = static_cast<size_t>(roi.w) * static_cast<size_t>(roi.h);
        out_tight->resize(total_pixels);
        uint16_t* dst = out_tight->data();
        for (uint32_t r = 0; r < roi.h; ++r) {
            const uint8_t* src_row = fb_bgra + static_cast<size_t>(roi.y + r) * stride_bytes +
                                     static_cast<size_t>(roi.x) * 4u;
            uint16_t* dst_row = dst + static_cast<size_t>(r) * roi.w;
            for (uint32_t c = 0; c < roi.w; ++c) {
                const size_t off = static_cast<size_t>(c) * 4u;
                dst_row[c] = BgraToRgb565(src_row[off + 0u], src_row[off + 1u], src_row[off + 2u]);
            }
        }
    }

    static double Wrap01(double x) noexcept {
        double w = x - std::floor(x);
        if (w < 0.0 || w >= 1.0) {
            w = 0.0;
        }
        return w;
    }

    static double ShortestUnitDelta(double target, double current) noexcept {
        double d = std::fmod(target - current, 1.0);
        if (d > 0.5) {
            d -= 1.0;
        } else if (d < -0.5) {
            d += 1.0;
        }
        return d;
    }

    static size_t PhaseToBucket(double phi01, size_t bucket_count) noexcept {
        if (bucket_count == 0u) {
            return 0u;
        }
        const double Bf = static_cast<double>(bucket_count);
        const double centered = Wrap01(phi01 + 0.5 / Bf);
        return static_cast<size_t>(std::floor(centered * Bf)) % bucket_count;
    }

    double PredictUnitPhaseAt(uint64_t now_ms) const noexcept {
        if (period_ms_ <= 0.0) {
            return 0.0;
        }
        const double elapsed = static_cast<double>(now_ms) - anchor_time_ms_;
        return Wrap01(elapsed / period_ms_ + anchor_phase_);
    }

    bool FindBracketingBuckets(double u,
                               size_t* out_b0,
                               double* out_d0,
                               size_t* out_b1,
                               double* out_d1) const noexcept {
        const size_t B = bucket_count_;
        if (B == 0u || buckets_.size() < B || populated_buckets_ == 0u) {
            return false;
        }
        const double Bf = static_cast<double>(B);
        double u_mod = u - std::floor(u / Bf) * Bf;
        if (u_mod < 0.0 || u_mod >= Bf) {
            u_mod = 0.0;
        }
        const size_t base = static_cast<size_t>(std::floor(u_mod)) % B;

        bool found0 = false;
        size_t b0 = 0;
        double d0 = 0.0;
        for (size_t step = 0u; step < B; ++step) {
            const size_t idx = (base + B - step) % B;
            if (buckets_[idx].populated) {
                b0 = idx;
                const double pos = static_cast<double>(idx);
                d0 = (pos <= u_mod) ? (u_mod - pos) : (u_mod + Bf - pos);
                found0 = true;
                break;
            }
        }
        if (!found0) {
            return false;
        }

        bool found1 = false;
        size_t b1 = b0;
        double d1 = 1.0;
        const size_t start_after = (base + 1u) % B;
        for (size_t step = 0u; step < B; ++step) {
            const size_t idx = (start_after + step) % B;
            if (buckets_[idx].populated) {
                b1 = idx;
                const double pos = static_cast<double>(idx);
                d1 = (pos > u_mod) ? (pos - u_mod) : (pos + Bf - u_mod);
                found1 = true;
                break;
            }
        }
        if (!found1) {
            b1 = b0;
            d1 = 1.0;
        }

        *out_b0 = b0;
        *out_d0 = d0;
        *out_b1 = b1;
        *out_d1 = d1;
        return true;
    }

    void ClearHistory() noexcept {
        std::vector<FrameSample>().swap(history_);
        hist_head_ = 0;
        hist_count_ = 0;
        history_cap_ = 0;
    }

    void ClearBuckets() noexcept {
        std::vector<PhaseBucket>().swap(buckets_);
        bucket_count_ = 0;
        populated_buckets_ = 0;
    }

    void BreakLockKeepRoi() noexcept {
        locked_ = false;
        ClearHistory();
        ClearBuckets();
        if (has_tracked_roi_) {
            history_cap_ = MaxSlotsForRoi(tracked_roi_.w, tracked_roi_.h);
        }
        period_ms_ = 0.0;
        anchor_time_ms_ = 0.0;
        anchor_phase_ = 0.0;
        last_valid_ms_ = 0;
        has_last_observed_ = false;
        has_last_matched_bucket_ = false;
        last_matched_bucket_ = 0;
    }

    FrameSample& HistoryAt(size_t logical_idx) noexcept {
        return history_[(hist_head_ + logical_idx) % history_.size()];
    }

    const FrameSample& HistoryAt(size_t logical_idx) const noexcept {
        return history_[(hist_head_ + logical_idx) % history_.size()];
    }

    void PushHistoryFrame(uint64_t now_ms,
                          const std::vector<uint16_t>& tight_rgb565,
                          uint64_t dhash,
                          const std::array<uint8_t, 16>& sketch) {
        if (history_cap_ == 0u) {
            return;
        }
        if (history_.size() < history_cap_) {
            FrameSample sample;
            sample.time_ms = now_ms;
            sample.dhash = dhash;
            sample.sketch = sketch;
            sample.rgb565 = tight_rgb565;
            history_.push_back(std::move(sample));
            hist_count_ = history_.size();
            return;
        }
        FrameSample& dst = history_[hist_head_];
        hist_head_ = (hist_head_ + 1u) % history_cap_;
        dst.time_ms = now_ms;
        dst.dhash = dhash;
        dst.sketch = sketch;
        dst.rgb565 = tight_rgb565;
    }

    void RemapHistoryToExpandedRoi(const AnimRoi& old_roi,
                                   const AnimRoi& new_roi,
                                   const uint8_t* curr_fb_bgra,
                                   uint32_t stride_bytes) {
        const size_t new_cap = MaxSlotsForRoi(new_roi.w, new_roi.h);
        if (new_cap == 0u) {
            ClearHistory();
            return;
        }

        std::vector<uint16_t> base_new;
        ExtractTightRgb565(curr_fb_bgra, stride_bytes, new_roi, &base_new);

        const uint32_t off_x = old_roi.x - new_roi.x;
        const uint32_t off_y = old_roi.y - new_roi.y;
        const size_t keep = (std::min)(hist_count_, new_cap);
        const size_t start_k = hist_count_ - keep;

        std::vector<FrameSample> remapped_hist;
        remapped_hist.reserve(keep);

        // Release discarded older entries first so peak memory stays strictly within budget.
        for (size_t k = 0; k < start_k; ++k) {
            std::vector<uint16_t>().swap(HistoryAt(k).rgb565);
        }

        for (size_t k = start_k; k < hist_count_; ++k) {
            FrameSample& entry = HistoryAt(k);
            std::vector<uint16_t> expanded = base_new;
            for (uint32_t r = 0; r < old_roi.h; ++r) {
                std::memcpy(expanded.data() + static_cast<size_t>(off_y + r) * new_roi.w + off_x,
                            entry.rgb565.data() + static_cast<size_t>(r) * old_roi.w,
                            static_cast<size_t>(old_roi.w) * sizeof(uint16_t));
            }
            FrameSample updated;
            updated.time_ms = entry.time_ms;
            updated.rgb565 = std::move(expanded);
            ComputeSignatureFromRgb565(
                updated.rgb565.data(), new_roi.w, new_roi.h, &updated.dhash, updated.sketch.data());
            std::vector<uint16_t>().swap(entry.rgb565);
            remapped_hist.push_back(std::move(updated));
        }

        history_.swap(remapped_hist);
        hist_head_ = 0;
        hist_count_ = history_.size();
        history_cap_ = new_cap;
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
            history_cap_ = MaxSlotsForRoi(tracked_roi_.w, tracked_roi_.h);
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
                if (history_cap_ == 0u) {
                    history_cap_ = MaxSlotsForRoi(tracked_roi_.w, tracked_roi_.h);
                }
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
        ClearHistory();
        has_last_observed_ = false;
        tracked_roi_ = clamped;
        has_tracked_roi_ = true;
        fb_w_ = fb_w;
        fb_h_ = fb_h;
        history_cap_ = MaxSlotsForRoi(tracked_roi_.w, tracked_roi_.h);
    }

    bool FramesMatchClosure(const FrameSample& a, const FrameSample& b) const noexcept {
        if (Popcount64(a.dhash ^ b.dhash) > kClosureMaxHamming) {
            return false;
        }
        if (SketchMad(a.sketch.data(), b.sketch.data()) > kClosureMaxSketchMad) {
            return false;
        }
        return ComputeRgb565LumaMad(
                   a.rgb565.data(), b.rgb565.data(), tracked_roi_.w, tracked_roi_.h) <=
               kClosureMaxLumaMad;
    }

    double EstimateStepMs(size_t j, size_t i, double period_ms) const {
        std::vector<double> steps;
        steps.reserve(i - j);
        for (size_t m = j + 1u; m <= i; ++m) {
            const uint64_t t_prev = HistoryAt(m - 1u).time_ms;
            const uint64_t t_curr = HistoryAt(m).time_ms;
            if (t_curr > t_prev) {
                steps.push_back(static_cast<double>(t_curr - t_prev));
            }
        }
        if (steps.empty()) {
            return (std::max)(8.0, period_ms / static_cast<double>((std::max)(size_t{1u}, i - j)));
        }

        std::sort(steps.begin(), steps.end());
        const size_t n = steps.size();
        const size_t lo_count = (std::max)(size_t{1u}, (n * 3u + 3u) / 4u);
        double lo_sum = 0.0;
        for (size_t k = 0; k < lo_count; ++k) {
            lo_sum += steps[k];
        }
        const double lo_mean = lo_sum / static_cast<double>(lo_count);
        // Exclude drop-gap outliers (> 1.6x lower-75% mean) and average the remaining normal
        // intervals so both short jittery cycles (no drops) and long drop-heavy cycles estimate
        // true frame spacing without bias from dropped-frame gaps.
        const double max_normal = lo_mean * 1.60;
        double sum = 0.0;
        size_t count = 0;
        for (double s : steps) {
            if (s <= max_normal) {
                sum += s;
                ++count;
            }
        }
        const double avg = (count > 0u) ? (sum / static_cast<double>(count)) : lo_mean;
        return (std::max)(8.0, avg);
    }

    void TryDetectCycleClosure(uint64_t now_ms) {
        if (hist_count_ < kMinPeriodFrames + 2u) {
            return;
        }
        const size_t max_bucket_slots =
            (std::min)(kMaxBuckets, MaxSlotsForRoi(tracked_roi_.w, tracked_roi_.h));
        if (max_bucket_slots < kMinPeriodFrames) {
            return;
        }

        const size_t i = hist_count_ - 1u;
        const FrameSample& curr = HistoryAt(i);
        const FrameSample& prev = HistoryAt(i - 1u);

        // Scan j downwards from i - 3 so shortest valid dt (fundamental period) is checked first.
        for (size_t j = i - kMinPeriodFrames + 1u; j-- > 0u;) {
            const FrameSample& ref_j = HistoryAt(j);
            if (curr.time_ms <= ref_j.time_ms) {
                continue;
            }
            const double dt = static_cast<double>(curr.time_ms - ref_j.time_ms);
            if (dt < kMinPeriodMs) {
                continue;
            }
            if (dt > kMaxPeriodMs) {
                break;
            }

            if (!FramesMatchClosure(curr, ref_j)) {
                continue;
            }

            // 2-step temporal consistency: find any history entry k < j whose timestamp is within
            // tol = max(75, dt * 0.10) ms of (t_{i-1} - dt) and perceptually matches prev.
            const double target_t = static_cast<double>(prev.time_ms) - dt;
            const double tol = (std::max)(75.0, dt * 0.10);
            bool found_prev_match = false;
            size_t best_k = 0;
            double best_k_err = 1e18;
            for (size_t k = j; k-- > 0u;) {
                const FrameSample& ref_k = HistoryAt(k);
                const double tk = static_cast<double>(ref_k.time_ms);
                const double err = std::fabs(tk - target_t);
                if (tk < target_t - tol) {
                    break;
                }
                if (err > tol) {
                    continue;
                }
                if (FramesMatchClosure(prev, ref_k) && err < best_k_err) {
                    best_k_err = err;
                    best_k = k;
                    found_prev_match = true;
                }
            }
            if (!found_prev_match) {
                continue;
            }

            // Motion gate: at least one intermediate frame in (j .. i) must differ from curr
            // by SketchMad >= 6.0f so static or low-noise regions never lock.
            bool has_motion = false;
            for (size_t m = j + 1u; m < i; ++m) {
                if (SketchMad(HistoryAt(m).sketch.data(), curr.sketch.data()) >=
                    kMinMotionSketchMad) {
                    has_motion = true;
                    break;
                }
            }
            if (!has_motion) {
                continue;
            }

            // Refine period estimate with prev->ref_k if consistent.
            double est_period = dt;
            const FrameSample& ref_k = HistoryAt(best_k);
            if (prev.time_ms > ref_k.time_ms) {
                const double prev_dt = static_cast<double>(prev.time_ms - ref_k.time_ms);
                if (prev_dt >= kMinPeriodMs && prev_dt <= kMaxPeriodMs &&
                    std::fabs(prev_dt - dt) <= tol) {
                    est_period = std::clamp(0.5 * (dt + prev_dt), kMinPeriodMs, kMaxPeriodMs);
                }
            }

            // Size normalized phase-bucket table strictly within memory budget.
            const double step_ms = EstimateStepMs(j, i, est_period);
            const size_t raw_buckets = static_cast<size_t>(std::llround(est_period / step_ms));
            const size_t B = std::clamp(raw_buckets, kMinPeriodFrames, max_bucket_slots);

            buckets_.clear();
            buckets_.resize(B);
            bucket_count_ = B;
            populated_buckets_ = 0u;

            period_ms_ = est_period;
            anchor_time_ms_ = static_cast<double>(curr.time_ms);
            anchor_phase_ = 0.0;

            // Populate buckets from history frames in [t_i - T, t_i] (and full cycle (j .. i]).
            for (size_t m = j + 1u; m <= i; ++m) {
                FrameSample& src = HistoryAt(m);
                const double phi =
                    Wrap01((static_cast<double>(src.time_ms) - anchor_time_ms_) / period_ms_);
                const size_t b = PhaseToBucket(phi, B);
                PhaseBucket& dst = buckets_[b];
                if (!dst.populated) {
                    ++populated_buckets_;
                }
                dst.populated = true;
                dst.last_seen_ms = src.time_ms;
                dst.dhash = src.dhash;
                dst.sketch = src.sketch;
                dst.rgb565 = std::move(src.rgb565);
            }

            // Release unlocked history buffer memory now that phase buckets are populated.
            ClearHistory();

            last_valid_ms_ = now_ms;
            last_matched_bucket_ = 0u;
            has_last_matched_bucket_ = true;
            locked_ = (populated_buckets_ > 0u);
            return;
        }
    }

    bool locked_ = false;
    bool has_tracked_roi_ = false;
    AnimRoi tracked_roi_{};
    uint32_t fb_w_ = 0;
    uint32_t fb_h_ = 0;

    std::vector<FrameSample> history_;
    size_t hist_head_ = 0;
    size_t hist_count_ = 0;
    size_t history_cap_ = 0;

    std::vector<PhaseBucket> buckets_;
    size_t bucket_count_ = 0;
    size_t populated_buckets_ = 0;

    std::vector<uint16_t> scratch_rgb565_;

    double period_ms_ = 0.0;
    double anchor_time_ms_ = 0.0;
    double anchor_phase_ = 0.0;
    uint64_t last_valid_ms_ = 0;

    std::array<uint8_t, 16> last_observed_sketch_{};
    bool has_last_observed_ = false;
    size_t last_matched_bucket_ = 0;
    bool has_last_matched_bucket_ = false;
};

}  // namespace mac_mirror
