#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "rfb_session.h"

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

// Standard Win32 virtual-key codes kept platform-independent so unit tests can
// validate Windows -> macOS key mapping on macOS/Linux as well as Windows.
namespace win_vk {
constexpr uint32_t kBack       = 0x08;
constexpr uint32_t kTab        = 0x09;
constexpr uint32_t kReturn     = 0x0D;
constexpr uint32_t kShift      = 0x10;
constexpr uint32_t kControl    = 0x11;
constexpr uint32_t kMenu       = 0x12;
constexpr uint32_t kPause      = 0x13;
constexpr uint32_t kCapital    = 0x14;
constexpr uint32_t kHangul     = 0x15;  // VK_KANA / VK_HANGUL
constexpr uint32_t kHanja      = 0x19;  // VK_KANJI / VK_HANJA
constexpr uint32_t kEscape     = 0x1B;
constexpr uint32_t kSpace      = 0x20;
constexpr uint32_t kPrior      = 0x21;
constexpr uint32_t kNext       = 0x22;
constexpr uint32_t kEnd        = 0x23;
constexpr uint32_t kHome       = 0x24;
constexpr uint32_t kLeft       = 0x25;
constexpr uint32_t kUp         = 0x26;
constexpr uint32_t kRight      = 0x27;
constexpr uint32_t kDown       = 0x28;
constexpr uint32_t kInsert     = 0x2D;
constexpr uint32_t kDelete     = 0x2E;
constexpr uint32_t kLWin       = 0x5B;
constexpr uint32_t kRWin       = 0x5C;
constexpr uint32_t kNumpad0    = 0x60;
constexpr uint32_t kNumpad1    = 0x61;
constexpr uint32_t kNumpad2    = 0x62;
constexpr uint32_t kNumpad3    = 0x63;
constexpr uint32_t kNumpad4    = 0x64;
constexpr uint32_t kNumpad5    = 0x65;
constexpr uint32_t kNumpad6    = 0x66;
constexpr uint32_t kNumpad7    = 0x67;
constexpr uint32_t kNumpad8    = 0x68;
constexpr uint32_t kNumpad9    = 0x69;
constexpr uint32_t kMultiply   = 0x6A;
constexpr uint32_t kAdd        = 0x6B;
constexpr uint32_t kSubtract   = 0x6D;
constexpr uint32_t kDecimal    = 0x6E;
constexpr uint32_t kDivide     = 0x6F;
constexpr uint32_t kF1         = 0x70;
constexpr uint32_t kF2         = 0x71;
constexpr uint32_t kF3         = 0x72;
constexpr uint32_t kF4         = 0x73;
constexpr uint32_t kF5         = 0x74;
constexpr uint32_t kF6         = 0x75;
constexpr uint32_t kF7         = 0x76;
constexpr uint32_t kF8         = 0x77;
constexpr uint32_t kF9         = 0x78;
constexpr uint32_t kF10        = 0x79;
constexpr uint32_t kF11        = 0x7A;
constexpr uint32_t kF12        = 0x7B;
constexpr uint32_t kScroll     = 0x91;
constexpr uint32_t kLShift     = 0xA0;
constexpr uint32_t kRShift     = 0xA1;
constexpr uint32_t kLControl   = 0xA2;
constexpr uint32_t kRControl   = 0xA3;
constexpr uint32_t kLMenu      = 0xA4;
constexpr uint32_t kRMenu      = 0xA5;
constexpr uint32_t kOem1       = 0xBA;
constexpr uint32_t kOemPlus    = 0xBB;
constexpr uint32_t kOemComma   = 0xBC;
constexpr uint32_t kOemMinus   = 0xBD;
constexpr uint32_t kOemPeriod  = 0xBE;
constexpr uint32_t kOem2       = 0xBF;
constexpr uint32_t kOem3       = 0xC0;
constexpr uint32_t kOem4       = 0xDB;
constexpr uint32_t kOem5       = 0xDC;
constexpr uint32_t kOem6       = 0xDD;
constexpr uint32_t kOem7       = 0xDE;
}  // namespace win_vk

struct KeyModifierState {
    bool shift = false;
    bool ctrl  = false;
    bool alt   = false;
    bool win   = false;
};

struct KeyEventStep {
    uint32_t keysym = 0;
    bool down = false;
};

