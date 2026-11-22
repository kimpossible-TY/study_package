#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <unordered_map>

#include "rfb_session.h"

namespace mac_mirror {

class Win32KeyMapper {
public:
    uint32_t TranslateKeyDown(WPARAM vk_param, LPARAM lparam) {
        const UINT scancode = (static_cast<UINT>(lparam) >> 16) & 0xFFu;
        const bool extended = (lparam & (1LL << 24)) != 0;
        const uint16_t scan_id = static_cast<uint16_t>(scancode | (extended ? 0x0100u : 0u));

        auto existing = active_by_scan_.find(scan_id);
        if (existing != active_by_scan_.end()) {
            return existing->second;
        }

        const uint32_t sym = ResolveKeysym(static_cast<UINT>(vk_param), scancode, extended);
        if (sym != 0 && scan_id != 0) {
            active_by_scan_[scan_id] = sym;
        }
        return sym;
    }

    uint32_t TranslateKeyUp(WPARAM vk_param, LPARAM lparam) {
        const UINT scancode = (static_cast<UINT>(lparam) >> 16) & 0xFFu;
        const bool extended = (lparam & (1LL << 24)) != 0;
        const uint16_t scan_id = static_cast<uint16_t>(scancode | (extended ? 0x0100u : 0u));

        auto it = active_by_scan_.find(scan_id);
        if (it != active_by_scan_.end()) {
            const uint32_t sym = it->second;
            active_by_scan_.erase(it);
            return sym;
        }
        return ResolveKeysym(static_cast<UINT>(vk_param), scancode, extended);
    }

