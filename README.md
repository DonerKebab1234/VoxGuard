# VoxGuard

Real-time voice-volume coaching for gaming headsets. Fights the [Lombard effect](https://en.wikipedia.org/wiki/Lombard_effect) — the unconscious shouting that happens when game audio and loud Discord friends mask your own voice.

**How it works:** Let you hear yourself (sidetone) and gently duck the game when you speak, so your voice cuts through naturally without having to shout.

---

## Installation

> **Requirements:** Windows 10 or 11 with Microsoft Edge installed (Edge ships WebView2, which VoxGuard uses for its UI — it's already on every modern Windows machine).

1. Download `VoxGuard.exe` from the [latest release](https://github.com/DonerKebab1234/VoxGuard/releases/latest)
2. Put it anywhere you like (Desktop, a `C:\Tools\` folder, wherever)
3. Double-click `VoxGuard.exe` to run it — no installer needed

On the first launch you'll be asked if you want a desktop shortcut. That's it.

---

## First-time setup

1. **Pick your microphone** from the Input Device dropdown (or leave it on Default)
2. Click **Start** — the status dot turns green and the meter starts moving
3. Click **Calibrate Voice** and follow the two-step process:
   - Speak at your normal gaming volume for 5 seconds
   - Speak as loud as you'd ever want to sound for 5 seconds
4. Enable **Sidetone** — you'll hear your own voice in your headphones
5. Enable **Duck Game** if you want the game volume to drop automatically when you speak

That's the full setup. VoxGuard runs quietly in the system tray after that.

---

## Features

| Feature | What it does |
|---|---|
| **Sidetone** | Routes mic → headphones in real time. Hearing yourself naturally keeps you quieter. |
| **Adaptive Sidetone** (Auto) | Automatically raises sidetone level as you get louder, so you always hear yourself clearly. |
| **DAF** | Plays your voice back with a ~180 ms delay. Makes most people instinctively lower their volume. |
| **Duck Game** | Lowers the game's volume when you're speaking, so you don't need to shout over it. |
| **Calibration** | Two-step voice calibration sets your personal thresholds for normal vs. too-loud speech. |
| **Live meter** | 40-segment dBFS bar. Green = normal, yellow = elevated, red = too loud. White line = peak hold. |
| **Session stats** | Counts escalation events (sustained loud stretches), shows a quiet score, tracks session time. |
| **System tray** | Close the window and VoxGuard keeps running silently. Right-click the tray icon to show or quit. |
| **Start with Windows** | Adds VoxGuard to startup so it's always ready when you boot. |
| **Overlay meter** | Always-on-top translucent meter you can position anywhere on screen — visible even in fullscreen games. |

Press **?** in the top-right corner of the app for a full in-app feature guide.

---

## Uninstalling

VoxGuard doesn't use an installer, so there's nothing to uninstall in the traditional sense:

1. Close VoxGuard (right-click tray icon → Quit)
2. Delete `VoxGuard.exe`
3. If you enabled "Start with Windows", either toggle it off in the app first, **or** open Registry Editor (`Win + R` → `regedit`) and delete the `VoxGuard` entry under:
   ```
   HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run
   ```
4. Optionally delete the desktop shortcut and the small temp folder at `%TEMP%\VoxGuard\`

---

## Building from source

### Prerequisites

- [Visual Studio 2022](https://visualstudio.microsoft.com/) with the **Desktop development with C++** workload
- CMake 3.20+ (comes bundled with Visual Studio — no separate install needed)
- Internet connection on first build (downloads WebView2 SDK and miniaudio automatically)

### Steps

```powershell
# Clone the repo
git clone https://github.com/DonerKebab1234/VoxGuard.git
cd VoxGuard

# Configure (CMake is bundled with VS — adjust the path if your VS version differs)
$cmake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake -S . -B build -G "Visual Studio 17 2022" -A x64

# Build
& $cmake --build build --config Release

# Run
.\build\Release\VoxGuard.exe
```

> **Note:** If you have Visual Studio 2025 Insiders (VS 18), use `-G "Visual Studio 18 2026"` and the corresponding cmake.exe path.

The built exe is fully standalone — it contains the UI files as embedded resources and extracts them to `%TEMP%\VoxGuard\ui\` on the first run. You only need `VoxGuard.exe`.

---

## Troubleshooting

**Stuck on "Connecting…"**
VoxGuard retries automatically every 2 seconds. If it still shows "Connect failed" after ~20 seconds:
- Make sure no other instance of VoxGuard is already running (check the system tray)
- Try restarting the app

**No devices in the microphone dropdown**
- Your microphone might not be recognized as a capture device in Windows
- Check Settings → System → Sound → Input and make sure a device is listed there
- VoxGuard requires a microphone to function, but it will start without one — the dropdown will just be empty

**Game volume not ducking**
- Click Start first — ducking only works while capture is active
- The app being ducked shows up in the app selector after you click Start. Make sure the right app is selected (or leave it on "All apps")
- Discord is intentionally never ducked

**Start with Windows isn't working**
- Make sure you toggled it ON inside the app before closing
- You can verify the entry exists in `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` via Registry Editor
