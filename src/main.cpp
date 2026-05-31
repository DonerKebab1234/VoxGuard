/*
 * VoxGuard — Voice-volume coaching tool
 * main.cpp — Phase 0+1+2+3+4: Win32/WebView2, bridge, capture, sidetone, calibration, ducking
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
#include <mmdeviceapi.h>   // IMMDeviceEnumerator, eRender, eConsole
#include <audiopolicy.h>   // IAudioSessionManager2, ISimpleAudioVolume
#include <shellapi.h>      // Shell_NotifyIcon — system tray
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

// ── Duck state (Phase 4) ──────────────────────────────────────────
// Proactive duck: when speaking detected (avgDbfs >= normalDbfs), smoothly
// reduce the game's volume so the user hears themselves without shouting.
static std::atomic<bool>  g_duckEnabled{false};
static std::atomic<float> g_duckAmount{25.0f};           // 0–80 %

// Escalation duck: fires after sustained-loud + chime; ducks harder.
static std::atomic<bool>  g_escalDuckEnabled{true};
static std::atomic<float> g_escalDuckAmount{50.0f};      // 0–80 %
static bool               g_escalationActive    = false;
static int                g_escalRecoveryLeft   = 0;
static constexpr int      ESCAL_RECOVERY_TICKS  = 60;    // 3 s at 50 ms/tick

// Smooth duck approach: attack fast, release slow.
// Per-tick coefficients for exponential approach:
//   attack  150 ms → coeff ≈ 1 - e^(-50/150) ≈ 0.28
//   release 800 ms → coeff ≈ 1 - e^(-50/800) ≈ 0.06
static constexpr float DUCK_ATTACK  = 0.28f;
static constexpr float DUCK_RELEASE = 0.06f;
static float g_duckCurrentVol = 1.0f;   // currently applied volume factor (UI thread only)

// Target process to duck (0 = duck all non-Discord sessions).
// Updated by "setDuckTarget {pid}" bridge message.
static DWORD g_duckTargetPid         = 0;
static int   g_sessionRefreshTick    = 0;  // counter: refresh every 40 ticks = 2 s
static std::vector<ComPtr<ISimpleAudioVolume>> g_duckSessions;

// ── Chime state ───────────────────────────────────────────────────
// A short sine-wave burst mixed into the playback callback.
// The UI thread sets g_chimeFramesLeft; the playback callback decrements it.
static std::atomic<bool>  g_chimeEnabled{true};
static std::atomic<float> g_chimeVol{0.3f};   // 0–1

static std::vector<float> g_chimeBuf;          // pre-generated PCM (mono)
static int                g_chimeTotal = 0;    // frames in chime
static std::atomic<int>   g_chimeLeft{0};

// ── System tray (Phase 5) ─────────────────────────────────────────
#define WM_TRAYICON  (WM_USER + 1)
#define ID_TRAY_SHOW  1001
#define ID_TRAY_QUIT  1002

static NOTIFYICONDATAW g_nid{};   // explicit W variant; NOTIFYICONDATA is ANSI under WIN32_LEAN_AND_MEAN
static HMENU          g_trayMenu    = nullptr;
static bool           g_hiddenToTray   = false;
static bool           g_startMinimized = false;

// ── DAF — Delayed Auditory Feedback (Phase 6) ─────────────────────
// Delays the sidetone by ~180 ms, disrupting speech fluency and
// causing the user to speak more quietly (the delayed-feedback effect).
// OFF by default — it's an optional deterrent, not comfortable.
//
// Implementation: a fixed-size circular delay line in the capture
// callback, shared with the sidetone write path.
// Read is always DAF_DELAY_FRAMES behind the write head.
// Both pointers are on the capture thread side; only g_dafWrite is
// shared (atomically) with the playback path.
static std::atomic<bool> g_dafEnabled{false};
static constexpr int DAF_SR           = 48000;
static constexpr int DAF_DELAY_FRAMES = DAF_SR * 180 / 1000;   // 8640 frames
static constexpr int DAF_MAX_FRAMES   = DAF_SR * 220 / 1000;   // 10560 frames
static float         g_dafLine[DAF_MAX_FRAMES]{};               // ~41 KB static
static std::atomic<int> g_dafWriteHead{0};

// ── Adaptive sidetone (Phase 6) ────────────────────────────────────
// When enabled, sidetone gain rises with mic loudness so the user
// always hears themselves clearly above the masker without needing to
// consciously increase volume.
static std::atomic<bool>  g_adaptiveSidetone{false};
static std::atomic<float> g_adaptiveIntensity{0.5f};  // 0–1

// ── Overlay meter (Phase 6) ────────────────────────────────────────
static HWND g_overlayHwnd = nullptr;
static bool g_overlayVisible = false;

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

    // ── DAF delay line — fill unconditionally ────────────────────────
    // Always keep the line populated so DAF can be toggled on-the-fly
    // without a burst of silence at the start.
    {
        int wHead = g_dafWriteHead.load(std::memory_order_relaxed);
        for (ma_uint32 i = 0; i < frameCount; ++i) {
            g_dafLine[wHead] = s[i];
            if (++wHead >= DAF_MAX_FRAMES) wHead = 0;
        }
        // release so the playback thread sees fully-written samples
        g_dafWriteHead.store(wHead, std::memory_order_release);
    }

    // ── Sidetone / DAF feed into playback ring buffer ─────────────
    if (g_sidetoneRbReady && g_sidetoneEnabled.load(std::memory_order_relaxed)) {

        // Base gain from slider (0–1)
        float gain = g_sidetoneGain.load(std::memory_order_relaxed);

        // Adaptive gain: boost sidetone proportionally to current loudness.
        // This ensures the user always hears themselves over the game/masker.
        if (g_adaptiveSidetone.load(std::memory_order_relaxed)) {
            float intensity = g_adaptiveIntensity.load(std::memory_order_relaxed);
            float loudNorm  = (dbfs + 60.0f) / 60.0f;  // 0 at silence → 1 at 0 dBFS
            gain = std::min(1.0f, gain + intensity * loudNorm * 0.5f);
        }

        bool daf = g_dafEnabled.load(std::memory_order_relaxed);
        // For DAF, determine read position (180 ms behind current write head)
        int dafReadHead = 0;
        if (daf) {
            int wHead = g_dafWriteHead.load(std::memory_order_relaxed);
            dafReadHead = (wHead - DAF_DELAY_FRAMES + DAF_MAX_FRAMES * 2)
                          % DAF_MAX_FRAMES;
        }

        ma_uint32 toWrite = frameCount;
        void*     writePtr = nullptr;
        if (ma_pcm_rb_acquire_write(&g_sidetoneRb, &toWrite, &writePtr) == MA_SUCCESS
            && toWrite > 0)
        {
            float* dst = static_cast<float*>(writePtr);
            for (ma_uint32 i = 0; i < toWrite; ++i) {
                float sample = daf
                    ? g_dafLine[(dafReadHead + static_cast<int>(i)) % DAF_MAX_FRAMES]
                    : s[i];
                float out = sample * gain;
                dst[i * 2]     = out;   // left
                dst[i * 2 + 1] = out;   // right
            }
            ma_pcm_rb_commit_write(&g_sidetoneRb, toWrite);
        }
        // Full ring buffer = playback stalled; drop this batch silently.
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

    // ── Mix chime burst into output ───────────────────────────────
    // The UI thread fires g_chimeLeft = g_chimeTotal to trigger a chime.
    // We read/decrement atomically; no other thread writes g_chimeLeft while
    // the playback device is running.
    int left = g_chimeLeft.load(std::memory_order_relaxed);
    if (left > 0 && !g_chimeBuf.empty()) {
        const int   offset = g_chimeTotal - left;
        const float vol    = g_chimeVol.load(std::memory_order_relaxed);
        const int   copy   = std::min(left, static_cast<int>(frameCount));

        for (int i = 0; i < copy; ++i) {
            float s = g_chimeBuf[static_cast<size_t>(offset + i)] * vol;
            out[i * 2]     = std::max(-1.0f, std::min(1.0f, out[i * 2]     + s));
            out[i * 2 + 1] = std::max(-1.0f, std::min(1.0f, out[i * 2 + 1] + s));
        }
        g_chimeLeft.store(std::max(0, left - static_cast<int>(frameCount)),
                          std::memory_order_relaxed);
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
// Phase 6 — Overlay meter (always-on-top translucent GDI window)
// ─────────────────────────────────────────────────────────────────

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rc{};
        GetClientRect(hwnd, &rc);

        // Black background
        HBRUSH bgBrush = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(hdc, &rc, bgBrush);
        DeleteObject(bgBrush);

        // Level bar
        float dbfs    = g_currentDbfs.load(std::memory_order_relaxed);
        float level   = std::max(0.0f, std::min(1.0f, (dbfs + 60.0f) / 60.0f));
        int   barH    = static_cast<int>(rc.bottom * level);
        COLORREF col  = (dbfs > -6.0f)  ? RGB(255, 60, 60)  :
                        (dbfs > -12.0f) ? RGB(232, 178, 0) :
                                           RGB(32, 212, 114);
        RECT bar = { 6, rc.bottom - barH, rc.right - 6, rc.bottom };
        HBRUSH barBrush = CreateSolidBrush(col);
        FillRect(hdc, &bar, barBrush);
        DeleteObject(barBrush);

        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_DESTROY) { g_overlayHwnd = nullptr; g_overlayVisible = false; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void OverlayCreate(HINSTANCE hInst)
{
    if (g_overlayHwnd) return;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = OverlayWndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"VoxGuardOverlay";
    RegisterClassExW(&wc);

    // Get primary monitor work area for placement
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);

    // Small strip in top-right corner: 48 × 220 px
    int W = 48, H = 220;
    int x = wa.right  - W - 12;
    int y = wa.top    + 40;

    g_overlayHwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
        L"VoxGuardOverlay", nullptr,
        WS_POPUP,
        x, y, W, H,
        nullptr, nullptr, hInst, nullptr
    );

    // 75% opaque black background
    SetLayeredWindowAttributes(g_overlayHwnd, 0, 190, LWA_ALPHA);
    ShowWindow(g_overlayHwnd, SW_SHOWNOACTIVATE);
    g_overlayVisible = true;
}

static void OverlayDestroy()
{
    if (g_overlayHwnd) { DestroyWindow(g_overlayHwnd); g_overlayHwnd = nullptr; }
    g_overlayVisible = false;
}

// ─────────────────────────────────────────────────────────────────
// Phase 5 — System tray
// ─────────────────────────────────────────────────────────────────

static void TrayUpdateTip(const wchar_t* tip)
{
    wcscpy_s(g_nid.szTip, _countof(g_nid.szTip), tip);
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void TrayCreate(HINSTANCE hInst)
{
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize           = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd             = g_hwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon            = LoadIcon(hInst, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, _countof(g_nid.szTip), L"VoxGuard");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    g_trayMenu = CreatePopupMenu();
    AppendMenuW(g_trayMenu, MF_STRING,    ID_TRAY_SHOW, L"Show VoxGuard");
    AppendMenuW(g_trayMenu, MF_SEPARATOR, 0,            nullptr);
    AppendMenuW(g_trayMenu, MF_STRING,    ID_TRAY_QUIT, L"Quit");
}

static void TrayDestroy()
{
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_trayMenu) { DestroyMenu(g_trayMenu); g_trayMenu = nullptr; }
}

static void TrayShowMenu()
{
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);     // required so menu dismisses on outside click
    TrackPopupMenu(g_trayMenu,
                   TPM_BOTTOMALIGN | TPM_LEFTALIGN | TPM_RIGHTBUTTON,
                   pt.x, pt.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0); // flush after SetForegroundWindow trick
}

static void TrayRestoreWindow()
{
    ShowWindow(g_hwnd, SW_RESTORE);
    SetForegroundWindow(g_hwnd);
    g_hiddenToTray = false;
}

// ─────────────────────────────────────────────────────────────────
// Phase 5 — Start-with-Windows (HKCU Run key)
// ─────────────────────────────────────────────────────────────────

static const wchar_t* RUN_KEY =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* RUN_VALUE = L"VoxGuard";

static bool GetStartWithWindows()
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &key)
        != ERROR_SUCCESS)
        return false;
    bool exists = (RegQueryValueExW(key, RUN_VALUE, nullptr, nullptr,
                                    nullptr, nullptr) == ERROR_SUCCESS);
    RegCloseKey(key);
    return exists;
}

static void SetStartWithWindows(bool enable)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_WRITE, &key)
        != ERROR_SUCCESS)
        return;
    if (enable) {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        // /minimized → starts hidden to tray, audio runs silently
        std::wstring val = std::wstring(L"\"") + path + L"\" /minimized";
        RegSetValueExW(key, RUN_VALUE, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(val.c_str()),
                       static_cast<DWORD>((val.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, RUN_VALUE);
    }
    RegCloseKey(key);
}

// Forward declaration — defined in the Helpers section below
static std::wstring JsonEscape(const std::wstring&);

// ─────────────────────────────────────────────────────────────────
// Phase 4 — Chime generation
// ─────────────────────────────────────────────────────────────────

// Pre-generate a 200 ms 880 Hz (A5) sine burst with cosine fade-in/out.
// Called once at startup, before any playback device is started.
static void GenerateChime()
{
    constexpr int   SR      = 48000;
    constexpr float FREQ    = 880.0f;
    constexpr float DUR_MS  = 200.0f;
    const     int   TOTAL   = static_cast<int>(SR * DUR_MS / 1000.0f); // 9600 frames
    const     int   FADE    = TOTAL / 8;                                 // 25 ms fade

    g_chimeBuf.resize(static_cast<size_t>(TOTAL));
    for (int i = 0; i < TOTAL; ++i) {
        float t     = static_cast<float>(i) / SR;
        float s     = std::sin(2.0f * 3.14159265f * FREQ * t);
        // Cosine fade envelope avoids clicks
        float env   = 1.0f;
        if (i < FADE)
            env = 0.5f * (1.0f - std::cos(3.14159265f * i / FADE));
        else if (i >= TOTAL - FADE)
            env = 0.5f * (1.0f - std::cos(3.14159265f * (TOTAL - i) / FADE));
        g_chimeBuf[static_cast<size_t>(i)] = s * env;
    }
    g_chimeTotal = TOTAL;
}

// Trigger the chime (UI thread only — sets the atomic that the playback callback reads)
static void TriggerChime()
{
    if (g_chimeEnabled.load() && g_sidetoneDevActive && g_chimeTotal > 0)
        g_chimeLeft.store(g_chimeTotal, std::memory_order_relaxed);
}

// ─────────────────────────────────────────────────────────────────
// Phase 4 — WASAPI per-session ducking (UI thread only — COM/STA)
// ─────────────────────────────────────────────────────────────────

// Returns true if the executable at `pid` contains any of the skip keywords.
// Used to never duck Discord, audiodg (Windows audio engine), or ourselves.
static bool ShouldSkipPid(DWORD pid)
{
    if (pid == GetCurrentProcessId()) return true;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    wchar_t path[MAX_PATH]{};
    DWORD   len = MAX_PATH;
    bool    skip = false;
    if (QueryFullProcessImageNameW(h, 0, path, &len)) {
        // Lowercase the filename part for comparison
        std::wstring name(path, len);
        for (auto& c : name) c = static_cast<wchar_t>(towlower(c));
        if (name.find(L"discord")  != std::wstring::npos) skip = true;
        if (name.find(L"audiodg")  != std::wstring::npos) skip = true;
        if (name.find(L"voxguard") != std::wstring::npos) skip = true;
    }
    CloseHandle(h);
    return skip;
}

// Re-enumerate render sessions and cache ISimpleAudioVolume for every target session.
// Called on the UI thread; cheap enough to run every 2 s.
static void RefreshDuckSessions()
{
    g_duckSessions.clear();

    ComPtr<IMMDeviceEnumerator> devEnum;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), &devEnum)))
        return;

    ComPtr<IMMDevice> device;
    if (FAILED(devEnum->GetDefaultAudioEndpoint(eRender, eConsole, &device)))
        return;

    ComPtr<IAudioSessionManager2> mgr;
    if (FAILED(device->Activate(__uuidof(IAudioSessionManager2),
                                CLSCTX_ALL, nullptr, &mgr)))
        return;

    ComPtr<IAudioSessionEnumerator> sessEnum;
    if (FAILED(mgr->GetSessionEnumerator(&sessEnum))) return;

    int count = 0;
    sessEnum->GetCount(&count);

    for (int i = 0; i < count; ++i) {
        ComPtr<IAudioSessionControl> ctrl;
        if (FAILED(sessEnum->GetSession(i, &ctrl))) continue;

        ComPtr<IAudioSessionControl2> ctrl2;
        if (FAILED(ctrl.As(&ctrl2))) continue;

        DWORD pid = 0;
        ctrl2->GetProcessId(&pid);
        if (pid == 0) continue;  // skip system/global session

        if (ShouldSkipPid(pid)) continue;

        // If the user picked a specific app, only duck that one
        if (g_duckTargetPid != 0 && pid != g_duckTargetPid) continue;

        ComPtr<ISimpleAudioVolume> vol;
        if (SUCCEEDED(ctrl.As(&vol)))
            g_duckSessions.push_back(std::move(vol));
    }
}

// Apply `factor` (0–1) to all cached sessions.
static void ApplyDuckVolume(float factor)
{
    for (auto& vol : g_duckSessions)
        if (vol) vol->SetMasterVolume(factor, nullptr);
}

// Restore all sessions to 1.0 and clear the cache.
static void RestoreDuck()
{
    ApplyDuckVolume(1.0f);
    g_duckSessions.clear();
    g_duckCurrentVol  = 1.0f;
    g_escalationActive = false;
}

// Build JSON array of render session process names for the UI dropdown.
static std::wstring GetAudioSessionsJson()
{
    ComPtr<IMMDeviceEnumerator> devEnum;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), &devEnum)))
        return L"[]";

    ComPtr<IMMDevice> device;
    if (FAILED(devEnum->GetDefaultAudioEndpoint(eRender, eConsole, &device)))
        return L"[]";

    ComPtr<IAudioSessionManager2> mgr;
    if (FAILED(device->Activate(__uuidof(IAudioSessionManager2),
                                CLSCTX_ALL, nullptr, &mgr)))
        return L"[]";

    ComPtr<IAudioSessionEnumerator> sessEnum;
    if (FAILED(mgr->GetSessionEnumerator(&sessEnum))) return L"[]";

    int count = 0;
    sessEnum->GetCount(&count);

    std::wostringstream json;
    json << L"[";
    bool first = true;

    for (int i = 0; i < count; ++i) {
        ComPtr<IAudioSessionControl> ctrl;
        if (FAILED(sessEnum->GetSession(i, &ctrl))) continue;

        ComPtr<IAudioSessionControl2> ctrl2;
        if (FAILED(ctrl.As(&ctrl2))) continue;

        DWORD pid = 0;
        ctrl2->GetProcessId(&pid);
        if (pid == 0 || ShouldSkipPid(pid)) continue;

        // Get executable base name
        std::wstring exeName = L"Unknown";
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (h) {
            wchar_t path[MAX_PATH]{};
            DWORD len = MAX_PATH;
            if (QueryFullProcessImageNameW(h, 0, path, &len)) {
                std::wstring full(path, len);
                auto slash = full.rfind(L'\\');
                exeName = (slash != std::wstring::npos) ? full.substr(slash + 1) : full;
            }
            CloseHandle(h);
        }

        if (!first) json << L",";
        first = false;
        json << L"{\"pid\":" << pid
             << L",\"name\":\"" << JsonEscape(exeName) << L"\"}";
    }
    json << L"]";
    return json.str();
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
        // Pong + send initial state so the UI can sync toggles on load
        bool sww = GetStartWithWindows();
        wchar_t pong[128];
        swprintf_s(pong,
            L"{\"type\":\"pong\",\"ok\":true,\"startWithWindows\":%s}",
            sww ? L"true" : L"false");
        wv->PostWebMessageAsJson(pong);

    } else if (msg.find(L"\"type\":\"getDevices\"") != std::wstring::npos) {
        // JS asked for the list of capture devices
        std::wstring resp = L"{\"type\":\"deviceList\",\"devices\":";
        resp += GetCaptureDevicesJson();
        resp += L"}";
        wv->PostWebMessageAsJson(resp.c_str());

    } else if (msg.find(L"\"type\":\"getStartWithWindows\"") != std::wstring::npos) {
        bool on = GetStartWithWindows();
        wv->PostWebMessageAsJson(on
            ? L"{\"type\":\"startWithWindows\",\"enabled\":true}"
            : L"{\"type\":\"startWithWindows\",\"enabled\":false}");

    } else if (msg.find(L"\"type\":\"setStartWithWindows\"") != std::wstring::npos) {
        bool enable = msg.find(L"\"enabled\":true") != std::wstring::npos;
        SetStartWithWindows(enable);
        wv->PostWebMessageAsJson(L"{\"type\":\"startWithWindowsSet\",\"ok\":true}");

    } else if (msg.find(L"\"type\":\"minimizeToTray\"") != std::wstring::npos) {
        ShowWindow(g_hwnd, SW_HIDE);
        g_hiddenToTray = true;

    } else if (msg.find(L"\"type\":\"setMaster\"") != std::wstring::npos) {
        // Master enable/disable (future: pause all processing when off)
        // For now just acknowledge
        wv->PostWebMessageAsJson(L"{\"type\":\"masterSet\",\"ok\":true}");

    } else if (msg.find(L"\"type\":\"setDuck\"") != std::wstring::npos) {
        bool enable = msg.find(L"\"enabled\":true") != std::wstring::npos;
        g_duckEnabled.store(enable, std::memory_order_relaxed);
        if (enable) {
            g_sessionRefreshTick = 40; // force immediate refresh on next tick
        } else {
            RestoreDuck();
        }
        wv->PostWebMessageAsJson(L"{\"type\":\"duckSet\",\"ok\":true}");

    } else if (msg.find(L"\"type\":\"setDuckAmount\"") != std::wstring::npos) {
        auto pos = msg.find(L"\"value\":");
        if (pos != std::wstring::npos) {
            pos += 8;
            try {
                float v = std::stof(msg.substr(pos));
                g_duckAmount.store(std::max(0.0f, std::min(80.0f, v)),
                                   std::memory_order_relaxed);
            } catch (...) {}
        }

    } else if (msg.find(L"\"type\":\"setDuckTarget\"") != std::wstring::npos) {
        // {"type":"setDuckTarget","pid":1234}  or  "pid":0 for "all"
        auto pos = msg.find(L"\"pid\":");
        if (pos != std::wstring::npos) {
            pos += 6;
            try {
                g_duckTargetPid = static_cast<DWORD>(std::stoul(msg.substr(pos)));
                g_sessionRefreshTick = 40; // force re-enumerate
            } catch (...) {}
        }

    } else if (msg.find(L"\"type\":\"setEscalDuck\"") != std::wstring::npos) {
        bool en = msg.find(L"\"enabled\":true") != std::wstring::npos;
        g_escalDuckEnabled.store(en, std::memory_order_relaxed);

    } else if (msg.find(L"\"type\":\"setEscalDuckAmount\"") != std::wstring::npos) {
        auto pos = msg.find(L"\"value\":");
        if (pos != std::wstring::npos) {
            pos += 8;
            try {
                float v = std::stof(msg.substr(pos));
                g_escalDuckAmount.store(std::max(0.0f, std::min(80.0f, v)),
                                        std::memory_order_relaxed);
            } catch (...) {}
        }

    } else if (msg.find(L"\"type\":\"setDaf\"") != std::wstring::npos) {
        bool en = msg.find(L"\"enabled\":true") != std::wstring::npos;
        g_dafEnabled.store(en, std::memory_order_relaxed);
        wv->PostWebMessageAsJson(L"{\"type\":\"dafSet\",\"ok\":true}");

    } else if (msg.find(L"\"type\":\"setAdaptiveSidetone\"") != std::wstring::npos) {
        bool en = msg.find(L"\"enabled\":true") != std::wstring::npos;
        g_adaptiveSidetone.store(en, std::memory_order_relaxed);

    } else if (msg.find(L"\"type\":\"setAdaptiveIntensity\"") != std::wstring::npos) {
        auto pos = msg.find(L"\"value\":");
        if (pos != std::wstring::npos) {
            pos += 8;
            try {
                float v = std::stof(msg.substr(pos)) / 100.0f;
                g_adaptiveIntensity.store(std::max(0.0f, std::min(1.0f, v)),
                                          std::memory_order_relaxed);
            } catch (...) {}
        }

    } else if (msg.find(L"\"type\":\"setOverlay\"") != std::wstring::npos) {
        bool en = msg.find(L"\"enabled\":true") != std::wstring::npos;
        if (en && !g_overlayHwnd) {
            // Get HINSTANCE from g_hwnd
            HINSTANCE hInst = reinterpret_cast<HINSTANCE>(
                GetWindowLongPtrW(g_hwnd, GWLP_HINSTANCE));
            OverlayCreate(hInst);
        } else if (!en && g_overlayHwnd) {
            OverlayDestroy();
        }
        wv->PostWebMessageAsJson(en
            ? L"{\"type\":\"overlaySet\",\"visible\":true}"
            : L"{\"type\":\"overlaySet\",\"visible\":false}");

    } else if (msg.find(L"\"type\":\"setChime\"") != std::wstring::npos) {
        bool en = msg.find(L"\"enabled\":true") != std::wstring::npos;
        g_chimeEnabled.store(en, std::memory_order_relaxed);

    } else if (msg.find(L"\"type\":\"setChimeVolume\"") != std::wstring::npos) {
        auto pos = msg.find(L"\"value\":");
        if (pos != std::wstring::npos) {
            pos += 8;
            try {
                float v = std::stof(msg.substr(pos)) / 100.0f;
                g_chimeVol.store(std::max(0.0f, std::min(1.0f, v)),
                                 std::memory_order_relaxed);
            } catch (...) {}
        }

    } else if (msg.find(L"\"type\":\"getAudioSessions\"") != std::wstring::npos) {
        std::wstring resp = L"{\"type\":\"sessionList\",\"sessions\":";
        resp += GetAudioSessionsJson();
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
    case WM_CREATE: {
        // Set g_hwnd BEFORE InitWebView — it uses it as the parent HWND
        g_hwnd = hwnd;
        GenerateChime();
        // Tray icon is created from WinMain after we have the HINSTANCE
        InitWebView(GetExeDir() + L"\\ui\\index.html");
        return 0;
    }

    case WM_CLOSE:
        // Hide to tray instead of destroying — audio keeps running
        ShowWindow(hwnd, SW_HIDE);
        g_hiddenToTray = true;
        return 0;  // skip DefWindowProcW (which would call DestroyWindow)

    case WM_TRAYICON:
        switch (LOWORD(lp)) {
        case WM_RBUTTONUP:
            TrayShowMenu();
            break;
        case WM_LBUTTONDBLCLK:
            TrayRestoreWindow();
            break;
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_TRAY_SHOW:
            TrayRestoreWindow();
            break;
        case ID_TRAY_QUIT:
            TrayDestroy();
            DestroyWindow(hwnd);
            break;
        }
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

        // Repaint the always-on-top overlay (if visible)
        if (g_overlayHwnd) InvalidateRect(g_overlayHwnd, nullptr, FALSE);

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

        // ── 5. Auto-duck logic ────────────────────────────────────
        if (g_duckEnabled.load() && g_calibrated) {
            // Refresh the session list every 2 s (40 ticks) so new game
            // sessions are picked up without restarting VoxGuard.
            if (++g_sessionRefreshTick >= 40) {
                g_sessionRefreshTick = 0;
                RefreshDuckSessions();
                // Immediately apply the current volume to any newly found sessions
                ApplyDuckVolume(g_duckCurrentVol);
            }

            bool isSpeaking  = (avgDbfs >= g_normalDbfs);
            bool isEscalated = (g_sustainCount >= g_sustainThreshTicks);

            // Escalation state machine
            if (isEscalated && !g_escalationActive) {
                g_escalationActive  = true;
                g_escalRecoveryLeft = ESCAL_RECOVERY_TICKS;
                TriggerChime();
                g_webview->PostWebMessageAsJson(
                    L"{\"type\":\"escalation\",\"active\":true}");

            } else if (g_escalationActive) {
                if (!isEscalated) {
                    if (--g_escalRecoveryLeft <= 0) {
                        g_escalationActive = false;
                        g_webview->PostWebMessageAsJson(
                            L"{\"type\":\"escalation\",\"active\":false}");
                    }
                } else {
                    g_escalRecoveryLeft = ESCAL_RECOVERY_TICKS; // reset while still loud
                }
            }

            // Compute target volume
            float targetVol = 1.0f;
            if (isSpeaking) {
                float amt = g_duckAmount.load() / 100.0f;          // e.g. 0.25
                targetVol = 1.0f - amt;                             // e.g. 0.75
            }
            if (g_escalationActive && g_escalDuckEnabled.load()) {
                float amt = g_escalDuckAmount.load() / 100.0f;     // e.g. 0.50
                targetVol = std::min(targetVol, 1.0f - amt);        // e.g. 0.50
            }

            // Exponential smooth approach: attack fast, release slow
            float coeff = (targetVol < g_duckCurrentVol) ? DUCK_ATTACK : DUCK_RELEASE;
            g_duckCurrentVol += (targetVol - g_duckCurrentVol) * coeff;

            ApplyDuckVolume(g_duckCurrentVol);

        } else if (!g_duckEnabled.load() && g_duckCurrentVol < 0.99f) {
            // Duck was just disabled — restore immediately
            RestoreDuck();
        }

        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        OverlayDestroy();
        TrayDestroy();
        RestoreDuck();
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
    // Parse /minimized command-line flag (set by the start-with-Windows registry entry)
    {
        int     argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; i < argc; ++i)
            if (_wcsicmp(argv[i], L"/minimized") == 0) g_startMinimized = true;
        LocalFree(argv);
    }

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
        L"VoxGuard — Voice Coach",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        960, 680,
        nullptr, nullptr, hInst, nullptr
    );

    // Tray icon must be created after the HWND exists (WM_CREATE already fired)
    TrayCreate(hInst);

    if (g_startMinimized) {
        // Start silently in tray — audio is not running yet, will auto-start
        // only if the user opens the window. (Future: auto-start audio option.)
        g_hiddenToTray = true;
    } else {
        ShowWindow(g_hwnd, nCmdShow);
        UpdateWindow(g_hwnd);
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    CoUninitialize();
    return static_cast<int>(message.wParam);
}
