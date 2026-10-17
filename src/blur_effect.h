#pragma once
#include <cwchar>
#include <windows.graphics.effects.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Effects.h>

// Native Windows effect description. No Win2D package or custom shader is used.
struct BlurEffect : winrt::implements<BlurEffect,
    winrt::Windows::Graphics::Effects::IGraphicsEffect,
    winrt::Windows::Graphics::Effects::IGraphicsEffectSource,
    ABI::Windows::Graphics::Effects::IGraphicsEffectD2D1Interop> {
    winrt::Windows::Graphics::Effects::IGraphicsEffectSource source{nullptr};
    winrt::hstring name{L"Blur"};
    winrt::hstring Name() const { return name; }
    void Name(winrt::hstring const& value) { name = value; }
    HRESULT __stdcall GetEffectId(GUID* id) noexcept override {
        if (!id) return E_POINTER;
        *id = {0x1feb6d69, 0x2fe6, 0x4ac9, {0x8c, 0x58, 0x1d, 0x7f, 0x93, 0xe7, 0xa6, 0xa5}};
        return S_OK;
    }
    HRESULT __stdcall GetNamedPropertyMapping(LPCWSTR property, UINT* index,
        ABI::Windows::Graphics::Effects::GRAPHICS_EFFECT_PROPERTY_MAPPING* mapping) noexcept override {
        if (!property || !index || !mapping) return E_POINTER;
        if (wcscmp(property, L"Amount") != 0) return E_INVALIDARG;
        *index = 0;
        *mapping = ABI::Windows::Graphics::Effects::GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT;
        return S_OK;
    }
    HRESULT __stdcall GetPropertyCount(UINT* count) noexcept override {
        if (!count) return E_POINTER;
        *count = 3;
        return S_OK;
    }
    HRESULT __stdcall GetProperty(UINT index, ABI::Windows::Foundation::IPropertyValue** value) noexcept override {
        if (!value) return E_POINTER;
        *value = nullptr;
        try {
            winrt::Windows::Foundation::IPropertyValue property{nullptr};
            using winrt::Windows::Foundation::PropertyValue;
            switch (index) {
            case 0: property = PropertyValue::CreateSingle(0.0f).as<winrt::Windows::Foundation::IPropertyValue>(); break;
            case 1: property = PropertyValue::CreateUInt32(1).as<winrt::Windows::Foundation::IPropertyValue>(); break;
            case 2: property = PropertyValue::CreateUInt32(1).as<winrt::Windows::Foundation::IPropertyValue>(); break;
            default: return E_INVALIDARG;
            }
            *value = reinterpret_cast<ABI::Windows::Foundation::IPropertyValue*>(winrt::detach_abi(property));
            return S_OK;
        } catch (...) { return winrt::to_hresult(); }
    }
    HRESULT __stdcall GetSourceCount(UINT* count) noexcept override {
        if (!count) return E_POINTER;
        *count = 1;
        return S_OK;
    }
    HRESULT __stdcall GetSource(UINT index, ABI::Windows::Graphics::Effects::IGraphicsEffectSource** value) noexcept override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (index != 0) return E_INVALIDARG;
        if (!source) return E_UNEXPECTED;
        winrt::copy_to_abi(source, *reinterpret_cast<void**>(value));
        return S_OK;
    }
};
