#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <winsock2.h>

#include <string>
#include <vector>

#include "cli_options.h"
#include "viewer_app.h"

namespace {

void EnablePerMonitorDpiAwareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == ((DPI_AWARENESS_CONTEXT)-4)
        using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);
        auto set_ctx = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
            reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
        if (set_ctx && set_ctx(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)))) {
            return;
        }
        // Fallback to DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE (-3)
        if (set_ctx && set_ctx(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-3)))) {
            return;
        }
    }

    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        using SetProcessDpiAwarenessFn = HRESULT(WINAPI*)(int);
        auto set_sh = reinterpret_cast<SetProcessDpiAwarenessFn>(
            reinterpret_cast<void*>(GetProcAddress(shcore, "SetProcessDpiAwareness")));
        if (set_sh && SUCCEEDED(set_sh(2))) {  // PROCESS_PER_MONITOR_DPI_AWARE = 2
            return;
        }
    }

    if (user32) {
        using SetProcessDPIAwareFn = BOOL(WINAPI*)();
        auto set_legacy = reinterpret_cast<SetProcessDPIAwareFn>(
            reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDPIAware")));
        if (set_legacy) {
            set_legacy();
        }
    }
}

std::string WideArgToUtf8(const wchar_t* w) {
    if (!w || !*w) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return std::string();
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], len - 1, nullptr, nullptr);
    return out;
}

void WriteConsoleOrPipeText(const std::string& text, bool is_error) {
    HANDLE h = GetStdHandle(is_error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    if (!h || h == INVALID_HANDLE_VALUE || GetFileType(h) == FILE_TYPE_UNKNOWN) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            h = GetStdHandle(is_error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
        }
    }
    if (h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN) {
        DWORD written = 0;
        WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    }
}

}  // namespace

int WINAPI WinMain(HINSTANCE hinstance, HINSTANCE /*prev_instance*/, LPSTR /*cmd_line*/, int /*show_cmd*/) {
    EnablePerMonitorDpiAwareness();

    WSADATA wsa = {};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    mac_mirror::AppOptions options;
    options.prefs = mac_mirror::LoadUiPrefsFromDisk();

    int argc = 0;
    LPWSTR* argv_w = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    if (argv_w && argc > 1) {
        args.reserve(static_cast<size_t>(argc - 1));
        for (int i = 1; i < argc; ++i) {
            args.push_back(WideArgToUtf8(argv_w[i]));
        }
    }
    if (argv_w) {
        LocalFree(argv_w);
    }

    const bool ok = mac_mirror::ParseCommandLineArgs(args, &options);
    for (std::string& a : args) {
        mac_mirror::SecureWipeString(&a);
    }

    if (!ok) {
        const std::string err = "Error: " + options.error_message + "\n\n" + mac_mirror::FormatUsageText();
        WriteConsoleOrPipeText(err, true);
        CoUninitialize();
        WSACleanup();
        return 2;
    }

    if (options.show_help) {
        WriteConsoleOrPipeText(mac_mirror::FormatUsageText(), false);
        CoUninitialize();
        WSACleanup();
        return 0;
    }

    int exit_code = 0;
    {
        mac_mirror::ViewerApp app(std::move(options));
        exit_code = app.Run(hinstance);
    }

    CoUninitialize();
    WSACleanup();
    return exit_code;
}
#endif
