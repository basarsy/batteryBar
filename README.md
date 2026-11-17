# BatteryBar

Tiny Windows 11 utility that shows the battery level of wireless peripherals
(mouse, headset, keyboard, controller) right in the taskbar, next to the tray.

![BatteryBar in the Windows 11 taskbar](docs/screenshot.png)

- Native C++ / Win32, single ~290 KB static exe, no runtime needed.
- ~1.7 MB private memory, no CPU at idle (one cheap poll per minute).
- Starts with Windows (per-user `Run` key, toggle in the right-click menu).

## Download

Get `BatteryBar.exe` from the [latest release](https://github.com/basarsy/batteryBar/releases/latest),
put it anywhere (e.g. `%LOCALAPPDATA%\Programs\BatteryBar`) and double-click it. That's it: no installer,
no runtime. It registers itself to start with Windows (toggle in the right-click menu).

The exe is unsigned, so SmartScreen may warn on first launch: click **More info › Run anyway**.

## Supported devices

| Source | Devices |
| --- | --- |
| Logitech HID++ 2.0 | Lightspeed / Unifying / Bolt receivers and wired Logitech devices (G Pro X Superlight, G502 X, MX Master, ...) |
| Windows Bluetooth battery | Any Bluetooth headset/mouse/keyboard whose battery Windows shows in *Settings › Bluetooth & devices* |

Other vendors (Razer, SteelSeries, Corsair, generic 2.4 GHz dongles) use their own
protocols. You can add one as another `Query*` provider in `src/devices.h`.

## Usage

- **Right-click / left-click** the bar: lists devices with names, *Refresh now*,
  *Start with Windows*, *Exit*.
- Battery ≤ 15% turns red, a ⚡ marks charging.
- A device that stops answering (sleeping/off) stays visible but dimmed for 30 minutes, then disappears.
- `BatteryBar.exe --list` prints the detected devices to the console.

## Build from source

Requires MinGW-w64 (`winget install BrechtSanders.WinLibs.POSIX.UCRT`), with `g++` on `PATH`.

```bat
build.bat                                         :: -> build\BatteryBar.exe
powershell -ExecutionPolicy Bypass -File install.ps1    :: copy to %LOCALAPPDATA%\Programs\BatteryBar and start
powershell -ExecutionPolicy Bypass -File uninstall.ps1  :: remove everything
```

## How it works

- The bar is a layered child window inside `Shell_TrayWnd`, positioned left of
  `TrayNotifyWnd`. Because it lives in the taskbar, it follows auto-hide and full-screen apps automatically.
  If Explorer restarts, the bar is recreated on `TaskbarCreated`.
- All device I/O runs on a worker thread. The UI thread never blocks, because its window is
  attached to Explorer's input queue.
- Logitech: on startup, plug/unplug, resume from sleep or manual refresh, it scans receiver slots 1–6
  (and index 0xFF for wired devices). Regular polls only query slots it already knows (~15 ms).
