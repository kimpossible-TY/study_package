#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "frame_gen_mode.h"
#include "rfb_session.h"

namespace mac_mirror {

inline float ClampCursorScale(float scale) noexcept {
    if (scale < 1.0f) return 1.0f;
    if (scale > 4.0f) return 4.0f;
    return scale;
}

// Non-sensitive UI preferences persisted to %LOCALAPPDATA%\Programs\mac-mirror\ui-prefs.json.
// Shared-PC security guarantee: NEVER add username, password, or secret fields here.
struct UiPrefs {
    float cursor_scale = 1.8f;
    QualityMode quality_mode = QualityMode::Turbo;
    FrameGenMode frame_gen_mode = FrameGenMode::Neural;
    bool fullscreen = true;
    bool swap_ctrl_cmd = true;
};

struct AppOptions {
    std::string host = "100.80.74.94";
    uint16_t port = 5900;
    std::string socks5_proxy;  // e.g. "127.0.0.1:1055"
    UiPrefs prefs;

    bool cursor_scale_set = false;
    bool quality_set = false;
    bool frame_gen_set = false;
    bool fullscreen_set = false;
    bool swap_ctrl_cmd_set = false;

    // Automated test hooks (in-memory only, never persisted to disk).
    std::string auto_user;
    std::string auto_pass_env;
    bool show_settings = false;
    bool show_hud = false;

    bool show_help = false;
    std::string error_message;

    ~AppOptions() {
        if (!auto_user.empty()) {
            volatile char* p = &auto_user[0];
            for (size_t i = 0; i < auto_user.size(); ++i) {
                p[i] = 0;
            }
            auto_user.clear();
        }
    }
};

std::string SerializeUiPrefsJson(const UiPrefs& prefs);
bool ParseUiPrefsJson(const std::string& json_text, UiPrefs* out_prefs);

UiPrefs LoadUiPrefsFromDisk();
bool SaveUiPrefsToDisk(const UiPrefs& prefs);

// Parses CLI arguments (argv[1..argc-1]) onto options (preserving existing options->prefs defaults
// unless explicitly overridden on the command line).
bool ParseCommandLineArgs(const std::vector<std::string>& args, AppOptions* options);

// Parses "host:port", "[ipv6]:port", or "host" (using default_port).
bool ParseHostPortString(const std::string& input,
                         std::string* out_host,
                         uint16_t* out_port,
                         uint16_t default_port = 5900);

std::string FormatUsageText();

// Reads an environment variable into out_value (in-memory UTF-8) and returns true if non-empty.
bool ReadEnvVarUtf8(const std::string& env_name, std::string* out_value);

// Securely wipes a std::string in memory.
inline void SecureWipeString(std::string* s) noexcept {
    if (s && !s->empty()) {
        volatile char* p = &(*s)[0];
        for (size_t i = 0; i < s->size(); ++i) {
            p[i] = 0;
        }
        s->clear();
    }
}

}  // namespace mac_mirror
