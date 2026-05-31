/*
 * VoxGuard — Voice-volume coaching tool
 * main.cpp — Phase 0+1: Win32 window, WebView2 shell, bridge, mic capture, live dBFS meter
 *
 * Threading model:
 *   UI thread    — Win32 message loop, WebView2 callbacks, all COM calls
 *   Audio thread — miniaudio real-time capture callback (never touches COM/WebView)
 *
 * Shared state between threads: g_currentDbfs (std::atomic<float>).
 * A WM_TIMER on the UI thread reads it and posts the value to JS every 50 ms.
 *
 * Audio runs in WASAPI SHARED mode so Discord and the game can share the mic device.
 * Buffer size ~10 ms (miniaudio default for WASAPI shared) → end-to-end latency < 15 ms.
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wrl/client.h>
#include <wrl/event.h>
#include <WebView2.h>
#include <string>
#include <sstream>
#include <atomic>
#include <cmath>
#include <vector>

// miniaudio — implementation defined in exactly one translation unit
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;

// ─────────────────────────────────────────────────────────────────
// Globals
// ─────────────────────────────────────────────────────────────────

static HWND g_hwnd = nullptr;
static ComPtr<ICoreWebView2Controller> g_controller;
static ComPtr<ICoreWebView2>           g_webview;

// Audio state — written by audio thread, read by UI thread
static std::atomic<float> g_currentDbfs{-60.0f};
static ma_device          g_captureDevice{};
static bool               g_audioActive = false;

// ─────────────────────────────────────────────────────────────────
// Audio — capture callback (real-time thread, no COM, no malloc)
// ─────────────────────────────────────────────────────────────────

static void AudioCaptureCallback(ma_device* /*dev*/,
                                  void*       /*out*/,
                                  const void* pInput,
                                  ma_uint32   frameCount)
{
    if (!pInput || frameCount == 0) return;
    const float* s = static_cast<const float*>(pInput);

    // RMS of the callback buffer
    double sumSq = 0.0;
    for (ma_uint32 i = 0; i < frameCount; ++i) {
        double v = s[i];
        sumSq += v * v;
    }
    float rms  = static_cast<float>(std::sqrt(sumSq / frameCount));

    // dBFS: 0 dB = amplitude 1.0 (full scale); clamp to [-60, 0]
    float dbfs = (rms > 1e-7f) ? 20.0f * std::log10(rms) : -60.0f;
    g_currentDbfs.store(std::max(-60.0f, std::min(0.0f, dbfs)),
                        std::memory_order_relaxed);
}

// ─────────────────────────────────────────────────────────────────
// Audio — device management (called from UI thread)
// ─────────────────────────────────────────────────────────────────

static std::wstring NarrowToWide(const char* s)
{
    if (!s || !*s) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring ws(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, ws.data(), n);
    if (!ws.empty() && ws.back() == L'\0') ws.pop_back();
    return ws;
}

// Build a JSON array of capture device names for the UI dropdown
static std::wstring GetCaptureDevicesJson()
{
    ma_context ctx;
    if (ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS)
        return L"[]";

    ma_device_info* pCapture = nullptr;
    ma_uint32       captureCount = 0;
    ma_context_get_devices(&ctx, nullptr, nullptr, &pCapture, &captureCount);

    std::wostringstream json;
    json << L"[";
    for (ma_uint32 i = 0; i < captureCount; ++i) {
        if (i > 0) json << L",";
        // Escape backslashes/quotes in device name (rare but safe)
        std::wstring name = NarrowToWide(pCapture[i].name);
        for (auto& c : name) if (c == L'"') c = L'\'';
        json << L"{\"id\":" << i
             << L",\"name\":\"" << name << L"\""
             << L",\"isDefault\":" << (pCapture[i].isDefault ? L"true" : L"false")
             << L"}";
    }
    json << L"]";

    ma_context_uninit(&ctx);
    return json.str();
}

// Start capture on the device at index `deviceIndex` (-1 = system default)
static bool StartAudioCapture(int deviceIndex)
{
    if (g_audioActive) {
        ma_device_uninit(&g_captureDevice);
        g_audioActive = false;
    }

    ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
    cfg.capture.format   = ma_format_f32;   // float samples [-1, 1]
    cfg.capture.channels = 1;               // mono — only need loudness
    cfg.sampleRate       = 48000;           // standard for gaming headsets
    cfg.dataCallback     = AudioCaptureCallback;
    // WASAPI shared mode is the default on Windows — coexists with Discord/game.
    // miniaudio chooses ~10 ms buffer in shared mode; leave as default.

    if (deviceIndex >= 0) {
        // Enumerate to get the specific device ID
        ma_context ctx;
        if (ma_context_init(nullptr, 0, nullptr, &ctx) == MA_SUCCESS) {
            ma_device_info* pCap = nullptr;
            ma_uint32 capCount = 0;
            ma_context_get_devices(&ctx, nullptr, nullptr, &pCap, &capCount);
            if (deviceIndex < static_cast<int>(capCount))
                cfg.capture.pDeviceID = &pCap[deviceIndex].id;
            // (pCap data stays valid until context uninit, but we init device first)
            if (ma_device_init(&ctx, &cfg, &g_captureDevice) != MA_SUCCESS) {
                ma_context_uninit(&ctx);
                return false;
            }
            ma_context_uninit(&ctx);
        }
    } else {
        if (ma_device_init(nullptr, &cfg, &g_captureDevice) != MA_SUCCESS)
            return false;
    }

    if (ma_device_start(&g_captureDevice) != MA_SUCCESS) {
        ma_device_uninit(&g_captureDevice);
        return false;
    }
    g_audioActive = true;
    return true;
}

