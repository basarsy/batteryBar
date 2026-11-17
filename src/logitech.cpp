// Logitech HID++ 2.0 battery reader.
// Works with Lightspeed / Unifying / Bolt receivers (devices at index 1..6) and
// with directly connected devices (index 0xFF).
#include "devices.h"
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <map>
#ifdef HIDPP_DEBUG
#include <cstdio>
#endif

namespace {

constexpr BYTE SW_ID = 0x0B;   // our "software id" so we can recognise our own replies
constexpr DWORD TIMEOUT_MS = 400;
constexpr DWORD EMPTY_RESCAN_MS = 10 * 60 * 1000;  // how long a silent slot is skipped

struct Channel {
    HANDLE h = INVALID_HANDLE_VALUE;
    OVERLAPPED ov{};
    BYTE buf[64]{};
    DWORD len = 0;
    bool pending = false;

    bool Open(const std::wstring& path, DWORD reportLen) {
        h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        len = reportLen;
        return true;
    }
    bool Arm() {
        if (pending) return true;
        ResetEvent(ov.hEvent);
        if (!ReadFile(h, buf, len, nullptr, &ov) && GetLastError() != ERROR_IO_PENDING) return false;
        pending = true;
        return true;
    }
    ~Channel() {
        if (h != INVALID_HANDLE_VALUE) {
            if (pending) { CancelIo(h); DWORD n; GetOverlappedResult(h, &ov, &n, TRUE); }
            CloseHandle(h);
        }
        if (ov.hEvent) CloseHandle(ov.hEvent);
    }
};

struct Receiver {
    Channel lng;   // 20-byte long reports (report id 0x11)
    Channel shrt;  // 7-byte short reports (0x10) - HID++ 1.0 errors arrive here
    bool timedOut = false;  // last Request got no answer at all (empty receiver slot / device off)

    // Sends a long request; returns true and fills `out` (16 param bytes) on success.
    bool Request(BYTE dev, BYTE feat, BYTE fn, const BYTE* params, int np, BYTE* out) {
        BYTE req[20]{0x11, dev, feat, BYTE((fn << 4) | SW_ID)};
        for (int i = 0; i < np && i < 16; i++) req[4 + i] = params[i];
        OVERLAPPED wov{}; wov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD n = 0;
        BOOL ok = WriteFile(lng.h, req, 20, nullptr, &wov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(wov.hEvent, TIMEOUT_MS) != WAIT_OBJECT_0) { CancelIo(lng.h); GetOverlappedResult(lng.h, &wov, &n, TRUE); }
            ok = GetOverlappedResult(lng.h, &wov, &n, FALSE);
        }
        CloseHandle(wov.hEvent);
        if (!ok) return false;

#ifdef HIDPP_DEBUG
        fprintf(stderr, "req dev=%02X feat=%02X fn=%d p=%02X%02X t=%lu\n", dev, feat, fn, req[4], req[5], GetTickCount());
#endif
        DWORD deadline = GetTickCount() + TIMEOUT_MS;
        timedOut = false;
        for (;;) {
            if (!lng.Arm()) return false;
            HANDLE evs[2] = {lng.ov.hEvent, nullptr};
            DWORD cnt = 1;
            if (shrt.h != INVALID_HANDLE_VALUE && shrt.Arm()) evs[cnt++] = shrt.ov.hEvent;
            DWORD now = GetTickCount();
            if ((LONG)(deadline - now) <= 0) { timedOut = true; return false; }
            DWORD w = WaitForMultipleObjects(cnt, evs, FALSE, deadline - now);
            if (w == WAIT_TIMEOUT) { timedOut = true; return false; }
            if (w >= WAIT_OBJECT_0 + cnt) return false;
            Channel& c = (w == WAIT_OBJECT_0) ? lng : shrt;
            c.pending = false;
            if (!GetOverlappedResult(c.h, &c.ov, &n, FALSE) || n < 7) continue;
            const BYTE* r = c.buf;
#ifdef HIDPP_DEBUG
            fprintf(stderr, "  [%s] rx(%lu):", &c == &lng ? "L" : "S", n);
            for (DWORD k = 0; k < n && k < 8; k++) fprintf(stderr, " %02X", r[k]);
            fprintf(stderr, "\n");
#endif
            if (r[1] != dev) continue;
            BYTE fnsw = BYTE((fn << 4) | SW_ID);
            if (r[0] == 0x11 && r[2] == feat && r[3] == fnsw) { memcpy(out, r + 4, 16); return true; }
            if ((r[2] == 0xFF || r[2] == 0x8F) && r[3] == feat && r[4] == fnsw) return false;  // error reply
        }
    }

