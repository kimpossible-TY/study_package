#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace mac_mirror {

// Converts raw Win32 WM_MOUSEWHEEL / WM_MOUSEHWHEEL deltas (where one standard
// mouse detent is 120, and precision touchpads / smooth wheels emit small
// fractional deltas like 10..40) into responsive, direction-safe scroll steps.
class ScrollWheelAccumulator {
public:
    static constexpr int kStepDelta = 40;        // WHEEL_DELTA (120) / 3
    static constexpr uint64_t kIdleResetMs = 250u;

    int Accumulate(int delta, uint64_t now_ms) noexcept {
        if (delta == 0) {
            return 0;
        }
        if (has_last_event_ && now_ms >= last_event_ms_ &&
            (now_ms - last_event_ms_) >= kIdleResetMs) {
            accum_ = 0;
        }
        if ((accum_ > 0 && delta < 0) || (accum_ < 0 && delta > 0)) {
            accum_ = 0;
        }
        has_last_event_ = true;
        last_event_ms_ = now_ms;

        accum_ += delta;
        const int steps = accum_ / kStepDelta;
        if (steps != 0) {
            accum_ -= steps * kStepDelta;
        }
        return steps;
    }

    void Reset() noexcept {
        accum_ = 0;
        has_last_event_ = false;
        last_event_ms_ = 0;
    }

    int remainder() const noexcept { return accum_; }

private:
    int accum_ = 0;
    bool has_last_event_ = false;
    uint64_t last_event_ms_ = 0;
};

struct ScrollRegion {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t w = 0;
    uint32_t h = 0;
};

struct ScrollVisualState {
    bool active = false;
    ScrollRegion region{};
    float offset_y = 0.0f;  // Framebuffer-pixel vertical glide offset decaying smoothly to 0
};

// Wheel-scroll-only pure 2D vertical translation interpolator.
// Detects high-confidence vertical translation (dy) inside the changed dirty region
// shortly after a vertical wheel event, and smoothly decays the visual offset at
// 60/120Hz without blurring text or triggering on non-scroll animations (e.g. Stage Manager).
class ScrollMotionInterpolator {
public:
    static constexpr uint64_t kWheelActiveWindowMs = 450u;
    static constexpr uint32_t kMinRegionWidth = 160u;
    static constexpr uint32_t kMinRegionHeight = 120u;
    static constexpr int kMinScrollShiftPx = 4;
    static constexpr int kMaxScrollShiftPx = 280;
    static constexpr float kDecayTauMs = 32.0f;
    static constexpr float kSettleThresholdPx = 0.75f;

    void NotifyVerticalWheel(uint64_t now_ms, int steps_vertical) noexcept {
        if (steps_vertical == 0) return;
        has_wheel_ = true;
        last_wheel_ms_ = now_ms;
    }

    bool IsWheelActive(uint64_t now_ms) const noexcept {
        return has_wheel_ && now_ms >= last_wheel_ms_ &&
               (now_ms - last_wheel_ms_) <= kWheelActiveWindowMs;
    }

