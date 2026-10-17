#include <windows.h>
#include <ocidl.h>
#include <xamlom.h>
// Win32's GetCurrentTime macro conflicts with XAML animation method names.
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif
#include <winrt/base.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Foundation.Collections.h>
#include "blur_effect.h"
#include <vector>
#include <algorithm>
#include <string>
#include <thread>
#include <memory>
#include "xaml_shared.h"

namespace {
namespace xaml = winrt::Windows::UI::Xaml;
// Intentionally resident until Explorer exits: unloading with XAML callbacks alive is unsafe.
HANDLE mapping = nullptr;
XamlControl* control = nullptr;
HINSTANCE moduleInstance = nullptr;
volatile LONG bootstrapStarted = 0;

DWORD WINAPI Bootstrap(void*) noexcept {
    HRESULT hr = E_FAIL;
    HMODULE xamlModule = nullptr;
    try {
        wchar_t path[32768]{};
        const DWORD length = GetModuleFileNameW(moduleInstance, path, ARRAYSIZE(path));
        if (!length) winrt::throw_hresult(HRESULT_FROM_WIN32(GetLastError()));
        if (length >= ARRAYSIZE(path)) winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));
        xamlModule = LoadLibraryExW(L"Windows.UI.Xaml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        auto initialize = xamlModule ? reinterpret_cast<decltype(&InitializeXamlDiagnosticsEx)>(
            GetProcAddress(xamlModule, "InitializeXamlDiagnosticsEx")) : nullptr;
        hr = initialize ? HRESULT_FROM_WIN32(ERROR_NOT_FOUND) : E_NOINTERFACE;
        if (initialize) {
            for (unsigned endpoint = 1; endpoint <= 8; ++endpoint) {
                std::thread attempt([&] {
                    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                    if (FAILED(com)) { hr = com; return; }
                    try {
                        const auto name = L"VisualDiagConnection" + std::to_wstring(endpoint);
                        hr = initialize(name.c_str(), GetCurrentProcessId(), nullptr, path, kTapClsid, kXamlMapping);
                    } catch (...) { hr = winrt::to_hresult(); }
                    CoUninitialize();
                });
                attempt.join();
                // One diagnostic subscription: later successful attaches can replace earlier ones.
                if (SUCCEEDED(hr)) break;
            }
        }
    } catch (...) { hr = winrt::to_hresult(); }
    if (xamlModule) FreeLibrary(xamlModule);
    if (FAILED(hr)) {
        HANDLE shared = OpenFileMappingW(FILE_MAP_WRITE, FALSE, kXamlMapping);
        if (shared) {
            auto state = static_cast<XamlControl*>(MapViewOfFile(shared, FILE_MAP_WRITE, 0, 0, sizeof(XamlControl)));
            if (state) { InterlockedExchange(&state->error, hr); UnmapViewOfFile(state); }
            CloseHandle(shared);
        }
        InterlockedExchange(&bootstrapStarted, 0);
    }
    return 0;
}
struct Target {
    InstanceHandle handle;
    xaml::Shapes::Shape shape{nullptr};
    xaml::Media::Brush original{nullptr};
    xaml::Media::Brush transparent{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual blurVisual{nullptr};
    winrt::Windows::UI::Composition::CompositionEffectBrush blurBrush{nullptr};
    bool changed = false;
    bool removed = false;
    int appliedBlur = -1;
};
struct ThreadTargets {
    std::vector<std::shared_ptr<Target>> targets;
    UINT_PTR timer = 0;
    bool ticking = false;
};
thread_local ThreadTargets local;

bool Requested() {
    if (!control || !ReadShared(control->enabled) || ReadShared(control->error) != S_OK) return false;
    const DWORD last = static_cast<DWORD>(ReadShared(control->heartbeat));
    return static_cast<DWORD>(GetTickCount() - last) < 5000;
}
void RemoveBlur(Target& target) {
    using Preview = xaml::Hosting::ElementCompositionPreview;
    if (target.blurBrush) target.blurBrush.Properties().InsertScalar(L"Blur.Amount", 0.0f);
    if (target.blurVisual) {
        if (Preview::GetElementChildVisual(target.shape) == target.blurVisual)
            Preview::SetElementChildVisual(target.shape, nullptr);
        target.blurVisual.Brush(nullptr);
    }
    target.blurVisual = nullptr;
    target.blurBrush = nullptr;
    target.appliedBlur = -1;
}
void RestoreTarget(Target& target) {
    HRESULT error = S_OK;
    try { RemoveBlur(target); }
    catch (...) { error = winrt::to_hresult(); }
    // A composition failure must not prevent restoration of the XAML fill.
    try {
        if (target.shape.Fill() == target.transparent) target.shape.Fill(target.original);
    } catch (...) { if (SUCCEEDED(error)) error = winrt::to_hresult(); }
    winrt::check_hresult(error);
    if (target.changed) {
        target.changed = false;
        InterlockedDecrement(&control->changed);
    }
}
void Tick() noexcept {
    if (local.ticking) return;
    local.ticking = true;
    struct Reset { ~Reset() { local.ticking = false; } } reset;
    try {
        const LONG percent = control ? std::clamp(ReadShared(control->blurPercent), LONG{0}, LONG{100}) : 0;
        // XAML setters can synchronously reenter mutation callbacks. Keep targets
        // alive and the iteration stable if those callbacks add or remove shapes.
        const auto targets = local.targets;
        for (const auto& entry : targets) {
            auto& target = *entry;
            if (target.removed) continue;
            try {
                const bool requested = Requested();
                if (!requested && !target.changed) continue;
                auto current = target.shape.Fill();
                if (requested) {
                    // Include partial changes in the next timer's rollback on failure.
                    if (!target.changed) {
                        target.changed = true;
                        InterlockedIncrement(&control->changed);
                    }
                    using Preview = xaml::Hosting::ElementCompositionPreview;
                    // Only BackgroundFill samples the backdrop. Blurring the border too
                    // would sample the fill again and apply the effect twice.
                    const bool needsBlur = percent > 0 && target.shape.Name() == L"BackgroundFill";
                    if (needsBlur && !target.blurVisual) {
                        if (Preview::GetElementChildVisual(target.shape))
                            winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
                        auto compositor = Preview::GetElementVisual(target.shape).Compositor();
                        auto effect = winrt::make_self<BlurEffect>();
                        effect->source = winrt::Windows::UI::Composition::CompositionEffectSourceParameter(L"Backdrop");
                        auto factory = compositor.CreateEffectFactory(
                            effect.as<winrt::Windows::Graphics::Effects::IGraphicsEffect>(), {L"Blur.Amount"});
                        auto brush = factory.CreateBrush();
                        brush.SetSourceParameter(L"Backdrop", compositor.CreateBackdropBrush());
                        auto visual = compositor.CreateSpriteVisual();
                        visual.RelativeSizeAdjustment({1.0f, 1.0f});
                        visual.Clip(compositor.CreateInsetClip());
                        visual.Brush(brush);
                        target.blurBrush = brush;
                        target.blurVisual = visual;
                    }
                    if (needsBlur) {
                        // 0..100 maps to Gaussian standard deviation 0..30 DIPs.
                        if (target.appliedBlur != percent) {
                            target.blurBrush.Properties().InsertScalar(L"Blur.Amount", static_cast<float>(percent) * 0.3f);
                            target.appliedBlur = static_cast<int>(percent);
                        }
                        const auto child = Preview::GetElementChildVisual(target.shape);
                        if (child != target.blurVisual) {
                            if (child) winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
                            Preview::SetElementChildVisual(target.shape, target.blurVisual);
                        }
                    } else {
                        // At zero, release the entire effect instead of keeping it cached.
                        RemoveBlur(target);
                    }
                    if (current != target.transparent) {
                        target.original = current;
                        target.shape.Fill(target.transparent);
                    }
                } else if (target.changed) {
                    RestoreTarget(target);
                }
                if (target.removed) RestoreTarget(target);
            } catch (...) {
                if (control) {
                    InterlockedExchange(&control->error, winrt::to_hresult());
                    InterlockedExchange(&control->enabled, 0);
                }
            }
        }
    } catch (...) { /* Never let a C++ exception escape into Explorer. */ }
}
void CALLBACK Timer(HWND, UINT, UINT_PTR, DWORD) noexcept { Tick(); }

bool InTaskbar(xaml::FrameworkElement element) {
    // Bound the walk, require an actual Taskbar.TaskbarFrame ancestor.
    for (unsigned depth = 0; element && depth < 32; ++depth) {
        if (winrt::get_class_name(element) == L"Taskbar.TaskbarFrame") return true;
        element = xaml::Media::VisualTreeHelper::GetParent(element).try_as<xaml::FrameworkElement>();
    }
    return false;
}

// XAML objects have UI-thread affinity. Do not advertise this callback as agile.
struct Tap : winrt::implements<Tap, IObjectWithSite, IVisualTreeServiceCallback2, winrt::non_agile> {
    winrt::com_ptr<IXamlDiagnostics> diagnostics;
    winrt::com_ptr<IVisualTreeService3> service;
    HWND dispatchWindow = nullptr;
    DWORD uiThread = 0;
    unsigned probes = 0;
    static constexpr UINT kSnapshot = WM_APP + 42;
    static constexpr UINT_PTR kProbeTimer = 17;

