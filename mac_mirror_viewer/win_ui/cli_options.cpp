#include "cli_options.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace mac_mirror {

namespace {

std::string TrimAscii(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(start, end - start);
}

std::string ToLowerAscii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool ParsePortNumber(const std::string& text, uint16_t* out_port) {
    if (text.empty() || text.size() > 5) return false;
    uint32_t val = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        val = val * 10u + static_cast<uint32_t>(c - '0');
    }
    if (val < 1u || val > 65535u) return false;
    *out_port = static_cast<uint16_t>(val);
    return true;
}

size_t FindJsonKeyColon(const std::string& json, const std::string& key) {
    const std::string quoted = "\"" + key + "\"";
    size_t pos = json.find(quoted);
    if (pos == std::string::npos) return std::string::npos;
    pos += quoted.size();
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != ':') return std::string::npos;
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
    return pos;
}

#ifdef _WIN32
std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), nullptr, 0);
    if (len <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), &out[0], len);
    return out;
}

std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int len = WideCharToMultiByte(
        CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return std::string();
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), &out[0], len, nullptr, nullptr);
    return out;
}

std::wstring GetUiPrefsFilePath(std::wstring* out_programs_dir, std::wstring* out_app_dir) {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    if (n == 0 || n >= sizeof(buf) / sizeof(buf[0])) {
        return std::wstring();
    }
    std::wstring base(buf, n);
    if (!base.empty() && base.back() != L'\\' && base.back() != L'/') {
        base.push_back(L'\\');
    }
    std::wstring programs = base + L"Programs";
    std::wstring app_dir = programs + L"\\mac-mirror";
    if (out_programs_dir) *out_programs_dir = programs;
    if (out_app_dir) *out_app_dir = app_dir;
    return app_dir + L"\\ui-prefs.json";
}
#endif

}  // namespace

std::string SerializeUiPrefsJson(const UiPrefs& prefs) {
    const float clamped = std::round(ClampCursorScale(prefs.cursor_scale) * 10.0f) / 10.0f;
    const char* mode_str = "adaptive";
    switch (prefs.quality_mode) {
        case QualityMode::Adaptive: mode_str = "adaptive"; break;
        case QualityMode::Sharp:    mode_str = "sharp"; break;
        case QualityMode::Turbo:    mode_str = "turbo"; break;
    }
    char scale_buf[32] = {};
    std::snprintf(scale_buf, sizeof(scale_buf), "%.1f", static_cast<double>(clamped));

    std::ostringstream oss;
    oss << "{\n"
        << "  \"cursor_scale\": " << scale_buf << ",\n"
        << "  \"quality_mode\": \"" << mode_str << "\",\n"
        << "  \"frame_gen\": \"" << FrameGenModeToString(prefs.frame_gen_mode) << "\",\n"
        << "  \"fullscreen\": " << (prefs.fullscreen ? "true" : "false") << ",\n"
        << "  \"swap_ctrl_cmd\": " << (prefs.swap_ctrl_cmd ? "true" : "false") << "\n"
        << "}\n";
    return oss.str();
}

