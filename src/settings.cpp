#include "settings.h"
#include <shlobj.h>
#include <roapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
struct Apartment {
    HRESULT result = RoInitialize(RO_INIT_SINGLETHREADED);
    Apartment() { if (result != RPC_E_CHANGED_MODE) winrt::check_hresult(result); }
    ~Apartment() { if (SUCCEEDED(result)) RoUninitialize(); }
};

std::filesystem::path SettingsPath() {
    PWSTR directory = nullptr;
    winrt::check_hresult(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &directory));
    struct Memory { PWSTR value; ~Memory() { CoTaskMemFree(value); } } memory{directory};
    return std::filesystem::path(directory) / L"TaskbarCustomizer" / L"settings.json";
}
}

HRESULT LoadSettings(Settings& settings) noexcept {
    settings = {};
    try {
        Apartment apartment;
        const auto path = SettingsPath();
        if (!std::filesystem::exists(path)) return S_FALSE;
        const auto size = std::filesystem::file_size(path);
        if (!size || size > 16384) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        std::ifstream file(path, std::ios::binary);
        std::string bytes(static_cast<size_t>(size), '\0');
        if (!file.read(bytes.data(), static_cast<std::streamsize>(size)))
            return HRESULT_FROM_WIN32(ERROR_READ_FAULT);
        // UTF-8 is mandatory. A UTF-8 BOM is accepted for hand-edited files.
        if (bytes.starts_with("\xEF\xBB\xBF")) bytes.erase(0, 3);
        const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        if (!length) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        std::wstring json(static_cast<size_t>(length), L'\0');
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
            static_cast<int>(bytes.size()), json.data(), length))
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        using namespace winrt::Windows::Data::Json;
        JsonObject object{nullptr};
        if (!JsonObject::TryParse(json, object)) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        if (object.GetNamedNumber(L"version") != 1) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        const double blur = object.GetNamedNumber(L"blurPercent");
        if (!std::isfinite(blur) || blur < 0 || blur > 100 || std::floor(blur) != blur)
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        Settings loaded{static_cast<int>(blur), object.GetNamedBoolean(L"enabled")};
        settings = loaded;
        return S_OK;
    } catch (...) { return winrt::to_hresult(); }
}

HRESULT SaveSettings(const Settings& settings) noexcept {
    std::filesystem::path temporary;
    HANDLE output = INVALID_HANDLE_VALUE;
    bool ownsTemporary = false;
    try {
        if (settings.blurPercent < 0 || settings.blurPercent > 100) return E_INVALIDARG;
        Apartment apartment;
        const auto path = SettingsPath();
        std::filesystem::create_directories(path.parent_path());
        using namespace winrt::Windows::Data::Json;
        JsonObject object;
        object.Insert(L"version", JsonValue::CreateNumberValue(1));
        object.Insert(L"blurPercent", JsonValue::CreateNumberValue(settings.blurPercent));
        object.Insert(L"enabled", JsonValue::CreateBooleanValue(settings.enabled));
        const auto bytes = winrt::to_string(object.Stringify());
        temporary = path;
        temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L"."
            + std::to_wstring(GetTickCount64()) + L".tmp";
        // Never truncate an existing temporary file or follow a pre-created link.
        output = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (output == INVALID_HANDLE_VALUE) winrt::throw_hresult(HRESULT_FROM_WIN32(GetLastError()));
        ownsTemporary = true;
        DWORD written = 0;
        if (!WriteFile(output, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr))
            winrt::throw_hresult(HRESULT_FROM_WIN32(GetLastError()));
        if (written != bytes.size()) winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT));
        if (!FlushFileBuffers(output)) winrt::throw_hresult(HRESULT_FROM_WIN32(GetLastError()));
        CloseHandle(output);
        output = INVALID_HANDLE_VALUE;
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            winrt::throw_hresult(HRESULT_FROM_WIN32(GetLastError()));
        return S_OK;
    } catch (...) {
        const HRESULT result = winrt::to_hresult();
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
        if (ownsTemporary) DeleteFileW(temporary.c_str());
        return result;
    }
}
