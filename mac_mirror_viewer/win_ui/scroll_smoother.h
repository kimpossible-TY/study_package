#pragma once

#include <cstdint>

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

}  // namespace mac_mirror
