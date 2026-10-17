#pragma once
#include <windows.h>

inline constexpr wchar_t kXamlMapping[] = L"Local\\TaskbarCustomizer.Xaml.v10";
inline constexpr DWORD kXamlMagic = 0x4354583A;
inline constexpr wchar_t kBootstrapMessage[] = L"TaskbarCustomizer.Xaml.Bootstrap.v1";
inline constexpr CLSID kTapClsid = {0xb42cb2c1, 0x98ea, 0x4486, {0x9a, 0x18, 0xfa, 0xa7, 0xa0, 0x3f, 0x62, 0x71}};
// Aligned LONG fields are accessed with Interlocked APIs in both processes.
struct XamlControl {
    DWORD magic;
    volatile LONG enabled;
    volatile LONG blurPercent; // 0 clear, 100 Gaussian standard deviation 30 DIPs
    volatile LONG heartbeat;
    volatile LONG attachedPid;
    volatile LONG matched;
    volatile LONG changed;
    volatile LONG error;
    volatile LONG probeError;
};
inline LONG ReadShared(volatile LONG& value) { return InterlockedCompareExchange(&value, 0, 0); }