bool ParseUiPrefsJson(const std::string& json_text, UiPrefs* out_prefs) {
    if (!out_prefs) return false;
    UiPrefs parsed = *out_prefs;

    size_t scale_pos = FindJsonKeyColon(json_text, "cursor_scale");
    if (scale_pos != std::string::npos) {
        char* end_ptr = nullptr;
        float v = std::strtof(json_text.c_str() + scale_pos, &end_ptr);
        if (end_ptr != json_text.c_str() + scale_pos && std::isfinite(v)) {
            parsed.cursor_scale = ClampCursorScale(v);
        }
    }

    size_t qual_pos = FindJsonKeyColon(json_text, "quality_mode");
    if (qual_pos != std::string::npos && qual_pos < json_text.size() && json_text[qual_pos] == '"') {
        size_t end_q = json_text.find('"', qual_pos + 1);
        if (end_q != std::string::npos) {
            std::string val = ToLowerAscii(json_text.substr(qual_pos + 1, end_q - (qual_pos + 1)));
            parsed.quality_mode = ParseQualityMode(val);
        }
    }

    size_t fg_pos = FindJsonKeyColon(json_text, "frame_gen");
    if (fg_pos != std::string::npos && fg_pos < json_text.size() && json_text[fg_pos] == '"') {
        size_t end_q = json_text.find('"', fg_pos + 1);
        if (end_q != std::string::npos) {
            std::string val = ToLowerAscii(TrimAscii(json_text.substr(fg_pos + 1, end_q - (fg_pos + 1))));
            FrameGenMode fg = parsed.frame_gen_mode;
            if (TryParseFrameGenMode(val, &fg)) {
                parsed.frame_gen_mode = fg;
            }
        }
    }

    size_t fs_pos = FindJsonKeyColon(json_text, "fullscreen");
    if (fs_pos != std::string::npos) {
        if (json_text.compare(fs_pos, 4, "true") == 0) {
            parsed.fullscreen = true;
        } else if (json_text.compare(fs_pos, 5, "false") == 0) {
            parsed.fullscreen = false;
        }
    }

    size_t swap_pos = FindJsonKeyColon(json_text, "swap_ctrl_cmd");
    if (swap_pos != std::string::npos) {
        if (json_text.compare(swap_pos, 4, "true") == 0) {
            parsed.swap_ctrl_cmd = true;
        } else if (json_text.compare(swap_pos, 5, "false") == 0) {
            parsed.swap_ctrl_cmd = false;
        }
    }

    *out_prefs = parsed;
    return true;
}

UiPrefs LoadUiPrefsFromDisk() {
    UiPrefs prefs;
#ifdef _WIN32
    std::wstring path = GetUiPrefsFilePath(nullptr, nullptr);
    if (path.empty()) return prefs;

    HANDLE hFile = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return prefs;
    }

    LARGE_INTEGER file_size = {};
    if (GetFileSizeEx(hFile, &file_size) && file_size.QuadPart > 0 && file_size.QuadPart <= 65536) {
        std::string content(static_cast<size_t>(file_size.QuadPart), '\0');
        DWORD bytes_read = 0;
        if (ReadFile(hFile, &content[0], static_cast<DWORD>(content.size()), &bytes_read, nullptr)) {
            content.resize(bytes_read);
            ParseUiPrefsJson(content, &prefs);
        }
    }
    CloseHandle(hFile);
#endif
    return prefs;
}