class KeyMapEngine {
public:
    static constexpr uint64_t kRightAltTapMaxMs = 450u;
    static constexpr uint64_t kLangToggleDebounceMs = 120u;

    void SetSwapCtrlCmd(bool swap) noexcept { swap_ctrl_cmd_ = swap; }
    bool swap_ctrl_cmd() const noexcept { return swap_ctrl_cmd_; }

    void CancelPendingTap() noexcept { right_alt_tap_candidate_ = false; }

    bool IsRemotePasteShortcut(uint32_t vk, const KeyModifierState& mods) const noexcept {
        if (vk != 'V') return false;
        if (swap_ctrl_cmd_) {
            return mods.ctrl && !mods.alt && !mods.win;
        }
        return mods.win && !mods.ctrl && !mods.alt;
    }

    std::vector<KeyEventStep> ProcessKeyDown(uint32_t vk,
                                             uint32_t scancode,
                                             bool extended,
                                             bool is_repeat,
                                             bool is_right_shift,
                                             const KeyModifierState& mods,
                                             uint32_t unicode_ascii,
                                             uint64_t now_ms) {
        std::vector<KeyEventStep> out;
        const bool is_right_alt = IsRightAltKey(vk, extended);
        if (!is_right_alt) {
            right_alt_tap_candidate_ = false;
        }

        // Korean Hangul key (VK_HANGUL / 0x15): send atomic macOS Control+Space
        // input-source switch (ignore auto-repeat; Windows often omits WM_KEYUP).
        if (vk == win_vk::kHangul) {
            if (!is_repeat && !mods.shift && !mods.ctrl && !mods.alt && !mods.win) {
                EmitInputSourceToggle(&out, now_ms);
            }
            return out;
        }

        // Korean Hanja key (VK_HANJA / 0x19): send atomic macOS Option+Return.
        if (vk == win_vk::kHanja) {
            if (!is_repeat && !mods.ctrl && !mods.alt && !mods.win) {
                EmitAtomicChord(&out, rfb_keysym::kAltL, rfb_keysym::kReturn);
            }
            return out;
        }

        const uint16_t scan_id = MakeScanId(scancode, extended);
        auto existing = active_by_scan_.find(scan_id);
        if (existing != active_by_scan_.end()) {
            out.push_back(KeyEventStep{existing->second, true});
            return out;
        }

        // Track clean bare Right Alt press so US-layout / 101-key Windows keyboards
        // (where physical Han/Eng sends Right Alt) also toggle macOS input source on tap,
        // while still acting as macOS Option_R whenever chorded with another key or mouse.
        if (is_right_alt && !is_repeat && !mods.shift && !mods.ctrl && !mods.win &&
            active_by_scan_.empty()) {
            right_alt_tap_candidate_ = true;
            right_alt_down_ms_ = now_ms;
        }

        const uint32_t sym = ResolveKeysym(vk, extended, is_right_shift, mods, unicode_ascii);
        if (sym != 0) {
            if (scan_id != 0) {
                active_by_scan_[scan_id] = sym;
            }
            out.push_back(KeyEventStep{sym, true});
        }
        return out;
    }

    std::vector<KeyEventStep> ProcessKeyUp(uint32_t vk,
                                           uint32_t scancode,
                                           bool extended,
                                           bool is_right_shift,
                                           const KeyModifierState& mods,
                                           uint32_t unicode_ascii,
                                           uint64_t now_ms) {
        std::vector<KeyEventStep> out;
        if (vk == win_vk::kHangul || vk == win_vk::kHanja) {
            return out;
        }

        const uint16_t scan_id = MakeScanId(scancode, extended);
        uint32_t sym = 0;
        auto it = active_by_scan_.find(scan_id);
        if (it != active_by_scan_.end()) {
            sym = it->second;
            active_by_scan_.erase(it);
        } else {
            sym = ResolveKeysym(vk, extended, is_right_shift, mods, unicode_ascii);
        }

        if (sym != 0) {
            out.push_back(KeyEventStep{sym, false});
        }

        if (IsRightAltKey(vk, extended) && right_alt_tap_candidate_) {
            right_alt_tap_candidate_ = false;
            if (now_ms >= right_alt_down_ms_ && (now_ms - right_alt_down_ms_) <= kRightAltTapMaxMs) {
                EmitInputSourceToggle(&out, now_ms);
            }
        }
        return out;
    }

