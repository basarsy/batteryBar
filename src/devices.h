#pragma once
#include <string>
#include <vector>

enum class Kind { Headset, Mouse, Keyboard, Gamepad, Other };

struct Device {
    std::wstring name;
    Kind kind = Kind::Other;
    int percent = -1;      // 0..100
    bool charging = false;
};

// Each provider appends what it finds. All of them block (HID I/O), so they
// must only be called from the worker thread, never from the UI thread.
// rescan: also re-probe receiver slots that were empty last time (slow: one timeout per slot).
void QueryLogitech(std::vector<Device>& out, bool rescan);  // Logitech HID++ 2.0 (Lightspeed / Unifying / Bolt / wired)
void QueryBluetooth(std::vector<Device>& out);  // Windows-reported Bluetooth battery (classic + LE)

inline std::vector<Device> QueryAllDevices(bool rescan) {
    std::vector<Device> d;
    QueryLogitech(d, rescan);
    QueryBluetooth(d);
    return d;
}
