#include "xaml_client.h"
#include "xaml_shared.h"
#include <algorithm>
#include <thread>
#include <atomic>
#include <cwchar>

namespace {
HANDLE mapping = nullptr;
XamlControl* control = nullptr;
std::atomic_bool connecting{false};
ULONGLONG attachStarted = 0;
}
bool InitializeXamlControl() {
    mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(XamlControl), kXamlMapping);
    if (!mapping) return false;
    const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    control = static_cast<XamlControl*>(MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(XamlControl)));
    if (!control) { CloseHandle(mapping); mapping = nullptr; return false; }
    if (!existed) { ZeroMemory(control, sizeof(*control)); control->magic = kXamlMagic; }
    if (control->magic != kXamlMagic) {
        UnmapViewOfFile(control);
        control = nullptr;
        CloseHandle(mapping);
        mapping = nullptr;
        return false;
    }
    InterlockedExchange(&control->enabled, 0);
    InterlockedExchange(&control->blurPercent, 0);
    InterlockedExchange(&control->heartbeat, static_cast<LONG>(GetTickCount()));
    return true;
}
void RequestXamlAttach(DWORD pid) {
    if (!control || connecting.load()) return;
    if (static_cast<DWORD>(ReadShared(control->attachedPid)) == pid) {
        // An explicit Enable can retry appearance changes without a second subscription.
        InterlockedExchange(&control->error, S_OK);
        return;
    }
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    if (!length || length >= ARRAYSIZE(path)) {
        InterlockedExchange(&control->error, HRESULT_FROM_WIN32(
            length ? ERROR_INSUFFICIENT_BUFFER : GetLastError()));
        return;
    }
    std::wstring dll(path, length);
    dll.resize(dll.find_last_of(L"\\/") + 1);
    dll += L"TaskbarCustomizerXaml.dll";
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        InterlockedExchange(&control->error, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
        return;
    }
    InterlockedExchange(&control->attachedPid, 0);
    InterlockedExchange(&control->matched, 0);
    InterlockedExchange(&control->changed, 0);
    InterlockedExchange(&control->error, S_FALSE); // Bootstrap queued, subscription not confirmed yet.
    InterlockedExchange(&control->probeError, S_OK);
    connecting = true;
    attachStarted = GetTickCount64();
    try {
        std::thread([pid, dll] {
            // Own mapping lifetime: the UI can exit while Windows is connecting.
            HANDLE ownMapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, kXamlMapping);
            auto state = ownMapping ? static_cast<XamlControl*>(MapViewOfFile(ownMapping,
                FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(XamlControl))) : nullptr;
            // Exiting while this worker is starting must not trigger a late attach.
            if (!state || state->magic != kXamlMagic || !ReadShared(state->heartbeat) ||
                static_cast<DWORD>(GetTickCount() - static_cast<DWORD>(ReadShared(state->heartbeat))) >= 5000) {
                if (state) UnmapViewOfFile(state);
                if (ownMapping) CloseHandle(ownMapping);
                connecting = false;
                return;
            }
            HRESULT hr = E_FAIL;
            HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
            HWND target = taskbar ? FindWindowExW(taskbar, nullptr,
                L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr) : nullptr;
            if (!target) target = taskbar;
            DWORD owner = 0;
            const DWORD threadId = target ? GetWindowThreadProcessId(target, &owner) : 0;
            HMODULE tapModule = nullptr;
            if (threadId && owner == pid) {
                tapModule = LoadLibraryExW(dll.c_str(), nullptr,
                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
                auto hookProc = tapModule ? reinterpret_cast<HOOKPROC>(
                    GetProcAddress(tapModule, "TaskbarCustomizerBootstrapHook")) : nullptr;
                hr = hookProc ? S_OK : HRESULT_FROM_WIN32(
                    tapModule ? ERROR_PROC_NOT_FOUND : GetLastError());
                if (hookProc) {
                    HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, hookProc, tapModule, threadId);
                    if (hook) {
                        const UINT message = RegisterWindowMessageW(kBootstrapMessage);
                        DWORD_PTR result = 0;
                        SetLastError(ERROR_SUCCESS);
                        if (!message || !SendMessageTimeoutW(target, message, 0, 0,
                            SMTO_ABORTIFHUNG | SMTO_BLOCK, 3000, &result)) {
                            const DWORD error = GetLastError();
                            hr = HRESULT_FROM_WIN32(error ? error : ERROR_TIMEOUT);
                        }
                        if (!UnhookWindowsHookEx(hook)) {
                            hr = HRESULT_FROM_WIN32(GetLastError());
                            // Keep our local module reference if the hook could still be active.
                            tapModule = nullptr;
                        }
                    } else hr = HRESULT_FROM_WIN32(GetLastError());
                }
            } else hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
            if (state) {
                if (FAILED(hr)) InterlockedExchange(&state->error, hr);
                UnmapViewOfFile(state);
            }
            if (ownMapping) CloseHandle(ownMapping);
            if (tapModule) FreeLibrary(tapModule);
            connecting = false;
        }).detach();
    } catch (...) {
        connecting = false;
        InterlockedExchange(&control->error, E_FAIL);
    }
}
void PollXaml(bool enabled, std::wstring& status) {
    if (!control) { status = L"XAML control unavailable"; return; }
    InterlockedExchange(&control->heartbeat, static_cast<LONG>(GetTickCount()));
    const HRESULT hr = ReadShared(control->error);
    InterlockedExchange(&control->enabled, enabled && hr == S_OK ? 1 : 0);
    if (connecting) { status = L"Connecting to Explorer XAML..."; return; }
    if (hr == S_FALSE) {
        status = attachStarted && GetTickCount64() - attachStarted > 20000
            ? L"Connection NOT confirmed after 20s; no success reported"
            : L"Connecting inside Explorer; subscription pending";
        return;
    }
    if (FAILED(hr)) {
        wchar_t text[80]{};
        swprintf_s(text, L"XAML error 0x%08lX; effect suspended", static_cast<unsigned long>(hr));
        status = text;
    } else if (!ReadShared(control->attachedPid)) status = L"Not connected; choose Enable";
    else if (!ReadShared(control->matched)) status = L"Connected, but TASKBAR NOT READY / NOT FOUND";
    else status = std::wstring(enabled ? L"Enabled; " : L"Paused; ")
        + std::to_wstring(ReadShared(control->matched)) + L" targets, "
        + std::to_wstring(ReadShared(control->changed)) + L" modified";
    if (!ReadShared(control->matched) && FAILED(ReadShared(control->probeError))) {
        wchar_t text[64]{};
        swprintf_s(text, L"\nDiscovery error 0x%08lX",
            static_cast<unsigned long>(ReadShared(control->probeError)));
        status += text;
    }
    status += L"\nBlur=" + std::to_wstring(ReadShared(control->blurPercent)) + L"% (1.0.0)";
}
void StopXaml() {
    if (control) {
        InterlockedExchange(&control->enabled, 0);
        InterlockedExchange(&control->heartbeat, 0);
        UnmapViewOfFile(control); control = nullptr;
    }
    if (mapping) { CloseHandle(mapping); mapping = nullptr; }
}
void SetXamlBlur(int percent) {
    if (control) InterlockedExchange(&control->blurPercent, std::clamp(percent, 0, 100));
}
