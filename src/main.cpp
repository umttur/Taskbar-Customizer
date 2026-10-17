#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <cwchar>
#include <string>
#include "xaml_client.h"
#include "startup.h"
#include "settings.h"
#include "resource.h"

namespace {
constexpr UINT kTray = WM_APP + 1;
constexpr UINT_PTR kTimer = 1;
constexpr UINT kEnable = 100, kExit = 102, kAbout = 103;
constexpr UINT kPercent = 106;
constexpr UINT kStartup = 107;
constexpr int kMinus = 201, kPlus = 203;
HWND mainWindow = nullptr;
HICON appIcon = nullptr;
int blurPercent = 0;
UINT taskbarCreated = 0;
bool enabled = false, trayPresent = false;
bool startupPending = false;
ULONGLONG startupDeadline = 0;
std::wstring status = L"Paused";
std::wstring lastTrayTip;

bool IsHighContrast() {
    HIGHCONTRASTW hc{sizeof(hc)};
    // Fail closed if accessibility preferences cannot be read.
    return !SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0)
        || (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

bool IsTaskbar(HWND window) {
    if (!IsWindow(window)) return false;
    wchar_t name[128]{};
    if (!GetClassNameW(window, name, ARRAYSIZE(name))) return false;
    // Shell implementation details; secondary taskbars need enumeration.
    if (lstrcmpW(name, L"Shell_TrayWnd") && lstrcmpW(name, L"Shell_SecondaryTrayWnd"))
        return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    wchar_t path[32768]{};
    DWORD size = ARRAYSIZE(path);
    bool valid = false;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        wchar_t windows[MAX_PATH]{};
        if (GetWindowsDirectoryW(windows, ARRAYSIZE(windows))) {
            const std::wstring expected = std::wstring(windows) + L"\\explorer.exe";
            valid = lstrcmpiW(path, expected.c_str()) == 0;
        }
    }
    CloseHandle(process);
    return valid;
}

void UpdateTray(bool add = false) {
    NOTIFYICONDATAW icon{sizeof(icon)};
    icon.hWnd = mainWindow;
    icon.uID = 1;
    icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    icon.hIcon = appIcon;
    icon.uCallbackMessage = kTray;
    const std::wstring tip = L"Taskbar Customizer - " + status;
    if (!add && trayPresent && tip == lastTrayTip) return;
    lstrcpynW(icon.szTip, tip.c_str(), ARRAYSIZE(icon.szTip));
    trayPresent = Shell_NotifyIconW(add || !trayPresent ? NIM_ADD : NIM_MODIFY, &icon) != FALSE;
    if (trayPresent) lastTrayTip = tip;
}

void Restore() {
    PollXaml(false, status);
}

void Refresh() {
    const bool highContrast = IsHighContrast();
    PollXaml(enabled && !highContrast, status);
    if (highContrast) status = L"Suspended: high contrast or settings unavailable";
    UpdateTray();
}

void Attach() {
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!IsTaskbar(taskbar)) { status = L"Verified Explorer taskbar not found"; UpdateTray(); return; }
    DWORD pid = 0;
    GetWindowThreadProcessId(taskbar, &pid);
    RequestXamlAttach(pid);
    Refresh();
}

void PersistSettings() {
    const HRESULT result = SaveSettings({blurPercent, enabled});
    if (FAILED(result)) {
        wchar_t error[128]{};
        swprintf_s(error, L"Could not save settings.json (0x%08lX). Changes apply only to this session.",
            static_cast<unsigned long>(result));
        MessageBoxW(mainWindow, error, L"Taskbar Customizer", MB_OK | MB_ICONERROR);
    }
}

void ApplyPercent(int value) {
    blurPercent = std::clamp(value, 0, 100);
    SetXamlBlur(blurPercent);
    PersistSettings();
    Refresh();
}

