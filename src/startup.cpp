#include "startup.h"
#include <string>

namespace {
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kValue[] = L"TaskbarCustomizer";

std::wstring Executable() {
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    return length && length < ARRAYSIZE(path) ? std::wstring(path, length) : std::wstring{};
}
}

bool StartupEnabled() {
    wchar_t command[261]{};
    DWORD size = sizeof(command);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kValue, RRF_RT_REG_SZ,
        nullptr, command, &size) != ERROR_SUCCESS) return false;
    const auto path = Executable();
    if (path.empty()) return false;
    const auto expected = L"\"" + path + L"\" --startup";
    // Recognize the previous version's command so users can remove that entry too.
    return command == expected || std::wstring(command).starts_with(expected + L" --blur=");
}

LSTATUS SetStartup(bool enabled) {
    HKEY key = nullptr;
    LSTATUS result;
    if (enabled) {
        const auto path = Executable();
        if (path.empty()) return ERROR_BAD_PATHNAME;
        const auto command = L"\"" + path + L"\" --startup";
        if (command.size() > 260) return ERROR_FILENAME_EXCED_RANGE;
        result = RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0,
            KEY_SET_VALUE, nullptr, &key, nullptr);
        if (result != ERROR_SUCCESS) return result;
        result = RegSetValueExW(key, kValue, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        result = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key);
        if (result == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
        if (result != ERROR_SUCCESS) return result;
        result = RegDeleteValueW(key, kValue);
        if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return result;
}