    BYTE FeatureIndex(BYTE dev, WORD feature) {
        BYTE p[2] = {BYTE(feature >> 8), BYTE(feature)}, r[16];
        return Request(dev, 0, 0, p, 2, r) ? r[0] : 0;
    }
};

struct DevInfo { std::wstring name; Kind kind = Kind::Other; BYTE fUnified = 0, fStatus = 0, fVoltage = 0; };
// Both keyed by interface + device index; only touched by the worker thread.
std::map<std::wstring, DevInfo> g_cache;  // identified devices
std::map<std::wstring, DWORD> g_empty;    // slots that did not answer -> when we last tried

int VoltageToPercent(int mv) {
    static const int t[][2] = {{4186, 100}, {4067, 90}, {3989, 80}, {3922, 70}, {3870, 60}, {3830, 50},
                               {3790, 40},  {3755, 30}, {3720, 20}, {3680, 10}, {3500, 0}};
    if (mv >= t[0][0]) return 100;
    for (int i = 1; i < 11; i++)
        if (mv >= t[i][0]) return t[i][1] + (mv - t[i][0]) * (t[i - 1][1] - t[i][1]) / (t[i - 1][0] - t[i][0]);
    return 0;
}

bool Identify(Receiver& rx, BYTE dev, DevInfo& info) {
    info.fUnified = rx.FeatureIndex(dev, 0x1004);
    if (rx.timedOut) return false;  // nothing there; don't wait for the other probes too
    if (!info.fUnified) info.fStatus = rx.FeatureIndex(dev, 0x1000);
    if (!info.fUnified && !info.fStatus) info.fVoltage = rx.FeatureIndex(dev, 0x1001);
    if (!info.fUnified && !info.fStatus && !info.fVoltage) return false;

    BYTE r[16];
    if (BYTE fName = rx.FeatureIndex(dev, 0x0005)) {
        if (rx.Request(dev, fName, 0, nullptr, 0, r)) {
            int total = r[0];
            std::string s;
            while ((int)s.size() < total) {
                BYTE off = BYTE(s.size());
                if (!rx.Request(dev, fName, 1, &off, 1, r)) break;
                for (int i = 0; i < 16 && (int)s.size() < total; i++) s += char(r[i]);
            }
            while (!s.empty() && (s.back() == ' ' || s.back() == 0)) s.pop_back();
            info.name.assign(s.begin(), s.end());
        }
        if (rx.Request(dev, fName, 2, nullptr, 0, r)) {
            switch (r[0]) {
                case 0: case 2: info.kind = Kind::Keyboard; break;
                case 3: case 4: case 5: info.kind = Kind::Mouse; break;
                case 8: info.kind = Kind::Headset; break;
                default: info.kind = Kind::Other;
            }
        }
    }
    if (info.name.empty()) info.name = L"Logitech device";
    return true;
}

