#pragma once

#include <cstdint>
#include <string>

namespace mac_mirror {

enum class FrameGenMode : uint8_t {
    Off         = 0,
    OpticalFlow = 1,
    Neural      = 2,
};

inline const char* FrameGenModeToString(FrameGenMode m) noexcept {
    switch (m) {
        case FrameGenMode::Off:         return "off";
        case FrameGenMode::OpticalFlow: return "flow";
        case FrameGenMode::Neural:      return "neural";
    }
    return "off";
}

inline const wchar_t* FrameGenModeHudLabel(FrameGenMode m) noexcept {
    switch (m) {
        case FrameGenMode::Off:         return L"FG:Off";
        case FrameGenMode::OpticalFlow: return L"FG:Flow";
        case FrameGenMode::Neural:      return L"FG:Neural";
    }
    return L"FG:Off";
}

inline bool TryParseFrameGenMode(const std::string& lower, FrameGenMode* out) noexcept {
    if (!out) return false;
    if (lower == "off" || lower == "none" || lower == "0") {
        *out = FrameGenMode::Off;
        return true;
    }
    if (lower == "flow" || lower == "optical" || lower == "opticalflow" || lower == "optical-flow") {
        *out = FrameGenMode::OpticalFlow;
        return true;
    }
    if (lower == "neural" || lower == "nn" || lower == "ai" || lower == "hybrid") {
        *out = FrameGenMode::Neural;
        return true;
    }
    return false;
}

inline FrameGenMode NextFrameGenMode(FrameGenMode m) noexcept {
    switch (m) {
        case FrameGenMode::Off:         return FrameGenMode::OpticalFlow;
        case FrameGenMode::OpticalFlow: return FrameGenMode::Neural;
        case FrameGenMode::Neural:      return FrameGenMode::Off;
    }
    return FrameGenMode::Off;
}

}  // namespace mac_mirror