    std::vector<KeyEventStep> ReleaseAllEvents() {
        right_alt_tap_candidate_ = false;
        std::vector<KeyEventStep> out;
        out.reserve(active_by_scan_.size());
        for (const auto& kv : active_by_scan_) {
            out.push_back(KeyEventStep{kv.second, false});
        }
        active_by_scan_.clear();
        return out;
    }

private:
    static uint16_t MakeScanId(uint32_t scancode, bool extended) noexcept {
        return static_cast<uint16_t>((scancode & 0xFFu) | (extended ? 0x0100u : 0u));
    }

    static bool IsRightAltKey(uint32_t vk, bool extended) noexcept {
        return vk == win_vk::kRMenu || (vk == win_vk::kMenu && extended);
    }

    bool IsKeysymActive(uint32_t keysym) const noexcept {
        for (const auto& kv : active_by_scan_) {
            if (kv.second == keysym) return true;
        }
        return false;
    }

    void EmitAtomicChord(std::vector<KeyEventStep>* out, uint32_t modifier_sym, uint32_t key_sym) {
        const bool mod_already_down = IsKeysymActive(modifier_sym);
        if (!mod_already_down) {
            out->push_back(KeyEventStep{modifier_sym, true});
        }
        out->push_back(KeyEventStep{key_sym, true});
        out->push_back(KeyEventStep{key_sym, false});
        if (!mod_already_down) {
            out->push_back(KeyEventStep{modifier_sym, false});
        }
    }

    void EmitInputSourceToggle(std::vector<KeyEventStep>* out, uint64_t now_ms) {
        if (has_last_lang_toggle_ && now_ms >= last_lang_toggle_ms_ &&
            (now_ms - last_lang_toggle_ms_) < kLangToggleDebounceMs) {
            return;
        }
        has_last_lang_toggle_ = true;
        last_lang_toggle_ms_ = now_ms;
        EmitAtomicChord(out, rfb_keysym::kControlL, 0x0020u);
    }

    uint32_t ResolveControlKeysym(bool right) const noexcept {
        if (swap_ctrl_cmd_) {
            return right ? rfb_keysym::kMetaR : rfb_keysym::kMetaL;
        }
        return right ? rfb_keysym::kControlR : rfb_keysym::kControlL;
    }

    uint32_t ResolveWinKeysym(bool right) const noexcept {
        if (swap_ctrl_cmd_) {
            return right ? rfb_keysym::kControlR : rfb_keysym::kControlL;
        }
        return right ? rfb_keysym::kMetaR : rfb_keysym::kMetaL;
    }