    // All discovery routes use the same strict filter and duplicate check.
    void Track(InstanceHandle handle, const winrt::Windows::Foundation::IInspectable& object) {
        auto shape = object.try_as<xaml::Shapes::Shape>();
        if (!shape) return;
        const auto name = shape.Name();
        if (name != L"BackgroundFill" && name != L"BackgroundStroke") return;
        if (!InTaskbar(shape)) return;
        for (const auto& target : local.targets) if (target->shape == shape) return;
        if (!local.timer) {
            local.timer = SetTimer(nullptr, 0, 500, Timer);
            if (!local.timer) winrt::throw_hresult(HRESULT_FROM_WIN32(GetLastError()));
        }
        xaml::Media::SolidColorBrush brush;
        brush.Color(winrt::Windows::UI::Color{0, 24, 24, 24});
        local.targets.push_back(std::make_shared<Target>(Target{handle, shape, shape.Fill(), brush}));
        InterlockedIncrement(&control->matched);
        Tick();
    }

    void ProbeRect(RECT rect) {
        unsigned count = 0;
        InstanceHandle* handles = nullptr;
        const HRESULT hr = diagnostics->HitTest(rect, &count, &handles);
        // XAML OM returns COM-allocated arrays. Always free, including on failure.
        struct Array { InstanceHandle* data; ~Array() { CoTaskMemFree(data); } } cleanup{handles};
        InterlockedExchange(&control->probeError, hr);
        if (FAILED(hr)) return;
        if (!handles && count) winrt::throw_hresult(E_UNEXPECTED);
        for (unsigned index = 0; index < count && index < 4096; ++index) {
            try {
                winrt::Windows::Foundation::IInspectable object{nullptr};
                if (SUCCEEDED(diagnostics->GetIInspectableFromHandle(handles[index],
                    reinterpret_cast<::IInspectable**>(winrt::put_abi(object))))) Track(handles[index], object);
            } catch (...) {
                InterlockedExchange(&control->probeError, winrt::to_hresult());
            }
        }
    }

