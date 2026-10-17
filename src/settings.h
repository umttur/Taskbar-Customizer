#pragma once
#include <windows.h>

struct Settings {
    int blurPercent = 0;
    bool enabled = false;
};

HRESULT LoadSettings(Settings& settings) noexcept;
HRESULT SaveSettings(const Settings& settings) noexcept;
