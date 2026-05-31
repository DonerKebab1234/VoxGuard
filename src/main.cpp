/*
 * VoxGuard — Voice-volume coaching tool
 * main.cpp — Phase 0+1+2: Win32/WebView2, bridge, mic capture, dBFS meter, sidetone
 *
 * Threading model:
 *   UI thread      — Win32 message loop, WebView2 callbacks, all COM calls
 *   Capture thread — miniaudio capture callback → writes g_currentDbfs + sidetone ring buf
 *   Playback thread— miniaudio playback callback → reads sidetone ring buf → headphones
 *
 * Cross-thread shared state:
 *   g_currentDbfs     std::atomic<float>    — metering only
 *   g_sidetoneEnabled std::atomic<bool>     — toggled from UI thread, read on audio threads
 *   g_sidetoneGain    std::atomic<float>    — same
 *   g_sidetoneRb      ma_pcm_rb             — lock-free ring buffer; single producer
 *                                             (capture), single consumer (playback)
 *
 * Audio runs in WASAPI SHARED mode so Discord and the game coexist on the same device.
 * Buffer size ~10 ms (miniaudio WASAPI shared default) → sidetone latency < 15 ms.
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
#include <algorithm>
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

// ── Capture state ────────────────────────────────────────────────
static std::atomic<float> g_currentDbfs{-60.0f};
static ma_device          g_captureDevice{};
static bool               g_audioActive = false;

// ── Sidetone state ───────────────────────────────────────────────
// Ring buffer: 100 ms of stereo float at 48 kHz = 4800 stereo frames.
// Single producer = capture callback, single consumer = playback callback.
static ma_pcm_rb          g_sidetoneRb{};
static bool               g_sidetoneRbReady    = false;
static ma_device          g_playbackDevice{};
static bool               g_sidetoneDevActive  = false;

static std::atomic<bool>  g_sidetoneEnabled{false};
static std::atomic<float> g_sidetoneGain{0.4f};  // 0–1; default 40%

// ── Calibration state ─────────────────────────────────────────────
enum class CalibState { Idle, Step1, Step2 };
static CalibState         g_calibState     = CalibState::Idle;
static int                g_calibTicksLeft = 0;       // 50 ms ticks remaining in step
static std::vector<float> g_calibSamples;              // dBFS readings collected so far

// Thresholds in dBFS.  Defaults are reasonable for a typical headset mic;
// calibration replaces these with values derived from the user's actual voice.
static float g_normalDbfs  = -30.0f;  // start of "speaking" zone
static float g_tooLoudDbfs = -15.0f;  // start of "too loud" zone
static bool  g_calibrated  = false;

// ── Sustained-loudness window ─────────────────────────────────────
// Circular buffer of dBFS readings, one per WM_TIMER tick (50 ms).
// 200 slots × 50 ms = 10 s of history.
static constexpr int LOUD_BUF = 200;
static float g_loudBuf[LOUD_BUF] = {};
static int   g_loudHead          = 0;

// Configurable (can be updated via bridge messages in later phases):
static int g_avgWindowTicks     = 10;  // 10 × 50 ms = 500 ms rolling average
static int g_sustainThreshTicks = 40;  // 40 × 50 ms = 2 s to trigger "too loud"
static int g_sustainCount       = 0;   // consecutive ticks above tooLoud threshold

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

    // ── Sidetone feed ─────────────────────────────────────────────
    // Write gain-scaled mono mic into the ring buffer as stereo.
    // The playback callback drains it independently on the playback thread.
    if (g_sidetoneRbReady && g_sidetoneEnabled.load(std::memory_order_relaxed)) {
        const float gain = g_sidetoneGain.load(std::memory_order_relaxed);
        ma_uint32   toWrite = frameCount;
        void*       writePtr = nullptr;
        if (ma_pcm_rb_acquire_write(&g_sidetoneRb, &toWrite, &writePtr) == MA_SUCCESS
            && toWrite > 0)
        {
            float*       dst = static_cast<float*>(writePtr);
            const float* src = static_cast<const float*>(pInput);
            for (ma_uint32 i = 0; i < toWrite; ++i) {
                float s       = src[i] * gain;
                dst[i * 2]     = s;  // left
                dst[i * 2 + 1] = s;  // right
            }
            ma_pcm_rb_commit_write(&g_sidetoneRb, toWrite);
        }
        // If the ring buffer is full (toWrite == 0), we just drop samples.
        // This can only happen if the playback device stalls — rare in normal use.
    }
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
// Sidetone playback (called from UI thread to start/stop)
// ─────────────────────────────────────────────────────────────────

// Playback callback: drain the sidetone ring buffer → headphones.
// Runs on miniaudio's dedicated playback thread.
static void AudioPlaybackCallback(ma_device* /*dev*/,
                                   void*       pOutput,
                                   const void* /*pInput*/,
                                   ma_uint32   frameCount)
{
    float* out = static_cast<float*>(pOutput);

    if (!g_sidetoneRbReady) {
        std::memset(out, 0, frameCount * 2 * sizeof(float));
        return;
    }

    ma_uint32 toRead   = frameCount;
    void*     readPtr  = nullptr;
    if (ma_pcm_rb_acquire_read(&g_sidetoneRb, &toRead, &readPtr) == MA_SUCCESS
        && toRead > 0)
    {
        std::memcpy(out, readPtr, toRead * 2 * sizeof(float));
        ma_pcm_rb_commit_read(&g_sidetoneRb, toRead);
        // If ring buffer had fewer frames than requested, zero the remainder
        if (toRead < frameCount)
            std::memset(out + toRead * 2, 0, (frameCount - toRead) * 2 * sizeof(float));
    } else {
        // Underrun: output silence (happens briefly on first start or if capture stalls)
        std::memset(out, 0, frameCount * 2 * sizeof(float));
    }
}

