# VoxGuard — CLAUDE.md

## What this is
Real-time voice-volume coaching tool for a gaming headset.
**The problem:** Masking / Lombard effect — game audio at max + screaming Discord friends causes unconscious shouting.
**The fix:** Let the user hear their own voice OVER the masker (sidetone), and gently duck the game when speaking so their voice cuts through naturally. Escalation mode fires only on sustained loud stretches.

**Target hardware:** Brother's gaming PC with headset.
Developed/tested on my own mic first, then shipped as a standalone .exe.

---

## How to build

### Prerequisites
- Visual Studio 2022 (or Build Tools) with C++ workload
- CMake 3.20+ in PATH
- Internet connection on first build (FetchContent downloads WebView2 SDK + miniaudio.h)
- WebView2 runtime (ships with modern Windows 10/11 via Edge)

### Build steps
```powershell
# From C:\Users\epost\projects\voxguard\
$cmake = "C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake -S . -B build -G "Visual Studio 18 2026" -A x64
& $cmake --build build --config Release
# Executable: build\Release\VoxGuard.exe
# UI folder:  build\Release\ui\   (copied automatically as post-build step)
```

Generator is "Visual Studio 18 2026" because the machine has VS 2025 Insiders (VS18).
cmake.exe is bundled with VS at the path above — not in PATH by default.

### Run
```powershell
.\build\Release\VoxGuard.exe
```

### Dev notes
- Open Chrome DevTools inside the running app: press F12.
- WebView2 navigates to `file:///...exe-dir.../ui/index.html`.
- Bridge test: click Start in the app to see C++↔JS messages working.
- The UI also runs in a browser (or Claude Preview) for design iteration — fake meter simulates audio.

---

## Architecture

```
[UI thread]   Win32 message loop + WebView2 callbacks + COM + WM_TIMER(50ms) → PostWebMessageAsJson
[Audio thread] miniaudio real-time capture callback → writes g_currentDbfs (std::atomic<float>)
[Bridge]       window.chrome.webview.postMessage / addEventListener('message')
```

- **WASAPI Shared mode** (miniaudio default on Windows) — coexists with Discord and the game on the same mic + output device.
- **Buffer:** ~10 ms (WASAPI shared default). End-to-end latency target < 20 ms.
- **No COM in audio thread.** Only `std::atomic<float>` crosses thread boundary.
- **WM_TIMER at 50 ms** reads the atomic dBFS and pushes `{type:"meter",dbfs:N}` to JS. 20 fps is enough for a smooth meter.

---

## Message protocol (C++ ↔ JS bridge)

| Direction | Type | Payload |
|-----------|------|---------|
| JS → C++ | `ping` | — |
| JS → C++ | `getDevices` | — |
| JS → C++ | `startCapture` | `deviceIndex: N` (-1 = default) |
| JS → C++ | `stopCapture` | — |
| JS → C++ | `setSidetoneLevel` | `value: 0–100` |
| JS → C++ | `setSidetone` | `enabled: bool` |
| JS → C++ | `setDuck` | `enabled: bool` |
| JS → C++ | `setDuckAmount` | `value: 0–80` |
| JS → C++ | `setMaster` | `enabled: bool` |
| JS → C++ | `calibrate` | — |
| C++ → JS | `pong` | `ok: true` |
| C++ → JS | `deviceList` | `devices: [{id,name,isDefault}]` |
| C++ → JS | `captureStarted` | `ok: bool` |
| C++ → JS | `meter` | `dbfs: float` |

---

## Full feature spec (roadmap)

### Controls / toggles
- Master enable
- Sidetone (toggle + level slider)
- Adaptive sidetone (toggle + intensity) — level rises with loudness
- Auto-duck-while-speaking (toggle + amount + attack ms + release ms)
- Escalation duck (toggle + amount)
- Chime on escalation (toggle + volume)
- Start-with-Windows (toggle → HKCU Run key)
- DAF — Delayed Auditory Feedback (toggle, default OFF, ~180 ms delay)

### Sliders / inputs
- Sidetone level (0–100%)
- Adaptive intensity
- Speaking-detection sensitivity
- Auto-duck amount (0–80%)
- Auto-duck attack ms / release ms
- Normal-voice threshold (dBFS, derived from calibration)
- Too-loud threshold (dBFS, derived from calibration)
- Averaging window (ms)
- Seconds-over-threshold before escalation
- Escalation duck amount
- Chime volume
- Recovery time after escalation
- Target app to duck (dropdown of active WASAPI audio sessions — never duck Discord)
- Input device
- Output device
- DAF delay (ms)