    uint32_t ResolveKeysym(uint32_t vk,
                           bool extended,
                           bool is_right_shift,
                           const KeyModifierState& mods,
                           uint32_t unicode_ascii) const noexcept {
        if (vk == win_vk::kShift) {
            return is_right_shift ? rfb_keysym::kShiftR : rfb_keysym::kShiftL;
        }
        if (vk == win_vk::kControl) {
            return ResolveControlKeysym(extended);
        }
        if (vk == win_vk::kMenu) {
            return extended ? rfb_keysym::kAltR : rfb_keysym::kAltL;
        }

        switch (vk) {
            case win_vk::kLShift:   return rfb_keysym::kShiftL;
            case win_vk::kRShift:   return rfb_keysym::kShiftR;
            case win_vk::kLControl: return ResolveControlKeysym(false);
            case win_vk::kRControl: return ResolveControlKeysym(true);
            case win_vk::kLMenu:    return rfb_keysym::kAltL;
            case win_vk::kRMenu:    return rfb_keysym::kAltR;
            case win_vk::kLWin:     return ResolveWinKeysym(false);
            case win_vk::kRWin:     return ResolveWinKeysym(true);
            case win_vk::kCapital:  return rfb_keysym::kCapsLock;
            case win_vk::kBack:     return rfb_keysym::kBackSpace;
            case win_vk::kTab:      return rfb_keysym::kTab;
            case win_vk::kReturn:   return rfb_keysym::kReturn;
            case win_vk::kEscape:   return rfb_keysym::kEscape;
            case win_vk::kDelete:   return rfb_keysym::kDelete;
            case win_vk::kInsert:   return rfb_keysym::kInsert;
            case win_vk::kHome:     return rfb_keysym::kHome;
            case win_vk::kEnd:      return rfb_keysym::kEnd;
            case win_vk::kPrior:    return rfb_keysym::kPageUp;
            case win_vk::kNext:     return rfb_keysym::kPageDown;
            case win_vk::kLeft:     return rfb_keysym::kLeft;
            case win_vk::kUp:       return rfb_keysym::kUp;
            case win_vk::kRight:    return rfb_keysym::kRight;
            case win_vk::kDown:     return rfb_keysym::kDown;
            case win_vk::kPause:    return rfb_keysym::kPause;
            case win_vk::kScroll:   return rfb_keysym::kScrollLock;
            case win_vk::kSpace:    return 0x0020u;
            case win_vk::kF1:       return rfb_keysym::kF1;
            case win_vk::kF2:       return rfb_keysym::kF2;
            case win_vk::kF3:       return rfb_keysym::kF3;
            case win_vk::kF4:       return rfb_keysym::kF4;
            case win_vk::kF5:       return rfb_keysym::kF5;
            case win_vk::kF6:       return rfb_keysym::kF6;
            case win_vk::kF7:       return rfb_keysym::kF7;
            case win_vk::kF8:       return rfb_keysym::kF8;
            case win_vk::kF9:       return rfb_keysym::kF9;
            case win_vk::kF10:      return rfb_keysym::kF10;
            case win_vk::kF11:      return rfb_keysym::kF11;
            case win_vk::kF12:      return rfb_keysym::kF12;
            case win_vk::kNumpad0:  return '0';
            case win_vk::kNumpad1:  return '1';
            case win_vk::kNumpad2:  return '2';
            case win_vk::kNumpad3:  return '3';
            case win_vk::kNumpad4:  return '4';
            case win_vk::kNumpad5:  return '5';
            case win_vk::kNumpad6:  return '6';
            case win_vk::kNumpad7:  return '7';
            case win_vk::kNumpad8:  return '8';
            case win_vk::kNumpad9:  return '9';
            case win_vk::kMultiply: return '*';
            case win_vk::kAdd:      return '+';
            case win_vk::kSubtract: return '-';
            case win_vk::kDecimal:  return '.';
            case win_vk::kDivide:   return '/';
            default: break;
        }

        if (!mods.ctrl && !mods.alt && !mods.win && unicode_ascii >= 0x20u && unicode_ascii <= 0x7Eu) {
            return unicode_ascii;
        }

        // Deliberately use physical Shift only (not local Windows Caps Lock state),
        // because macOS manages its own Caps Lock / ABC switching state and uppercase
        // keysyms would trigger shifted Korean double-consonants (ㄲ/ㄸ/ㅃ/ㅆ/ㅉ).
        if (vk >= 'A' && vk <= 'Z') {
            return mods.shift ? vk : (vk - 'A' + 'a');
        }
        if (vk >= '0' && vk <= '9') {
            if (!mods.shift) return vk;
            static const char kShiftedDigits[10] = {')', '!', '@', '#', '$', '%', '^', '&', '*', '('};
            return static_cast<uint32_t>(kShiftedDigits[vk - '0']);
        }

        switch (vk) {
            case win_vk::kOemMinus:  return mods.shift ? '_' : '-';
            case win_vk::kOemPlus:   return mods.shift ? '+' : '=';
            case win_vk::kOem4:      return mods.shift ? '{' : '[';
            case win_vk::kOem6:      return mods.shift ? '}' : ']';
            case win_vk::kOem5:      return mods.shift ? '|' : '\\';
            case win_vk::kOem1:      return mods.shift ? ':' : ';';
            case win_vk::kOem7:      return mods.shift ? '"' : '\'';
            case win_vk::kOemComma:  return mods.shift ? '<' : ',';
            case win_vk::kOemPeriod: return mods.shift ? '>' : '.';
            case win_vk::kOem2:      return mods.shift ? '?' : '/';
            case win_vk::kOem3:      return mods.shift ? '~' : '`';
            default: break;
        }
        return 0;
    }

    bool swap_ctrl_cmd_ = true;
    bool right_alt_tap_candidate_ = false;
    uint64_t right_alt_down_ms_ = 0;
    bool has_last_lang_toggle_ = false;
    uint64_t last_lang_toggle_ms_ = 0;
    std::unordered_map<uint16_t, uint32_t> active_by_scan_;
};