bool SaveUiPrefsToDisk(const UiPrefs& prefs) {
#ifdef _WIN32
    std::wstring programs_dir;
    std::wstring app_dir;
    std::wstring path = GetUiPrefsFilePath(&programs_dir, &app_dir);
    if (path.empty()) return false;

    CreateDirectoryW(programs_dir.c_str(), nullptr);
    CreateDirectoryW(app_dir.c_str(), nullptr);

    const std::string payload = SerializeUiPrefsJson(prefs);
    const std::wstring tmp_path = path + L".tmp";

    HANDLE hFile = CreateFileW(
        tmp_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD written = 0;
    const BOOL write_ok = WriteFile(
        hFile, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr);
    FlushFileBuffers(hFile);
    CloseHandle(hFile);

    if (!write_ok || written != payload.size()) {
        DeleteFileW(tmp_path.c_str());
        return false;
    }

    if (!MoveFileExW(tmp_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp_path.c_str());
        return false;
    }
    return true;
#else
    (void)prefs;
    return false;
#endif
}

bool ParseHostPortString(const std::string& input,
                         std::string* out_host,
                         uint16_t* out_port,
                         uint16_t default_port) {
    if (!out_host || !out_port) return false;
    const std::string s = TrimAscii(input);
    if (s.empty()) return false;

    if (s.front() == '[') {
        size_t close_bracket = s.find(']');
        if (close_bracket == std::string::npos || close_bracket <= 1) return false;
        std::string host_part = s.substr(1, close_bracket - 1);
        uint16_t port_part = default_port;
        if (close_bracket + 1 < s.size()) {
            if (s[close_bracket + 1] != ':') return false;
            if (!ParsePortNumber(s.substr(close_bracket + 2), &port_part)) return false;
        }
        *out_host = host_part;
        *out_port = port_part;
        return true;
    }

    size_t first_colon = s.find(':');
    size_t last_colon = s.rfind(':');
    if (first_colon != std::string::npos && first_colon == last_colon) {
        std::string host_part = TrimAscii(s.substr(0, first_colon));
        std::string port_str = TrimAscii(s.substr(first_colon + 1));
        if (host_part.empty()) return false;
        uint16_t port_part = default_port;
        if (!ParsePortNumber(port_str, &port_part)) return false;
        *out_host = host_part;
        *out_port = port_part;
        return true;
    }

    *out_host = s;
    *out_port = default_port;
    return true;
}

bool ParseCommandLineArgs(const std::vector<std::string>& args, AppOptions* options) {
    if (!options) return false;

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& raw = args[i];
        if (raw.empty()) continue;

        std::string flag = raw;
        std::string inline_val;
        bool has_inline_val = false;
        const size_t eq_pos = raw.find('=');
        if (raw.rfind("--", 0) == 0 && eq_pos != std::string::npos) {
            flag = raw.substr(0, eq_pos);
            inline_val = raw.substr(eq_pos + 1);
            has_inline_val = true;
        }

        auto consume_value = [&](std::string* out) -> bool {
            if (has_inline_val) {
                *out = inline_val;
                return true;
            }
            if (i + 1 >= args.size()) {
                options->error_message = "Missing value for option: " + flag;
                return false;
            }
            *out = args[++i];
            return true;
        };

        if (flag == "-h" || flag == "--help" || flag == "/?") {
            options->show_help = true;
            return true;
        } else if (flag == "--windowed") {
            options->prefs.fullscreen = false;
            options->fullscreen_set = true;
        } else if (flag == "--fullscreen") {
            options->prefs.fullscreen = true;
            options->fullscreen_set = true;
        } else if (flag == "--swap-ctrl-cmd") {
            options->prefs.swap_ctrl_cmd = true;
            options->swap_ctrl_cmd_set = true;
        } else if (flag == "--no-swap-ctrl-cmd") {
            options->prefs.swap_ctrl_cmd = false;
            options->swap_ctrl_cmd_set = true;
        } else if (flag == "--host") {
            std::string val;
            if (!consume_value(&val)) return false;
            uint16_t parsed_port = options->port;
            std::string parsed_host;
            if (val.find(':') != std::string::npos &&
                ParseHostPortString(val, &parsed_host, &parsed_port, options->port)) {
                options->host = parsed_host;
                options->port = parsed_port;
            } else {
                options->host = TrimAscii(val);
            }
            if (options->host.empty()) {
                options->error_message = "Invalid --host value";
                return false;
            }
        } else if (flag == "--port") {
            std::string val;
            if (!consume_value(&val)) return false;
            uint16_t p = 0;
            if (!ParsePortNumber(TrimAscii(val), &p)) {
                options->error_message = "Invalid --port value: " + val;
                return false;
            }
            options->port = p;
        } else if (flag == "--socks5") {
            std::string val;
            if (!consume_value(&val)) return false;
            options->socks5_proxy = TrimAscii(val);
            if (!options->socks5_proxy.empty()) {
                std::string shost;
                uint16_t sport = 0;
                if (!ParseHostPortString(options->socks5_proxy, &shost, &sport, 1055)) {
                    options->error_message = "Invalid --socks5 address (expected host:port): " + val;
                    return false;
                }
                options->socks5_proxy = shost + ":" + std::to_string(sport);
            }
        } else if (flag == "--no-socks5") {
            options->socks5_proxy.clear();
        } else if (flag == "--cursor-scale") {
            std::string val;
            if (!consume_value(&val)) return false;
            char* end_ptr = nullptr;
            float s = std::strtof(val.c_str(), &end_ptr);
            if (end_ptr == val.c_str() || !std::isfinite(s)) {
                options->error_message = "Invalid --cursor-scale value: " + val;
                return false;
            }
            options->prefs.cursor_scale = ClampCursorScale(s);
            options->cursor_scale_set = true;
        } else if (flag == "--quality") {
            std::string val;
            if (!consume_value(&val)) return false;
            const std::string low = ToLowerAscii(TrimAscii(val));
            if (low != "adaptive" && low != "sharp" && low != "turbo") {
                options->error_message = "Invalid --quality mode (expected adaptive|sharp|turbo): " + val;
                return false;
            }
            options->prefs.quality_mode = ParseQualityMode(low);
            options->quality_set = true;
        } else if (flag == "--frame-gen") {
            std::string val;
            if (!consume_value(&val)) return false;
            const std::string low = ToLowerAscii(TrimAscii(val));
            FrameGenMode fg = FrameGenMode::Off;
            if (!TryParseFrameGenMode(low, &fg)) {
                options->error_message = "Invalid --frame-gen mode (expected off|flow|neural): " + val;
                return false;
            }
            options->prefs.frame_gen_mode = fg;
            options->frame_gen_set = true;
        } else if (flag == "--auto-user") {
            std::string val;
            if (!consume_value(&val)) return false;
            options->auto_user = val;
            SecureWipeString(&val);
        } else if (flag == "--auto-pass-env") {
            std::string val;
            if (!consume_value(&val)) return false;
            options->auto_pass_env = TrimAscii(val);
        } else if (flag == "--show-settings") {
            options->show_settings = true;
        } else if (flag == "--show-hud") {
            options->show_hud = true;
        } else {
            options->error_message = "Unknown option: " + raw;
            return false;
        }
    }

    return true;
}