    // Detects vertical pixel translation `dy` where `curr[y]` matches `prev[y + dy]`.
    // Positive `dy`: content moved UP (scrolling down). Negative `dy`: content moved DOWN.
    // Returns 0 if the region is not a high-confidence pure vertical translation.
    static int DetectVerticalTranslation(const uint8_t* prev_bgra,
                                         const uint8_t* curr_bgra,
                                         uint32_t fb_w,
                                         uint32_t fb_h,
                                         uint32_t stride_bytes,
                                         const ScrollRegion& region) noexcept {
        if (!prev_bgra || !curr_bgra || fb_w == 0 || fb_h == 0 || stride_bytes < fb_w * 4u) {
            return 0;
        }
        if (region.x >= fb_w || region.y >= fb_h) return 0;
        const uint32_t rx = region.x;
        const uint32_t ry = region.y;
        const uint32_t rw = std::min(region.w, fb_w - rx);
        const uint32_t rh = std::min(region.h, fb_h - ry);
        if (rw < kMinRegionWidth || rh < kMinRegionHeight) {
            return 0;
        }

        // Sample the central 2/3 horizontally to avoid scrollbars/gutter borders.
        const uint32_t x_start = rx + rw / 6u;
        const uint32_t x_end = rx + rw - rw / 6u;
        if (x_end <= x_start + 16u) return 0;

        // Clamp search range so at least half the region height overlaps.
        const int max_dy = std::min<int>(kMaxScrollShiftPx, static_cast<int>(rh / 3u));
        if (max_dy < kMinScrollShiftPx) return 0;

        // Inner vertical band that stays strictly inside [ry, ry + rh) for all |dy| <= max_dy.
        const uint32_t y_min = ry + static_cast<uint32_t>(max_dy);
        const uint32_t y_max = ry + rh - static_cast<uint32_t>(max_dy);
        if (y_max <= y_min + 8u) return 0;

        constexpr uint32_t kNumRows = 16u;
        const uint32_t y_span = y_max - y_min;

        auto compute_mad = [&](int dy, uint32_t x_step) -> float {
            uint64_t sum_abs = 0;
            uint64_t samples = 0;
            for (uint32_t r = 0; r < kNumRows; ++r) {
                const uint32_t cy = y_min + (r * y_span) / kNumRows;
                const uint32_t py = static_cast<uint32_t>(static_cast<int>(cy) + dy);
                const uint8_t* crow = curr_bgra + static_cast<size_t>(cy) * stride_bytes;
                const uint8_t* prow = prev_bgra + static_cast<size_t>(py) * stride_bytes;
                for (uint32_t x = x_start; x < x_end; x += x_step) {
                    const size_t off = static_cast<size_t>(x) * 4u;
                    // Fast integer luma approximation: (B + 2*G + R) >> 2
                    const int cl = (static_cast<int>(crow[off]) +
                                    static_cast<int>(crow[off + 1]) * 2 +
                                    static_cast<int>(crow[off + 2])) >> 2;
                    const int pl = (static_cast<int>(prow[off]) +
                                    static_cast<int>(prow[off + 1]) * 2 +
                                    static_cast<int>(prow[off + 2])) >> 2;
                    sum_abs += static_cast<uint64_t>(std::abs(cl - pl));
                    ++samples;
                }
            }
            return (samples > 0) ? (static_cast<float>(sum_abs) / static_cast<float>(samples)) : 255.0f;
        };

        const uint32_t coarse_x_step = std::max<uint32_t>(4u, (x_end - x_start) / 32u);
        const uint32_t fine_x_step = std::max<uint32_t>(2u, (x_end - x_start) / 64u);

        const float mad_zero = compute_mad(0, fine_x_step);
        if (mad_zero < 8.0f) {
            return 0;  // Region barely changed (or is blank/uniform)
        }

        // Coarse pass (step 4px)
        int best_coarse_dy = 0;
        float best_coarse_mad = 255.0f;
        for (int dy = -max_dy; dy <= max_dy; dy += 4) {
            if (std::abs(dy) < kMinScrollShiftPx) continue;
            const float mad = compute_mad(dy, coarse_x_step);
            if (mad < best_coarse_mad) {
                best_coarse_mad = mad;
                best_coarse_dy = dy;
            }
        }

        if (best_coarse_dy == 0 || best_coarse_mad > 12.0f) {
            return 0;
        }

        // Fine pass (1px refinement around best coarse candidate)
        int best_dy = best_coarse_dy;
        float best_mad = 255.0f;
        const int fine_lo = std::max(-max_dy, best_coarse_dy - 3);
        const int fine_hi = std::min(max_dy, best_coarse_dy + 3);
        for (int dy = fine_lo; dy <= fine_hi; ++dy) {
            if (std::abs(dy) < kMinScrollShiftPx) continue;
            const float mad = compute_mad(dy, fine_x_step);
            if (mad < best_mad) {
                best_mad = mad;
                best_dy = dy;
            }
        }

        // Strict confidence gate: reject scaling, 3D perspective (Stage Manager), and tab switches.
        if (best_mad > 3.5f || best_mad > mad_zero * 0.25f) {
            return 0;
        }
        return best_dy;
    }

    // Processes an incoming framebuffer update. Returns true if smooth scroll interpolation activated.
    bool OnFrameUpdated(uint64_t now_ms,
                        const uint8_t* prev_bgra,
                        const uint8_t* curr_bgra,
                        uint32_t fb_w,
                        uint32_t fb_h,
                        uint32_t stride_bytes,
                        const ScrollRegion& dirty_bounds) noexcept {
        if (!IsWheelActive(now_ms)) {
            if (dirty_bounds.w >= kMinRegionWidth && dirty_bounds.h >= kMinRegionHeight) {
                ResetAnimation();
            }
            return false;
        }

        Advance(now_ms);
        const int dy = DetectVerticalTranslation(
            prev_bgra, curr_bgra, fb_w, fb_h, stride_bytes, dirty_bounds);
        if (dy == 0) {
            if (dirty_bounds.w >= kMinRegionWidth && dirty_bounds.h >= kMinRegionHeight) {
                ResetAnimation();
            }
            return false;
        }

        float next_offset = static_cast<float>(dy);
        if (state_.active) {
            next_offset += state_.offset_y;
            next_offset = std::clamp(
                next_offset,
                -static_cast<float>(kMaxScrollShiftPx),
                static_cast<float>(kMaxScrollShiftPx));
        }

        state_.active = true;
        state_.region = dirty_bounds;
        state_.offset_y = next_offset;
        last_tick_ms_ = now_ms;
        return true;
    }

    // Advances the exponential ease-out decay. Returns true while still animating (caller should redraw).
    bool Advance(uint64_t now_ms) noexcept {
        if (!state_.active) {
            return false;
        }
        if (now_ms > last_tick_ms_) {
            const float dt_ms = static_cast<float>(std::min<uint64_t>(now_ms - last_tick_ms_, 100u));
            state_.offset_y *= std::exp(-dt_ms / kDecayTauMs);
            last_tick_ms_ = now_ms;
        }
        if (std::fabs(state_.offset_y) < kSettleThresholdPx) {
            state_.active = false;
            state_.offset_y = 0.0f;
            return false;
        }
        return true;
    }

    void ResetAnimation() noexcept {
        state_.active = false;
        state_.offset_y = 0.0f;
    }

    void Reset() noexcept {
        ResetAnimation();
        has_wheel_ = false;
        last_wheel_ms_ = 0;
        last_tick_ms_ = 0;
    }

    const ScrollVisualState& state() const noexcept { return state_; }
    bool IsAnimating() const noexcept { return state_.active; }

private:
    bool has_wheel_ = false;
    uint64_t last_wheel_ms_ = 0;
    uint64_t last_tick_ms_ = 0;
    ScrollVisualState state_{};
};

}  // namespace mac_mirror