### Features
- **Sidetone:** mic → headphones in real time, adjustable level
- **Adaptive sidetone:** sidetone level tracks mic loudness (Lombard fix)
- **Auto-duck (proactive):** when speaking detected, gently duck selected game app. Smooth attack/release. Keeps his voice audible without needing him to shout.
- **Escalation duck (reactive):** only on sustained-loud past threshold + N continuous seconds. Ducks game harder + plays chime. Restores after recovery time.
- **Live meter:** segmented dBFS bar, green/yellow/red zones, peak hold
- **Calibration:** 2-step guided flow — "normal voice" 5s, "max acceptable" 5s → derives thresholds
- **Session stats / quiet score:** count threshold breaches per session, show trend
- **System tray:** minimize-to-tray, right-click → Show / Enable / Quit
- **Start with Windows:** toggle writes/removes HKCU\Software\Microsoft\Windows\CurrentVersion\Run
- **Optional overlay:** always-on-top translucent click-through meter window
- **Optional DAF:** 180 ms delayed auditory feedback (deterrent, off by default)

---

## PROGRESS / STATUS

- [x] **Phase 0** — CMake scaffold + native Win32 window hosting WebView2 + bridge round-trip + clean HTML shell
  - Files: CMakeLists.txt, src/main.cpp, ui/index.html, ui/style.css, ui/app.js
  - Build: `cmake -S . -B build -G "Visual Studio 17 2022" -A x64 && cmake --build build --config Release`
  - Verify: window opens, F12 DevTools accessible, Ping button returns pong in JS console
- [x] **Phase 1** — miniaudio mic capture + dBFS computation + live segmented meter pushed over bridge + device picker
  - Already wired into main.cpp (WM_TIMER, AudioCaptureCallback, GetCaptureDevicesJson, StartAudioCapture)
  - Verify: click Start, see the meter animate to your mic input, pick a specific device from dropdown
- [x] **Phase 2** — Sidetone: route mic → headphones; level slider wired to C++
  - `ma_pcm_rb` ring buffer (100 ms / 4800 stereo frames) connects capture → playback threads
  - `g_sidetoneEnabled` / `g_sidetoneGain` atomics; `StartSidetone()` / `StopSidetone()`
  - Bridge: `setSidetone {enabled}` and `setSidetoneLevel {value: 0–100}` both wired
  - Verify: Start capture, toggle Sidetone ON in footer — you should hear yourself in headphones
- [x] **Phase 3** — Calibration flow (2-step), derive normal/too-loud thresholds, sustained-loudness window
  - C++: `CalibState` enum, 100-tick countdown (5 s), mean dBFS from non-silent samples → `g_normalDbfs` / `g_tooLoudDbfs`
  - C++: 200-entry circular buffer, rolling average over configurable window, `g_sustainCount` ramp logic
  - C++: sends `calibProgress`, `calibDone`, `loudnessState` bridge messages from WM_TIMER
  - UI: calibration panel overlaying main area — step indicator, SVG ring progress, result + summary cards
  - UI: zone legend updates to calibrated thresholds; loudness badge (normal/elevated/tooLoud) shown after calibration
  - Verify: click Calibrate, run both steps, confirm zone legend changes; then Start capture to see loudness badge
- [x] **Phase 4** — Auto-duck-while-speaking + escalation duck + chime
  - C++: `RefreshDuckSessions()` enumerates WASAPI render sessions every 2 s; never ducks Discord/audiodg/self
  - C++: `ApplyDuckVolume(factor)` via `ISimpleAudioVolume::SetMasterVolume` on all target sessions
  - C++: Proactive duck (speaking detected → smooth attack/release with 0.28/0.06 coefficients)
  - C++: Escalation state machine (sustained-loud → harder duck + chime + `escalation` bridge msg)
  - C++: Chime = 200 ms 880 Hz sine burst with cosine fade, mixed into playback callback
  - UI: Duck-target dropdown (populated from `sessionList` after capture starts); "All apps" default
  - UI: Status label flashes red "Too Loud!" on escalation event
- [x] **Phase 5** — System tray, start-with-Windows, session stats/quiet-score, packaging to .exe
  - C++: `Shell_NotifyIcon` tray icon always present; WM_CLOSE hides to tray instead of quitting
  - C++: Right-click tray menu → Show / Quit; double-click → restore window
  - C++: HKCU Run key written/removed by `SetStartWithWindows()`; `/minimized` flag starts hidden
  - C++: Pong response includes `startWithWindows` state so UI toggle initialises correctly
  - UI: Startup toggle in footer wired to `setStartWithWindows`; fix breach counting to use escalation events
  - Packaging: zip `build\Release\` (VoxGuard.exe + ui\) — WebView2 runtime ships with Windows 11/Edge
- [ ] **Phase 6** — DAF toggle, optional overlay meter, adaptive sidetone

### Next up (start of next session)
Read this file, then:
1. Run `.\build\Release\VoxGuard.exe` — Start capture, calibrate, verify loudness badge + duck works
2. Ship to brother: zip `build\Release\` (VoxGuard.exe + ui\) — he just extracts and runs
3. Begin Phase 6 if desired: DAF toggle, adaptive sidetone, always-on-top overlay meter

---

## Design notes
- UI palette: single-hue violet (`--accent: #6e58f8`), void black background, no glassmorphism
- Font: Inter (UI) + JetBrains Mono (numbers/code)
- The meter is the hero — 300px tall segmented bar, 40 segments, peak hold
- Settings live in a compact 72px footer strip
- Colors: green = below -12 dBFS, yellow = -12 to -6, red = above -6