// ─────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────

static void ResizeWebView()
{
    if (!g_controller) return;
    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    g_controller->put_Bounds(rc);
}

static std::wstring GetExeDir()
{
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    auto pos = p.rfind(L'\\');
    return (pos != std::wstring::npos) ? p.substr(0, pos) : p;
}

// Escape a wstring for embedding in a JSON string value
static std::wstring JsonEscape(const std::wstring& s)
{
    std::wstring out;
    out.reserve(s.size() + 4);
    for (wchar_t c : s) {
        switch (c) {
        case L'\\': out += L"\\\\"; break;
        case L'"':  out += L"\\\""; break;
        case L'\n': out += L"\\n";  break;
        case L'\r': out += L"\\r";  break;
        default:    out += c;
        }
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────
// Bridge: handle incoming messages from JS
// ─────────────────────────────────────────────────────────────────

static HRESULT HandleJsMessage(ICoreWebView2* wv,
                                ICoreWebView2WebMessageReceivedEventArgs* args)
{
    LPWSTR raw = nullptr;
    args->TryGetWebMessageAsString(&raw);
    if (!raw) return S_OK;
    std::wstring msg(raw);
    CoTaskMemFree(raw);

    // Simple type-based dispatch — Phase 0 uses "ping", Phase 1 adds "startCapture"
    if (msg.find(L"\"type\":\"ping\"") != std::wstring::npos) {
        // Bridge round-trip test
        wv->PostWebMessageAsJson(L"{\"type\":\"pong\",\"ok\":true}");

    } else if (msg.find(L"\"type\":\"getDevices\"") != std::wstring::npos) {
        // JS asked for the list of capture devices
        std::wstring resp = L"{\"type\":\"deviceList\",\"devices\":";
        resp += GetCaptureDevicesJson();
        resp += L"}";
        wv->PostWebMessageAsJson(resp.c_str());

    } else if (msg.find(L"\"type\":\"startCapture\"") != std::wstring::npos) {
        // JS asked to start capture on a device index
        // Parse "deviceIndex": N  (simple manual parse — no JSON lib needed)
        int idx = -1;
        auto pos = msg.find(L"\"deviceIndex\":");
        if (pos != std::wstring::npos) {
            pos += 14; // skip key + colon
            while (pos < msg.size() && (msg[pos] == L' ' || msg[pos] == L'\t')) ++pos;
            idx = std::stoi(msg.substr(pos));
        }
        bool ok = StartAudioCapture(idx);
        wchar_t resp[64];
        swprintf_s(resp, L"{\"type\":\"captureStarted\",\"ok\":%s}", ok ? L"true" : L"false");
        wv->PostWebMessageAsJson(resp);
    }

    return S_OK;
}

// ─────────────────────────────────────────────────────────────────
// WebView2 initialisation (asynchronous — callbacks run on UI thread)
// ─────────────────────────────────────────────────────────────────

static void InitWebView(std::wstring htmlFilePath)
{
    std::wstring url = L"file:///";
    url += htmlFilePath;
    for (auto& c : url) if (c == L'\\') c = L'/';

    CreateCoreWebView2EnvironmentWithOptions(
        nullptr, nullptr, nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [url](HRESULT, ICoreWebView2Environment* env) -> HRESULT {
                return env->CreateCoreWebView2Controller(
                    g_hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [url](HRESULT, ICoreWebView2Controller* ctrl) -> HRESULT {
                            g_controller = ctrl;
                            ctrl->get_CoreWebView2(&g_webview);

                            // DevTools stays enabled during development (F12 opens it)
                            ComPtr<ICoreWebView2Settings> settings;
                            g_webview->get_Settings(&settings);
                            settings->put_AreDefaultContextMenusEnabled(FALSE);
                            settings->put_AreDevToolsEnabled(TRUE);

                            g_webview->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [](ICoreWebView2* wv,
                                       ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        return HandleJsMessage(wv, args);
                                    }
                                ).Get(), nullptr
                            );

                            ResizeWebView();
                            g_webview->Navigate(url.c_str());

                            // 50 ms tick → 20 fps meter updates to JS
                            SetTimer(g_hwnd, 1, 50, nullptr);
                            return S_OK;
                        }
                    ).Get()
                );
            }
        ).Get()
    );
}

// ─────────────────────────────────────────────────────────────────
// Window Procedure
// ─────────────────────────────────────────────────────────────────

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        // Set g_hwnd BEFORE InitWebView — it uses it as the parent HWND
        g_hwnd = hwnd;
        InitWebView(GetExeDir() + L"\\ui\\index.html");
        return 0;

    case WM_SIZE:
        ResizeWebView();
        return 0;

    case WM_TIMER:
        // Push current dBFS reading to JS for the live meter
        if (g_webview && g_audioActive) {
            wchar_t buf[64];
            swprintf_s(buf, L"{\"type\":\"meter\",\"dbfs\":%.1f}",
                       g_currentDbfs.load(std::memory_order_relaxed));
            g_webview->PostWebMessageAsJson(buf);
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        if (g_audioActive) {
            ma_device_uninit(&g_captureDevice);
            g_audioActive = false;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ─────────────────────────────────────────────────────────────────
// Entry Point
// ─────────────────────────────────────────────────────────────────

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow)
{
    // STA required by WebView2 (and COM in general for UI threads)
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"VoxGuardWindow";
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon         = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(
        0,
        L"VoxGuardWindow",
        L"VoxGuard — Voice Coach",   // em-dash
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        960, 680,
        nullptr, nullptr, hInst, nullptr
    );

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    CoUninitialize();
    return static_cast<int>(message.wParam);
}