void Menu() {
    const std::wstring percent = L"Blur: %" + std::to_wstring(blurPercent);
    const bool launchAtLogon = StartupEnabled();
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kEnable, enabled ? L"Disable" : L"Enable");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | MF_DISABLED, kPercent, percent.c_str());
    AppendMenuW(menu, MF_STRING | (blurPercent >= 100 ? MF_GRAYED : 0), kPlus, L"+10");
    AppendMenuW(menu, MF_STRING | (blurPercent <= 0 ? MF_GRAYED : 0), kMinus, L"−10");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (launchAtLogon ? MF_CHECKED : 0), kStartup, L"Start with Windows");
    AppendMenuW(menu, MF_STRING, kAbout, L"Status");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kExit, L"Exit");
    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(mainWindow);
    const UINT command = static_cast<UINT>(TrackPopupMenuEx(menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON, cursor.x, cursor.y, mainWindow, nullptr));
    DestroyMenu(menu);
    PostMessageW(mainWindow, WM_NULL, 0, 0);
    if (command) PostMessageW(mainWindow, WM_COMMAND, command, 0);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) noexcept try {
    if (taskbarCreated && message == taskbarCreated) {
        // Explorer has recreated its UI; stale handles must not be restored.
        trayPresent = false;
        UpdateTray(true);
        if (enabled) Attach();
        Refresh();
        return 0;
    }
    switch (message) {
    case kTray:
        if (lp == WM_LBUTTONUP) Menu();
        else if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) Menu();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case kEnable: startupPending = false; enabled = !enabled; PersistSettings(); if (enabled) Attach(); else { Restore(); UpdateTray(); } break;
        case kPlus: ApplyPercent(blurPercent + 10); break;
        case kMinus: ApplyPercent(blurPercent - 10); break;
        case kStartup: {
            const LSTATUS result = SetStartup(!StartupEnabled());
            if (result != ERROR_SUCCESS) {
                const auto error = L"Could not change startup setting. Windows error: " + std::to_wstring(result);
                MessageBoxW(window, error.c_str(), L"Taskbar Customizer", MB_OK | MB_ICONERROR);
            }
            break;
        }
        case kAbout: {
            Refresh();
            MessageBoxW(window, status.c_str(), L"Taskbar Customizer 1.0.0", MB_OK | MB_ICONINFORMATION);
            break;
        }
        case kExit: DestroyWindow(window); break;
        }
        return 0;
    case WM_TIMER:
        if (wp == kTimer) {
            if (startupPending) {
                if (IsTaskbar(FindWindowW(L"Shell_TrayWnd", nullptr))) {
                    startupPending = false;
                    Attach();
                } else if (GetTickCount64() >= startupDeadline) {
                    startupPending = false;
                    enabled = false;
                    status = L"Taskbar not ready; choose Enable to retry";
                    UpdateTray();
                    return 0;
                } else {
                    status = L"Waiting for Windows taskbar...";
                    UpdateTray();
                    return 0;
                }
            }
            Refresh();
        }
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_DWMCOMPOSITIONCHANGED: Refresh(); return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_APMRESUMEAUTOMATIC) Refresh();
        return TRUE;
    case WM_CLOSE: DestroyWindow(window); return 0;
    case WM_QUERYENDSESSION: enabled = false; Restore(); return TRUE;
    case WM_DESTROY: {
        KillTimer(window, kTimer);
        Restore();
        StopXaml();
        NOTIFYICONDATAW icon{sizeof(icon)};
        icon.hWnd = window;
        icon.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &icon);
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(window, message, wp, lp);
} catch (...) {
    // Do not let allocation failures escape through a Windows callback.
    enabled = false;
    StopXaml();
    KillTimer(window, kTimer);
    NOTIFYICONDATAW icon{sizeof(icon)};
    icon.hWnd = window;
    icon.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &icon);
    if (message != WM_DESTROY && IsWindow(window)) DestroyWindow(window);
    PostQuitMessage(1);
    return 0;
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\TaskbarCustomizer.SingleInstance");
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"Taskbar Customizer is already running in the notification area.", L"Taskbar Customizer", MB_OK);
        CloseHandle(mutex);
        return 0;
    }
    if (!InitializeXamlControl()) {
        MessageBoxW(nullptr, L"Could not create XAML control channel.", L"Taskbar Customizer", MB_OK | MB_ICONERROR);
        CloseHandle(mutex);
        return 1;
    }
    WNDCLASSW wc{};
    appIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_TASKBAR_CUSTOMIZER));
    if (!appIcon) { StopXaml(); CloseHandle(mutex); return 1; }
    wc.hIcon = appIcon;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"TaskbarCustomizer.MessageWindow";
    if (!RegisterClassW(&wc)) { StopXaml(); CloseHandle(mutex); return 1; }
    taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    // Hidden top-level window receives shell broadcasts (message-only windows do not).
    mainWindow = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Taskbar Customizer",
        WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!mainWindow) { StopXaml(); CloseHandle(mutex); return 1; }
    UpdateTray(true);
    if (!trayPresent || !SetTimer(mainWindow, kTimer, 1000, nullptr)) {
        MessageBoxW(nullptr, L"Could not initialize the tray icon or recovery timer.", L"Taskbar Customizer", MB_OK | MB_ICONERROR);
        DestroyWindow(mainWindow);
        CloseHandle(mutex);
        return 1;
    }
    Settings saved;
    const HRESULT loadResult = LoadSettings(saved);
    blurPercent = saved.blurPercent;
    enabled = saved.enabled;
    SetXamlBlur(blurPercent);
    if (loadResult == S_FALSE) {
        PersistSettings();
    } else if (FAILED(loadResult)) {
        MessageBoxW(mainWindow,
            L"Could not read settings.json. Using defaults (Blur 0, disabled). The file was not overwritten.",
            L"Taskbar Customizer", MB_OK | MB_ICONWARNING);
    }
    if (enabled) {
        startupPending = true;
        startupDeadline = GetTickCount64() + 60000;
    }
    MSG message{};
    int result;
    while ((result = static_cast<int>(GetMessageW(&message, nullptr, 0, 0))) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (result == -1 && IsWindow(mainWindow)) DestroyWindow(mainWindow);
    CloseHandle(mutex);
    return result == -1 ? 1 : 0;
}