#ifdef _WIN32
class Win32KeyMapper {
public:
    void SetSwapCtrlCmd(bool swap) noexcept { engine_.SetSwapCtrlCmd(swap); }
    bool swap_ctrl_cmd() const noexcept { return engine_.swap_ctrl_cmd(); }

    void CancelPendingTap() noexcept { engine_.CancelPendingTap(); }

    bool IsRemotePasteShortcut(WPARAM vk_param) const noexcept {
        return engine_.IsRemotePasteShortcut(static_cast<uint32_t>(vk_param), QueryModifiers());
    }

    void HandleKeyDown(RfbSession* session, WPARAM vk_param, LPARAM lparam, uint64_t now_ms) {
        if (!session) return;
        const uint32_t vk = static_cast<uint32_t>(vk_param);
        const uint32_t scancode = (static_cast<uint32_t>(lparam) >> 16) & 0xFFu;
        const bool extended = (lparam & (1LL << 24)) != 0;
        const bool is_repeat = (lparam & (1LL << 30)) != 0;
        const bool is_right_shift = (vk == VK_SHIFT && scancode != 0) &&
                                    (MapVirtualKeyW(scancode, MAPVK_VSC_TO_VK_EX) == VK_RSHIFT);
        const KeyModifierState mods = QueryModifiers();
        const uint32_t ascii = ResolveUnicodeAscii(vk, scancode, mods);

        auto steps = engine_.ProcessKeyDown(
            vk, scancode, extended, is_repeat, is_right_shift, mods, ascii, now_ms);
        for (const auto& step : steps) {
            session->SendKeyEvent(step.keysym, step.down);
        }
    }

    void HandleKeyUp(RfbSession* session, WPARAM vk_param, LPARAM lparam, uint64_t now_ms) {
        if (!session) return;
        const uint32_t vk = static_cast<uint32_t>(vk_param);
        const uint32_t scancode = (static_cast<uint32_t>(lparam) >> 16) & 0xFFu;
        const bool extended = (lparam & (1LL << 24)) != 0;
        const bool is_right_shift = (vk == VK_SHIFT && scancode != 0) &&
                                    (MapVirtualKeyW(scancode, MAPVK_VSC_TO_VK_EX) == VK_RSHIFT);
        const KeyModifierState mods = QueryModifiers();
        const uint32_t ascii = ResolveUnicodeAscii(vk, scancode, mods);

        auto steps = engine_.ProcessKeyUp(vk, scancode, extended, is_right_shift, mods, ascii, now_ms);
        for (const auto& step : steps) {
            session->SendKeyEvent(step.keysym, step.down);
        }
    }

    void ReleaseAll(RfbSession* session) {
        auto steps = engine_.ReleaseAllEvents();
        if (!session) return;
        for (const auto& step : steps) {
            session->SendKeyEvent(step.keysym, step.down);
        }
    }

private:
    static KeyModifierState QueryModifiers() noexcept {
        KeyModifierState mods{};
        mods.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        mods.ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        mods.alt   = (GetKeyState(VK_MENU) & 0x8000) != 0;
        mods.win   = ((GetKeyState(VK_LWIN) & 0x8000) != 0) ||
                     ((GetKeyState(VK_RWIN) & 0x8000) != 0);
        return mods;
    }

    static uint32_t ResolveUnicodeAscii(uint32_t vk, uint32_t scancode, const KeyModifierState& mods) noexcept {
        if (mods.ctrl || mods.alt || mods.win) {
            return 0;
        }
        BYTE key_state[256] = {};
        if (!GetKeyboardState(key_state)) {
            return 0;
        }
        // Ignore Windows Caps Lock toggle so macOS handles Caps Lock & Korean 2-Set natively.
        key_state[VK_CAPITAL] = 0;
        wchar_t chars[4] = {};
        int rc = ToUnicode(vk, scancode, key_state, chars, 4, 0);
        if (rc == 1 && chars[0] >= 0x20 && chars[0] <= 0x7E) {
            return static_cast<uint32_t>(chars[0]);
        }
        return 0;
    }

    KeyMapEngine engine_;
};
#endif

}  // namespace mac_mirror