// Start the sidetone playback device.
// The ring buffer is initialized once and stays alive for the session.
static bool StartSidetone()
{
    if (g_sidetoneDevActive) return true;

    // Init the ring buffer on first call
    if (!g_sidetoneRbReady) {
        // 4800 stereo frames = 100 ms at 48 kHz — enough headroom for any callback size
        if (ma_pcm_rb_init(ma_format_f32, 2, 4800,
                           nullptr, nullptr, &g_sidetoneRb) != MA_SUCCESS)
            return false;
        g_sidetoneRbReady = true;
    }

    ma_device_config cfg   = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format    = ma_format_f32;
    cfg.playback.channels  = 2;      // stereo headphones
    cfg.sampleRate         = 48000;
    cfg.dataCallback       = AudioPlaybackCallback;
    // Use system default playback device (headphones selected in Windows Sound settings)

    if (ma_device_init(nullptr, &cfg, &g_playbackDevice) != MA_SUCCESS)
        return false;

    if (ma_device_start(&g_playbackDevice) != MA_SUCCESS) {
        ma_device_uninit(&g_playbackDevice);
        return false;
    }

    g_sidetoneDevActive = true;
    return true;
}

static void StopSidetone()
{
    if (!g_sidetoneDevActive) return;
    ma_device_uninit(&g_playbackDevice);
    g_sidetoneDevActive = false;
    // Leave the ring buffer alive (cheap to keep, avoids re-init cost if re-enabled)
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

    } else if (msg.find(L"\"type\":\"setSidetone\"") != std::wstring::npos) {
        bool enable = msg.find(L"\"enabled\":true") != std::wstring::npos;
        g_sidetoneEnabled.store(enable, std::memory_order_relaxed);
        if (enable) {
            bool ok = StartSidetone();
            wv->PostWebMessageAsJson(ok ? L"{\"type\":\"sidetoneSet\",\"ok\":true}"
                                       : L"{\"type\":\"sidetoneSet\",\"ok\":false}");
        } else {
            // Keep device alive but silence it (gain=0 effectively via enabled flag)
            wv->PostWebMessageAsJson(L"{\"type\":\"sidetoneSet\",\"ok\":true}");
        }

    } else if (msg.find(L"\"type\":\"setSidetoneLevel\"") != std::wstring::npos) {
        // Parse "value": N  (0–100)
        auto pos = msg.find(L"\"value\":");
        if (pos != std::wstring::npos) {
            pos += 8;
            while (pos < msg.size() && msg[pos] == L' ') ++pos;
            try {
                float v = std::stof(msg.substr(pos)) / 100.0f;
                g_sidetoneGain.store(std::max(0.0f, std::min(1.0f, v)),
                                     std::memory_order_relaxed);
            } catch (...) {}
        }

    } else if (msg.find(L"\"type\":\"stopCapture\"") != std::wstring::npos) {
        if (g_audioActive) {
            g_calibState = CalibState::Idle;
            ma_device_uninit(&g_captureDevice);
            g_audioActive = false;
        }
        wv->PostWebMessageAsJson(L"{\"type\":\"captureStopped\",\"ok\":true}");

    } else if (msg.find(L"\"type\":\"calibrate\"") != std::wstring::npos) {
        // {"type":"calibrate","step":1}  or  step:2
        int step = 1;
        auto pos = msg.find(L"\"step\":");
        if (pos != std::wstring::npos) {
            pos += 7;
            while (pos < msg.size() && msg[pos] == L' ') ++pos;
            try { step = std::stoi(msg.substr(pos)); } catch (...) {}
        }
        g_calibSamples.clear();
        g_calibSamples.reserve(120);
        g_calibTicksLeft = 100; // 5 s × 20 ticks/s
        g_calibState     = (step == 2) ? CalibState::Step2 : CalibState::Step1;
        wv->PostWebMessageAsJson(L"{\"type\":\"calibStarted\",\"ok\":true}");

    } else if (msg.find(L"\"type\":\"setThresholds\"") != std::wstring::npos) {
        // Manual override: {"type":"setThresholds","normal":-25,"tooLoud":-12}
        auto p1 = msg.find(L"\"normal\":");
        if (p1 != std::wstring::npos) {
            p1 += 9;
            try { g_normalDbfs = std::stof(msg.substr(p1)); } catch (...) {}
        }
        auto p2 = msg.find(L"\"tooLoud\":");
        if (p2 != std::wstring::npos) {
            p2 += 10;
            try { g_tooLoudDbfs = std::stof(msg.substr(p2)); } catch (...) {}
        }
        g_calibrated = true;
        wv->PostWebMessageAsJson(L"{\"type\":\"thresholdsSet\",\"ok\":true}");

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

    case WM_TIMER: {
        if (!g_webview || !g_audioActive) return 0;

        float    dbfs = g_currentDbfs.load(std::memory_order_relaxed);
        wchar_t  buf[256];

        // ── 1. Instantaneous meter ───────────────────────────────
        swprintf_s(buf, L"{\"type\":\"meter\",\"dbfs\":%.1f}", dbfs);
        g_webview->PostWebMessageAsJson(buf);

        // ── 2. Rolling average (store linear power, average, convert back) ──
        // Averaging dBFS directly is an approximation; this is close enough
        // for a coaching tool operating in a narrow 60 dB range.
        g_loudBuf[g_loudHead] = dbfs;
        g_loudHead = (g_loudHead + 1) % LOUD_BUF;

        int   win   = std::max(1, std::min(g_avgWindowTicks, LOUD_BUF));
        float wsum  = 0.0f;
        int   wStart = (g_loudHead - win + LOUD_BUF) % LOUD_BUF;
        for (int i = 0; i < win; ++i)
            wsum += g_loudBuf[(wStart + i) % LOUD_BUF];
        float avgDbfs = wsum / win;

        // ── 3. Calibration countdown ─────────────────────────────
        if (g_calibState != CalibState::Idle && g_calibTicksLeft > 0) {
            // Skip near-silence readings (mic not yet speaking)
            if (dbfs > -55.0f) g_calibSamples.push_back(dbfs);
            --g_calibTicksLeft;

            constexpr int TOTAL_TICKS = 100; // 5 s / 50 ms
            float progress = 1.0f - static_cast<float>(g_calibTicksLeft) / TOTAL_TICKS;
            int   step     = (g_calibState == CalibState::Step1) ? 1 : 2;

            swprintf_s(buf,
                L"{\"type\":\"calibProgress\",\"step\":%d,"
                L"\"progress\":%.3f,\"dbfs\":%.1f,\"ticksLeft\":%d}",
                step, progress, dbfs, g_calibTicksLeft);
            g_webview->PostWebMessageAsJson(buf);

            if (g_calibTicksLeft == 0) {
                // Compute mean of collected samples
                float s = 0.0f;
                for (float v : g_calibSamples) s += v;
                float avg = g_calibSamples.empty() ? -30.0f
                                                   : s / static_cast<float>(g_calibSamples.size());

                if (g_calibState == CalibState::Step1) {
                    g_normalDbfs = avg;
                    swprintf_s(buf,
                        L"{\"type\":\"calibDone\",\"step\":1,\"dbfs\":%.1f}", avg);
                } else {
                    // Clamp: tooLoud must be at least 3 dB above normal
                    g_tooLoudDbfs = std::max(avg, g_normalDbfs + 3.0f);
                    g_calibrated  = true;
                    swprintf_s(buf,
                        L"{\"type\":\"calibDone\",\"step\":2,\"dbfs\":%.1f,"
                        L"\"normalDbfs\":%.1f,\"tooLoudDbfs\":%.1f}",
                        avg, g_normalDbfs, g_tooLoudDbfs);
                }
                g_webview->PostWebMessageAsJson(buf);
                g_calibState = CalibState::Idle;
                g_calibSamples.clear();
            }
        }

        // ── 4. Loudness state (only meaningful after calibration) ─
        if (g_calibrated) {
            // Ramp up faster than ramp-down: single shout shouldn't sustain,
            // but sustained shouting should escalate quickly.
            if (avgDbfs >= g_tooLoudDbfs)
                g_sustainCount = std::min(g_sustainCount + 1,
                                          g_sustainThreshTicks + 20);
            else
                g_sustainCount = std::max(0, g_sustainCount - 2);

            const wchar_t* state =
                (g_sustainCount >= g_sustainThreshTicks) ? L"tooLoud" :
                (avgDbfs >= g_normalDbfs)                ? L"elevated" :
                                                           L"normal";

            swprintf_s(buf,
                L"{\"type\":\"loudnessState\",\"state\":\"%s\","
                L"\"avg\":%.1f,\"sustained\":%d,\"sustainThresh\":%d}",
                state, avgDbfs, g_sustainCount, g_sustainThreshTicks);
            g_webview->PostWebMessageAsJson(buf);
        }

        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        StopSidetone();
        if (g_sidetoneRbReady) {
            ma_pcm_rb_uninit(&g_sidetoneRb);
            g_sidetoneRbReady = false;
        }
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