    void Snapshot() noexcept {
        if (!control || GetCurrentThreadId() != uiThread) return;
        const DWORD heartbeat = static_cast<DWORD>(ReadShared(control->heartbeat));
        if (static_cast<DWORD>(GetTickCount() - heartbeat) >= 5000 ||
            ReadShared(control->matched) > 0 || probes >= 3) {
            KillTimer(dispatchWindow, kProbeTimer);
            return;
        }
        ++probes;
        try {
            // Discover actual taskbar rectangles inside this Explorer process.
            std::vector<RECT> rectangles;
            EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
                DWORD pid = 0;
                GetWindowThreadProcessId(window, &pid);
                if (pid != GetCurrentProcessId()) return TRUE;
                wchar_t type[128]{};
                GetClassNameW(window, type, ARRAYSIZE(type));
                if (lstrcmpW(type, L"Shell_TrayWnd") && lstrcmpW(type, L"Shell_SecondaryTrayWnd")) return TRUE;
                RECT rect{};
                if (GetWindowRect(window, &rect)) {
                    try { reinterpret_cast<std::vector<RECT>*>(parameter)->push_back(rect); }
                    catch (...) { return FALSE; }
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&rectangles));
            for (const auto& rect : rectangles) {
                ProbeRect(rect);
                // SDK does not specify the rectangle coordinate space. Also try
                // host-local bounds; the strict ancestor filter remains in place.
                const RECT localRect{0, 0, rect.right - rect.left, rect.bottom - rect.top};
                ProbeRect(localRect);
            }
        } catch (...) { InterlockedExchange(&control->probeError, winrt::to_hresult()); }
        if (ReadShared(control->matched) > 0 || probes >= 3) KillTimer(dispatchWindow, kProbeTimer);
    }

