#pragma once

#include <string>

namespace mac_mirror {

// Normalizes Windows CRLF (\r\n) and legacy Mac CR (\r) line endings to LF (\n)
// before sending text to macOS via RFB ClientCutText.
inline std::string NormalizeLineEndingsToLf(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '\r') {
            if (i + 1 < input.size() && input[i + 1] == '\n') {
                ++i;
            }
            out.push_back('\n');
        } else {
            out.push_back(input[i]);
        }
    }
    return out;
}

// Normalizes LF (\n) or lone CR (\r) line endings to Windows CRLF (\r\n)
// without doubling existing CRLF pairs, for Windows CF_UNICODETEXT clipboard.
inline std::string NormalizeLineEndingsToCrlf(const std::string& input) {
    std::string out;
    out.reserve(input.size() + input.size() / 16u);
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '\r') {
            if (i + 1 < input.size() && input[i + 1] == '\n') {
                ++i;
            }
            out.push_back('\r');
            out.push_back('\n');
        } else if (input[i] == '\n') {
            out.push_back('\r');
            out.push_back('\n');
        } else {
            out.push_back(input[i]);
        }
    }
    return out;
}

}  // namespace mac_mirror