    void ReleaseAll(RfbSession* session) {
        if (session) {
            for (const auto& kv : active_by_scan_) {
                session->SendKeyEvent(kv.second, false);
            }
        }
        active_by_scan_.clear();
    }

private:
    static uint32_t ResolveKeysym(UINT vk, UINT scancode, bool extended) {
        if (vk == VK_SHIFT && scancode != 0) {
            UINT ex_vk = MapVirtualKeyW(scancode, MAPVK_VSC_TO_VK_EX);
            if (ex_vk == VK_RSHIFT) return rfb_keysym::kShiftR;
            return rfb_keysym::kShiftL;
        }
        if (vk == VK_CONTROL) {
            return extended ? rfb_keysym::kControlR : rfb_keysym::kControlL;
        }
        if (vk == VK_MENU) {
            return extended ? rfb_keysym::kAltR : rfb_keysym::kAltL;
        }

        switch (vk) {
            case VK_LSHIFT:   return rfb_keysym::kShiftL;
            case VK_RSHIFT:   return rfb_keysym::kShiftR;
            case VK_LCONTROL: return rfb_keysym::kControlL;
            case VK_RCONTROL: return rfb_keysym::kControlR;
            case VK_LMENU:    return rfb_keysym::kAltL;
            case VK_RMENU:    return rfb_keysym::kAltR;
            case VK_LWIN:     return rfb_keysym::kMetaL;
            case VK_RWIN:     return rfb_keysym::kMetaR;
            case VK_CAPITAL:  return rfb_keysym::kCapsLock;
            case VK_BACK:     return rfb_keysym::kBackSpace;
            case VK_TAB:      return rfb_keysym::kTab;
            case VK_RETURN:   return rfb_keysym::kReturn;
            case VK_ESCAPE:   return rfb_keysym::kEscape;
            case VK_DELETE:   return rfb_keysym::kDelete;
            case VK_INSERT:   return rfb_keysym::kInsert;
            case VK_HOME:     return rfb_keysym::kHome;
            case VK_END:      return rfb_keysym::kEnd;
            case VK_PRIOR:    return rfb_keysym::kPageUp;
            case VK_NEXT:     return rfb_keysym::kPageDown;
            case VK_LEFT:     return rfb_keysym::kLeft;
            case VK_UP:       return rfb_keysym::kUp;
            case VK_RIGHT:    return rfb_keysym::kRight;
            case VK_DOWN:     return rfb_keysym::kDown;
            case VK_PAUSE:    return rfb_keysym::kPause;
            case VK_SCROLL:   return rfb_keysym::kScrollLock;
            case VK_SPACE:    return 0x0020u;
            case VK_F1:       return rfb_keysym::kF1;
            case VK_F2:       return rfb_keysym::kF2;
            case VK_F3:       return rfb_keysym::kF3;
            case VK_F4:       return rfb_keysym::kF4;
            case VK_F5:       return rfb_keysym::kF5;
            case VK_F6:       return rfb_keysym::kF6;
            case VK_F7:       return rfb_keysym::kF7;
            case VK_F8:       return rfb_keysym::kF8;
            case VK_F9:       return rfb_keysym::kF9;
            case VK_F10:      return rfb_keysym::kF10;
            case VK_F11:      return rfb_keysym::kF11;
            case VK_F12:      return rfb_keysym::kF12;
            case VK_NUMPAD0:  return '0';
            case VK_NUMPAD1:  return '1';
            case VK_NUMPAD2:  return '2';
            case VK_NUMPAD3:  return '3';
            case VK_NUMPAD4:  return '4';
            case VK_NUMPAD5:  return '5';
            case VK_NUMPAD6:  return '6';
            case VK_NUMPAD7:  return '7';
            case VK_NUMPAD8:  return '8';
            case VK_NUMPAD9:  return '9';
            case VK_MULTIPLY: return '*';
            case VK_ADD:      return '+';
            case VK_SUBTRACT: return '-';
            case VK_DECIMAL:  return '.';
            case VK_DIVIDE:   return '/';
            default: break;
        }

        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool caps  = (GetKeyState(VK_CAPITAL) & 0x0001) != 0;
        const bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool alt   = (GetKeyState(VK_MENU) & 0x8000) != 0;
        const bool win   = ((GetKeyState(VK_LWIN) & 0x8000) != 0) ||
                           ((GetKeyState(VK_RWIN) & 0x8000) != 0);

        if (!ctrl && !alt && !win) {
            BYTE key_state[256] = {};
            if (GetKeyboardState(key_state)) {
                wchar_t chars[4] = {};
                int rc = ToUnicode(vk, scancode, key_state, chars, 4, 0);
                if (rc == 1 && chars[0] >= 0x20 && chars[0] <= 0x7E) {
                    return static_cast<uint32_t>(chars[0]);
                }
            }
        }

        if (vk >= 'A' && vk <= 'Z') {
            const bool upper = shift ^ caps;
            return upper ? static_cast<uint32_t>(vk) : static_cast<uint32_t>(vk - 'A' + 'a');
        }
        if (vk >= '0' && vk <= '9') {
            if (!shift) return static_cast<uint32_t>(vk);
            static const char kShiftedDigits[10] = {')', '!', '@', '#', '$', '%', '^', '&', '*', '('};
            return static_cast<uint32_t>(kShiftedDigits[vk - '0']);
        }

        switch (vk) {
            case VK_OEM_MINUS:  return shift ? '_' : '-';
            case VK_OEM_PLUS:   return shift ? '+' : '=';
            case VK_OEM_4:      return shift ? '{' : '[';
            case VK_OEM_6:      return shift ? '}' : ']';
            case VK_OEM_5:      return shift ? '|' : '\\';
            case VK_OEM_1:      return shift ? ':' : ';';
            case VK_OEM_7:      return shift ? '"' : '\'';
            case VK_OEM_COMMA:  return shift ? '<' : ',';
            case VK_OEM_PERIOD: return shift ? '>' : '.';
            case VK_OEM_2:      return shift ? '?' : '/';
            case VK_OEM_3:      return shift ? '~' : '`';
            default: break;
        }
        return 0;
    }

    std::unordered_map<uint16_t, uint32_t> active_by_scan_;
};

}  // namespace mac_mirror
#endif