    static LRESULT CALLBACK Dispatch(HWND window, UINT message, WPARAM wp, LPARAM lp) noexcept {
        auto self = reinterpret_cast<Tap*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto creation = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = static_cast<Tap*>(creation->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && message == kSnapshot) {
            self->Snapshot();
            if (self->probes < 3 && control && !ReadShared(control->matched)) {
                if (!SetTimer(window, kProbeTimer, 1500, nullptr))
                    InterlockedExchange(&control->probeError, HRESULT_FROM_WIN32(GetLastError()));
            }
            return 0;
        }
        if (self && message == WM_TIMER && wp == kProbeTimer) { self->Snapshot(); return 0; }
        return DefWindowProcW(window, message, wp, lp);
    }

    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) noexcept override {
        try {
            if (!site) return S_OK; // Module remains resident; heartbeat controls restoration.
            if (diagnostics) return E_UNEXPECTED;
            winrt::check_hresult(site->QueryInterface(__uuidof(IXamlDiagnostics), diagnostics.put_void()));
            winrt::check_hresult(site->QueryInterface(__uuidof(IVisualTreeService3), service.put_void()));
            if (!control) {
                mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, kXamlMapping);
                if (!mapping) return HRESULT_FROM_WIN32(GetLastError());
                control = static_cast<XamlControl*>(MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(XamlControl)));
                if (!control) {
                    const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
                    CloseHandle(mapping);
                    mapping = nullptr;
                    return hr;
                }
                if (control->magic != kXamlMagic) {
                    UnmapViewOfFile(control);
                    control = nullptr;
                    CloseHandle(mapping);
                    mapping = nullptr;
                    return E_INVALIDARG;
                }
            }
            uiThread = GetCurrentThreadId();
            APTTYPE apartment;
            APTTYPEQUALIFIER qualifier;
            winrt::check_hresult(CoGetApartmentType(&apartment, &qualifier));
            if (apartment != APTTYPE_STA && apartment != APTTYPE_MAINSTA)
                winrt::throw_hresult(RPC_E_WRONG_THREAD);
            WNDCLASSW wc{};
            wc.hInstance = moduleInstance;
            wc.lpfnWndProc = Dispatch;
            wc.lpszClassName = L"TaskbarCustomizer.Xaml.Dispatch.v5";
            if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
                winrt::throw_hresult(HRESULT_FROM_WIN32(GetLastError()));
            dispatchWindow = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                HWND_MESSAGE, nullptr, moduleInstance, this);
            if (!dispatchWindow) winrt::throw_hresult(HRESULT_FROM_WIN32(GetLastError()));
            // The resident UI window owns a reference. Explicit unload is unsupported.
            AddRef();
            // Subscribe away from SetSite's call stack to avoid reentrant XAML initialization.
            // Advise has its own UI dispatch. A COM apartment proxy may send
            // this blocking call back to the UI. Use a direct worker subscription
            // for this XAML API only, not arbitrary non-agile COM interfaces.
            AddRef();
            HANDLE thread = CreateThread(nullptr, 0, [](void* context) -> DWORD {
                auto self = static_cast<Tap*>(context);
                HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                const bool initialized = SUCCEEDED(hr);
                if (initialized) {
                    hr = self->service->AdviseVisualTreeChange(self);
                }
                if (control) {
                    if (SUCCEEDED(hr)) {
                        // Do not erase an error reported by a callback during subscription.
                        InterlockedCompareExchange(&control->error, S_OK, S_FALSE);
                        InterlockedExchange(&control->attachedPid, static_cast<LONG>(GetCurrentProcessId()));
                        if (!PostMessageW(self->dispatchWindow, kSnapshot, 0, 0))
                            InterlockedExchange(&control->probeError, HRESULT_FROM_WIN32(GetLastError()));
                    } else if (!ReadShared(control->attachedPid)) {
                        InterlockedExchange(&control->error, hr);
                    }
                }
                // Keep the successful subscription. The service owns its callback
                // reference; do not re-subscribe merely because the snapshot is empty.

                if (initialized) CoUninitialize();
                self->Release();
                return 0;
            }, this, 0, nullptr);
            if (!thread) {
                const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());

                Release(); return hr;
            }
            CloseHandle(thread);
            return S_OK;
        } catch (...) { return winrt::to_hresult(); }
    }
    HRESULT STDMETHODCALLTYPE GetSite(REFIID iid, void** output) noexcept override {
        if (!output) return E_POINTER;
        *output = nullptr;
        return diagnostics ? diagnostics->QueryInterface(iid, output) : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE OnElementStateChanged(InstanceHandle, VisualElementState, LPCWSTR) noexcept override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnVisualTreeChange(ParentChildRelation, VisualElement element,
                                                  VisualMutationType mutation) noexcept override {
        // Strings are transferred to the callback by the diagnostics API.
        struct Strings {
            VisualElement& value;
            bool owns;
            ~Strings() {
                if (!owns) return;
                SysFreeString(value.Name); SysFreeString(value.Type);
                SysFreeString(value.SrcInfo.FileName); SysFreeString(value.SrcInfo.Hash);
            }
        } strings{element, mutation == Add};
        try {
            if (!control) return S_OK;
            if (mutation == Remove) {
                const auto found = std::find_if(local.targets.begin(), local.targets.end(), [&](const auto& target) {
                    return target->handle == element.Handle;
                });
                if (found != local.targets.end()) {
                    const auto target = *found;
                    target->removed = true;
                    local.targets.erase(found);
                    // Erase first: restoration can itself invoke mutation callbacks.
                    if (target->changed) {
                        try { RestoreTarget(*target); }
                        catch (...) { InterlockedExchange(&control->error, winrt::to_hresult()); }
                    }
                    if (target->changed) {
                        target->changed = false;
                        InterlockedDecrement(&control->changed);
                    }
                    InterlockedDecrement(&control->matched);
                }
                if (local.targets.empty() && local.timer) {
                    KillTimer(nullptr, local.timer);
                    local.timer = 0;
                }
                return S_OK;
            }
            if (mutation != Add) return S_OK;
            if (!element.Name) return S_OK;
            if (lstrcmpW(element.Name, L"BackgroundFill") && lstrcmpW(element.Name, L"BackgroundStroke")) return S_OK;
            winrt::Windows::Foundation::IInspectable object{nullptr};
            winrt::check_hresult(diagnostics->GetIInspectableFromHandle(element.Handle,
                reinterpret_cast<::IInspectable**>(winrt::put_abi(object))));
            Track(element.Handle, object);
            return S_OK;
        } catch (...) {
            const HRESULT hr = winrt::to_hresult();
            if (control) InterlockedExchange(&control->error, hr);
            return S_OK; // Report through shared state, do not fail Explorer's mutation callback.
        }
    }
};

