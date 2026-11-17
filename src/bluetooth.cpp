// Battery levels Windows itself knows about (what Settings > Bluetooth shows).
// Windows stores them as a PnP property on the Bluetooth device node.
#include "devices.h"
#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <devpkey.h>
#include <set>

static const DEVPROPKEY kBattery = {{0x104EA319, 0x6EE2, 0x4701, {0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5}}, 2};
static const DEVPROPKEY kClassOfDevice = {{0x2BD67D8B, 0x8BEB, 0x48D5, {0x87, 0xE0, 0x6C, 0xDA, 0x34, 0x28, 0x04, 0x0A}}, 10};

static bool GetStr(HDEVINFO s, SP_DEVINFO_DATA* d, const DEVPROPKEY& k, std::wstring& out) {
    wchar_t buf[256]; DEVPROPTYPE t;
    if (!SetupDiGetDevicePropertyW(s, d, &k, &t, (BYTE*)buf, sizeof(buf), nullptr, 0) || t != DEVPROP_TYPE_STRING) return false;
    out = buf; return !out.empty();
}

static bool Contains(const std::wstring& hay, const wchar_t* needle) {
    std::wstring h = hay; CharLowerW(h.data());
    return h.find(needle) != std::wstring::npos;
}

static Kind Guess(const std::wstring& name, UINT32 cod, bool hasCod) {
    if (hasCod) {
        UINT32 major = (cod >> 8) & 0x1F, minor = (cod >> 2) & 0x3F;
        if (major == 4) return Kind::Headset;
        if (major == 5) {
            if (minor & 0x10) return Kind::Keyboard;
            if (minor & 0x20) return Kind::Mouse;
            if ((minor & 0x0F) == 1 || (minor & 0x0F) == 2) return Kind::Gamepad;
        }
    }
    for (auto w : {L"head", L"bud", L"ear", L"pods", L"audio", L"speaker", L"wh-", L"wf-"})
        if (Contains(name, w)) return Kind::Headset;
    for (auto w : {L"mouse", L"mx ", L"trackball"}) if (Contains(name, w)) return Kind::Mouse;
    for (auto w : {L"keyboard", L"keys"}) if (Contains(name, w)) return Kind::Keyboard;
    for (auto w : {L"controller", L"gamepad", L"xbox", L"dualsense"}) if (Contains(name, w)) return Kind::Gamepad;
    return Kind::Other;
}

void QueryBluetooth(std::vector<Device>& out) {
    HDEVINFO s = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (s == INVALID_HANDLE_VALUE) return;
    std::set<std::wstring> seen;
    SP_DEVINFO_DATA d{sizeof(d)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(s, i, &d); i++) {
        BYTE pct = 0; DEVPROPTYPE t;
        if (!SetupDiGetDevicePropertyW(s, &d, &kBattery, &t, &pct, 1, nullptr, 0) || t != DEVPROP_TYPE_BYTE || pct > 100) continue;
        Device dev;
        if (!GetStr(s, &d, DEVPKEY_Device_FriendlyName, dev.name) && !GetStr(s, &d, DEVPKEY_Device_DeviceDesc, dev.name))
            dev.name = L"Bluetooth device";
        if (!seen.insert(dev.name).second) continue;  // same device exposed by several nodes (HFP/A2DP/...)
        UINT32 cod = 0;
        bool hasCod = SetupDiGetDevicePropertyW(s, &d, &kClassOfDevice, &t, (BYTE*)&cod, 4, nullptr, 0) && t == DEVPROP_TYPE_UINT32;
        dev.kind = Guess(dev.name, cod, hasCod);
        dev.percent = pct;
        out.push_back(dev);
    }
    SetupDiDestroyDeviceInfoList(s);
}
