#pragma once

#include "../include/rfb_session.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>

namespace mac_mirror {

struct PointerSample {
    uint16_t x = 0;
    uint16_t y = 0;
    uint8_t button_mask = 0;
};

struct WheelStep {
    uint16_t x = 0;
    uint16_t y = 0;
    int8_t dv = 0;  // -1, 0, or +1 vertical wheel step
    int8_t dh = 0;  // -1, 0, or +1 horizontal wheel step
    uint8_t button_mask = 0;
};

struct QualityDecision {
    uint8_t target_bpp = 32;            // 16 (RGB565) or 32 (BGRX8888)
    bool format_changed = false;        // True if target_bpp != currently applied bpp
    bool request_full_refresh = false;  // True when sharpening 16 -> 32 or explicit refresh
};

// Thread-compatible controller (external mutex or single-mutex synchronization) managing:
// - Runtime QualityMode (Adaptive, Sharp, Turbo)
// - 250ms active-burst -> idle FullColor transition
// - 6ms coalescing pointer rate-limiter (zero click loss)
// - Sliding-window FPS & FramebufferUpdate RTT (ms) telemetry
class AdaptiveQualityController {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    static constexpr auto kBurstHoldDuration = std::chrono::milliseconds(250);
    static constexpr auto kRapidMoveWindow = std::chrono::milliseconds(50);
    static constexpr auto kPointerMinInterval = std::chrono::milliseconds(6);
    static constexpr auto kWheelStepInterval = std::chrono::milliseconds(12);
    static constexpr int32_t kMaxQueuedWheelSteps = 24;
    static constexpr size_t kRapidMoveThreshold = 3;

    explicit AdaptiveQualityController(QualityMode initial_mode = QualityMode::Adaptive);

    void SetQualityMode(QualityMode mode, TimePoint now);
    QualityMode GetQualityMode() const noexcept { return mode_; }

    void RequestFullRefresh() noexcept { explicit_full_refresh_ = true; }

    // Records pointer input and applies 6ms motion coalescing (clicks/releases never delayed).
    // Returns a PointerSample if one should be transmitted immediately on the wire.
    std::optional<PointerSample> OnPointerInput(TimePoint now,
                                                uint16_t x,
                                                uint16_t y,
                                                uint8_t button_mask);

    // Flushes any coalesced pointer move whose 6ms rate-limit window has elapsed.
    std::optional<PointerSample> FlushPendingPointer(TimePoint now);

    bool HasPendingPointer() const noexcept { return pending_pointer_.has_value(); }

    // Queues wheel scroll steps and paces emission smoothly (1 step per 12ms)
    // without churning SetPixelFormat on macOS screensharingd.
    // Returns an immediate WheelStep if the pacing window allows instant emission.
    std::optional<WheelStep> OnWheelInput(TimePoint now,
                                          uint16_t x,
                                          uint16_t y,
                                          int32_t steps_vertical,
                                          int32_t steps_horizontal,
                                          uint8_t button_mask);

    // Flushes the next paced wheel step once kWheelStepInterval has elapsed.
    std::optional<WheelStep> FlushPendingWheel(TimePoint now);

    bool HasPendingWheel() const noexcept {
        return pending_wheel_v_ != 0 || pending_wheel_h_ != 0;
    }

    // Evaluates desired pixel format (16 vs 32 bpp) at `now` against `applied_bpp`.
    QualityDecision PeekDecision(TimePoint now, uint8_t applied_bpp) const noexcept;

    // Commits that `new_bpp` (and optional full refresh) has now been applied on the wire.
    void CommitAppliedFormat(uint8_t new_bpp, bool full_refresh_sent) noexcept;

    // Frame RTT & FPS tracking.
    void OnUpdateRequestSent(TimePoint now, bool is_nudge = false) noexcept;
    void OnUpdateCompleted(TimePoint now);

    double GetFps(TimePoint now) const noexcept;
    double GetRttMs() const noexcept { return rtt_ms_; }
    uint64_t GetFramesReceived() const noexcept { return frames_received_; }
    bool IsBurstActive(TimePoint now) const noexcept;

private:
    uint8_t DesiredBpp(TimePoint now) const noexcept;

    QualityMode mode_ = QualityMode::Adaptive;
    bool burst_initialized_ = false;
    TimePoint last_burst_time_{};
    std::deque<TimePoint> recent_move_times_;

    bool has_last_pointer_ = false;
    PointerSample last_sent_pointer_{};
    TimePoint last_pointer_send_time_{};
    std::optional<PointerSample> pending_pointer_;

    bool has_last_wheel_ = false;
    TimePoint last_wheel_send_time_{};
    uint16_t wheel_x_ = 0;
    uint16_t wheel_y_ = 0;
    uint8_t wheel_button_mask_ = 0;
    int32_t pending_wheel_v_ = 0;
    int32_t pending_wheel_h_ = 0;

    bool explicit_full_refresh_ = false;

    bool request_in_flight_ = false;
    TimePoint request_sent_time_{};
    bool has_rtt_sample_ = false;
    double rtt_ms_ = 0.0;
    uint64_t frames_received_ = 0;
    std::deque<TimePoint> frame_times_;
};

}  // namespace mac_mirror