bool ReadBattery(Receiver& rx, BYTE dev, const DevInfo& info, Device& out) {
    BYTE r[16];
    if (info.fUnified) {
        if (!rx.Request(dev, info.fUnified, 1, nullptr, 0, r)) return false;
        int pct = r[0];
        if (pct == 0 && r[1]) pct = (r[1] & 8) ? 100 : (r[1] & 4) ? 50 : (r[1] & 2) ? 20 : 5;  // level-only devices
        out.percent = pct;
        out.charging = r[2] == 1 || r[2] == 2;
    } else if (info.fStatus) {
        if (!rx.Request(dev, info.fStatus, 0, nullptr, 0, r)) return false;
        out.percent = r[0];
        out.charging = r[2] >= 1 && r[2] <= 4;
    } else {
        if (!rx.Request(dev, info.fVoltage, 0, nullptr, 0, r)) return false;
        out.percent = VoltageToPercent((r[0] << 8) | r[1]);
        out.charging = (r[2] & 0x80) != 0;
    }
    if (out.percent > 100) out.percent = 100;
    return true;
}

struct Collection { std::wstring path; USHORT usage; DWORD inLen; };

}  // namespace

void QueryLogitech(std::vector<Device>& out, bool rescan) {
    // Group the Logitech vendor collections (page 0xFF00) by interface instance.
    std::map<std::wstring, std::pair<Collection, Collection>> ifaces;  // key -> (long, short)
    GUID g; HidD_GetHidGuid(&g);
    HDEVINFO s = SetupDiGetClassDevsW(&g, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (s == INVALID_HANDLE_VALUE) return;
    SP_DEVICE_INTERFACE_DATA d{sizeof(d)};
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(s, nullptr, &g, i, &d); i++) {
        BYTE buf[1024]; auto* det = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)buf; det->cbSize = sizeof(*det);
        if (!SetupDiGetDeviceInterfaceDetailW(s, &d, det, sizeof(buf), nullptr, nullptr)) continue;
        std::wstring path = det->DevicePath;
        std::wstring low = path; CharLowerW(low.data());
        if (low.find(L"vid_046d") == std::wstring::npos) continue;
        HANDLE h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        PHIDP_PREPARSED_DATA pp;
        if (HidD_GetPreparsedData(h, &pp)) {
            HIDP_CAPS caps;
            if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS && caps.UsagePage == 0xFF00) {
                // Strip the "&colNN" part so both collections of one interface share a key.
                std::wstring key = low;
                size_t c = key.find(L"&col");
                if (c != std::wstring::npos) key.erase(c, 6);
                Collection col{path, caps.Usage, caps.InputReportByteLength};
                if (caps.Usage == 2 && caps.InputReportByteLength == 20) ifaces[key].first = col;
                if (caps.Usage == 1 && caps.InputReportByteLength == 7) ifaces[key].second = col;
            }
            HidD_FreePreparsedData(pp);
        }
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(s);

    for (auto& [key, cols] : ifaces) {
        if (cols.first.path.empty()) continue;
        Receiver rx;
        if (!rx.lng.Open(cols.first.path, 20)) continue;
        if (!cols.second.path.empty()) rx.shrt.Open(cols.second.path, 7);
        for (BYTE dev : {BYTE(0xFF), BYTE(1), BYTE(2), BYTE(3), BYTE(4), BYTE(5), BYTE(6)}) {
            std::wstring ck = key + L"#" + std::to_wstring(dev);
            auto it = g_cache.find(ck);
            if (it == g_cache.end()) {
                // Slots that never answered are only re-probed occasionally: each costs a full timeout.
                auto e = g_empty.find(ck);
                if (!rescan && e != g_empty.end() && GetTickCount() - e->second < EMPTY_RESCAN_MS) continue;
                DevInfo info;
                if (!Identify(rx, dev, info)) { g_empty[ck] = GetTickCount(); continue; }
                g_empty.erase(ck);
                it = g_cache.emplace(ck, info).first;
            }
            Device dv;
            dv.name = it->second.name;
            dv.kind = it->second.kind;
            if (ReadBattery(rx, dev, it->second, dv)) {
                out.push_back(dv);
                if (dev == 0xFF) break;  // directly connected device: no receiver slots to scan
            } else if (!rx.timedOut) {
                g_cache.erase(it);  // explicit error (unpaired / different device): re-identify next time
            }  // timeout = known device asleep or off: keep it and just try again next poll
        }
    }
}