std::string FormatUsageText() {
    return "mac-mirror-viewer.exe - Native Direct3D 11 macOS Screen Sharing Viewer\n\n"
           "Options:\n"
           "  --host <ip>                Target Mac IP or hostname (default: 100.80.74.94)\n"
           "  --port <port>              Target RFB/VNC TCP port (default: 5900)\n"
           "  --socks5 <host:port>       Optional SOCKS5 proxy (e.g. 127.0.0.1:1055)\n"
           "  --cursor-scale <1.0..4.0>  Vector macOS cursor scale (default: 1.8)\n"
           "  --quality <mode>           adaptive | sharp | turbo (default: adaptive)\n"
           "  --frame-gen <mode>         off | flow | neural (default: off)\n"
           "  --windowed                 Start in windowed mode instead of borderless fullscreen\n"
           "  --swap-ctrl-cmd            Swap Windows Ctrl and macOS Command (default: on)\n"
           "  --no-swap-ctrl-cmd         Keep Windows Ctrl as macOS Control and Win as Command\n"
           "  --auto-user <username>     Automated test username (in-memory only)\n"
           "  --auto-pass-env <ENV>      Read automated test password from ENV (in-memory only)\n"
           "  -h, --help                 Show this help message\n";
}

bool ReadEnvVarUtf8(const std::string& env_name, std::string* out_value) {
    if (!out_value || env_name.empty()) return false;
#ifdef _WIN32
    const std::wstring wname = Utf8ToWide(env_name);
    if (wname.empty()) return false;
    DWORD needed = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (needed <= 1) return false;
    std::wstring wbuf(static_cast<size_t>(needed - 1), L'\0');
    DWORD got = GetEnvironmentVariableW(wname.c_str(), &wbuf[0], needed);
    if (got == 0) return false;
    wbuf.resize(got);
    *out_value = WideToUtf8(wbuf);
    volatile wchar_t* wp = &wbuf[0];
    for (size_t i = 0; i < wbuf.size(); ++i) {
        wp[i] = 0;
    }
    return !out_value->empty();
#else
    const char* v = std::getenv(env_name.c_str());
    if (!v || !*v) return false;
    *out_value = v;
    return true;
#endif
}

}  // namespace mac_mirror