struct Factory : winrt::implements<Factory, IClassFactory> {
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** output) noexcept override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        try { return winrt::make_self<Tap>()->QueryInterface(iid, output); }
        catch (...) { return winrt::to_hresult(); }
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) noexcept override { return S_OK; }
};
}
extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID iid, void** output) {
    if (!output) return E_POINTER;
    *output = nullptr;
    if (clsid != kTapClsid) return CLASS_E_CLASSNOTAVAILABLE;
    try { return winrt::make_self<Factory>()->QueryInterface(iid, output); }
    catch (...) { return winrt::to_hresult(); }
}
extern "C" HRESULT __stdcall DllCanUnloadNow() { return S_FALSE; }
extern "C" LRESULT CALLBACK TaskbarCustomizerBootstrapHook(int code, WPARAM wp, LPARAM lp) noexcept {
    if (code == HC_ACTION) {
        const auto message = reinterpret_cast<const CWPSTRUCT*>(lp);
        const UINT bootstrapMessage = RegisterWindowMessageW(kBootstrapMessage);
        DWORD pid = 0;
        GetWindowThreadProcessId(message->hwnd, &pid);
        wchar_t className[128]{};
        GetClassNameW(GetAncestor(message->hwnd, GA_ROOT), className, ARRAYSIZE(className));
        const bool taskbar = !lstrcmpW(className, L"Shell_TrayWnd") || !lstrcmpW(className, L"Shell_SecondaryTrayWnd");
        if (bootstrapMessage && message->message == bootstrapMessage && pid == GetCurrentProcessId()
            && taskbar && InterlockedCompareExchange(&bootstrapStarted, 1, 0) == 0) {
            // Keep callback/worker code valid after the short-lived Windows hook is removed.
            HMODULE pinned = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&moduleInstance), &pinned)) {
                HANDLE thread = CreateThread(nullptr, 0, Bootstrap, nullptr, 0, nullptr);
                if (thread) CloseHandle(thread);
                else InterlockedExchange(&bootstrapStarted, 0);
            } else InterlockedExchange(&bootstrapStarted, 0);
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) moduleInstance = instance;
    return TRUE;
}
