#include "rfb_adaptive.h"

#include <algorithm>

namespace mac_mirror {

AdaptiveQualityController::AdaptiveQualityController(QualityMode initial_mode)
    : mode_(initial_mode) {}

void AdaptiveQualityController::SetQualityMode(QualityMode mode, TimePoint now) {
    (void)now;
    if (mode_ != mode) {
        mode_ = mode;
        if (mode_ == QualityMode::Sharp) {
            explicit_full_refresh_ = true;
        }
    }
}

bool AdaptiveQualityController::IsBurstActive(TimePoint now) const noexcept {
    if (!burst_initialized_) {
        return false;
    }
    return (now - last_burst_time_) < kBurstHoldDuration;
}

uint8_t AdaptiveQualityController::DesiredBpp(TimePoint now) const noexcept {
    switch (mode_) {
        case QualityMode::Sharp:
            return 32;
        case QualityMode::Turbo:
            return 16;
        case QualityMode::Adaptive:
            return IsBurstActive(now) ? 16 : 32;
    }
    return 32;
}

std::optional<PointerSample> AdaptiveQualityController::OnPointerInput(TimePoint now,
                                                                       uint16_t x,
                                                                       uint16_t y,
                                                                       uint8_t button_mask) {
    bool button_changed = !has_last_pointer_ || (button_mask != last_sent_pointer_.button_mask);
    bool coords_changed = !has_last_pointer_ || (x != last_sent_pointer_.x) || (y != last_sent_pointer_.y);

    if (coords_changed) {
        while (!recent_move_times_.empty() &&
               (now - recent_move_times_.front()) > kRapidMoveWindow) {
            recent_move_times_.pop_front();
        }
        recent_move_times_.push_back(now);

        // Enter/extend 16-bit burst on drag, rapid pointer burst, or ongoing burst motion
        if (button_mask != 0 ||
            recent_move_times_.size() >= kRapidMoveThreshold ||
            IsBurstActive(now)) {
            burst_initialized_ = true;
            last_burst_time_ = now;
        }
    } else if (button_changed && IsBurstActive(now)) {
        // Drag release: hold 250ms before sharpening so final drop frame arrives smoothly first
        last_burst_time_ = now;
    }

    PointerSample sample{x, y, button_mask};

    // Never delay button press/release events, or initial pointer event, or Sharp mode
    bool throttle_active = (mode_ != QualityMode::Sharp);
    if (!has_last_pointer_ || button_changed || !throttle_active ||
        (now - last_pointer_send_time_) >= kPointerMinInterval) {
        has_last_pointer_ = true;
        last_sent_pointer_ = sample;
        last_pointer_send_time_ = now;
        pending_pointer_.reset();
        return sample;
    }

    // Coalesce rapid motion within the 6ms window
    pending_pointer_ = sample;
    return std::nullopt;
}

std::optional<PointerSample> AdaptiveQualityController::FlushPendingPointer(TimePoint now) {
    if (!pending_pointer_.has_value()) {
        return std::nullopt;
    }
    if (has_last_pointer_ && (now - last_pointer_send_time_) < kPointerMinInterval) {
        return std::nullopt;
    }
    PointerSample out = *pending_pointer_;
    pending_pointer_.reset();
    has_last_pointer_ = true;
    last_sent_pointer_ = out;
    last_pointer_send_time_ = now;
    return out;
}

void AdaptiveQualityController::OnWheelInput(TimePoint now) {
    burst_initialized_ = true;
    last_burst_time_ = now;
}

QualityDecision AdaptiveQualityController::PeekDecision(TimePoint now,
                                                        uint8_t applied_bpp) const noexcept {
    QualityDecision d{};
    d.target_bpp = DesiredBpp(now);
    d.format_changed = (d.target_bpp != applied_bpp);
    if (explicit_full_refresh_ || (d.format_changed && d.target_bpp == 32)) {
        d.request_full_refresh = true;
    }
    return d;
}

void AdaptiveQualityController::CommitAppliedFormat(uint8_t new_bpp,
                                                    bool full_refresh_sent) noexcept {
    (void)new_bpp;
    if (full_refresh_sent) {
        explicit_full_refresh_ = false;
    }
}

void AdaptiveQualityController::OnUpdateRequestSent(TimePoint now, bool is_nudge) noexcept {
    if (!is_nudge && !request_in_flight_) {
        request_in_flight_ = true;
        request_sent_time_ = now;
    }
}

void AdaptiveQualityController::OnUpdateCompleted(TimePoint now) {
    if (request_in_flight_) {
        double sample_ms = std::chrono::duration<double, std::milli>(now - request_sent_time_).count();
        if (sample_ms >= 0.0) {
            if (!has_rtt_sample_) {
                rtt_ms_ = sample_ms;
                has_rtt_sample_ = true;
            } else {
                rtt_ms_ = 0.35 * sample_ms + 0.65 * rtt_ms_;
            }
        }
        request_in_flight_ = false;
    }

    ++frames_received_;
    frame_times_.push_back(now);
    constexpr auto kOneSecond = std::chrono::milliseconds(1000);
    while (!frame_times_.empty() && (now - frame_times_.front()) > kOneSecond) {
        frame_times_.pop_front();
    }
}

double AdaptiveQualityController::GetFps(TimePoint now) const noexcept {
    constexpr auto kStaleWindow = std::chrono::milliseconds(1200);
    if (frame_times_.empty() || (now - frame_times_.back()) > kStaleWindow) {
        return 0.0;
    }
    constexpr auto kOneSecond = std::chrono::milliseconds(1000);
    size_t count = 0;
    TimePoint oldest = now;
    for (const auto& tp : frame_times_) {
        if ((now - tp) <= kOneSecond) {
            if (count == 0) oldest = tp;
            ++count;
        }
    }
    if (count == 0) {
        return 0.0;
    }
    double span_sec = std::chrono::duration<double>(now - oldest).count();
    if (count >= 2 && span_sec >= 0.15 && span_sec < 1.0) {
        // Smooth early ramp-up before a full 1.0s window accumulates
        return static_cast<double>(count - 1) / span_sec;
    }
    return static_cast<double>(count);
}

}  // namespace mac_mirror
