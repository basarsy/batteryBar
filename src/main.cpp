// BatteryBar - shows battery levels of wireless peripherals inside the Windows taskbar.
//
// Threads:
//   UI thread     - owns a hidden controller window and the overlay window that is
//                   parented to the taskbar (Shell_TrayWnd). Because the overlay is a
//                   child of Explorer's window, this thread must NEVER block.
//   worker thread - does all device I/O and posts results back.
#include "devices.h"
#include <windows.h>
#include <dbt.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>

namespace {

constexpr wchar_t kAppName[] = L"BatteryBar";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kAppKey[] = L"Software\\BatteryBar";
constexpr UINT WM_APP_DEVICES = WM_APP + 1;
constexpr UINT_PTR TIMER_LAYOUT = 1;
constexpr DWORD POLL_MS = 60 * 1000;            // battery drains slowly; once a minute is plenty
constexpr DWORD STALE_KEEP_MS = 30 * 60 * 1000; // keep showing (dimmed) a device that stopped answering
constexpr int LOW_BATTERY = 15;

enum Cmd { CMD_REFRESH = 100, CMD_AUTOSTART, CMD_EXIT };

struct Entry { Device dev; DWORD lastSeen; bool stale; };

HINSTANCE g_inst;
HWND g_ctl, g_bar;
bool g_barIsChild;
UINT g_msgTaskbarCreated;
HANDLE g_wake, g_quit;
std::map<std::wstring, Entry> g_entries;  // UI thread only

// Cached layout state so the 1s timer only does work when something changed.
struct { RECT tray{}, notify{}; UINT dpi = 0; bool light = false; SIZE size{}; POINT pos{}; } g_last;
struct { HFONT icon = nullptr, text = nullptr; UINT dpi = 0; } g_fonts;

// ---------------------------------------------------------------- settings ---

bool IsLightTaskbar() {
    DWORD v = 0, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v != 0;
}

bool AutostartEnabled() {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kAppName, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

void SetAutostart(bool on) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr)) return;
    if (on) {
        wchar_t exe[MAX_PATH], val[MAX_PATH + 2];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        swprintf(val, MAX_PATH + 2, L"\"%ls\"", exe);
        RegSetValueExW(k, kAppName, 0, REG_SZ, (BYTE*)val, DWORD((wcslen(val) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kAppName);
    }
    RegCloseKey(k);
}

// Enable autostart the first time the app ever runs; after that respect the user's choice.
void FirstRunSetup() {
    HKEY k; DWORD disp;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kAppKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, &disp)) return;
    RegCloseKey(k);
    if (disp == REG_CREATED_NEW_KEY || AutostartEnabled()) SetAutostart(true);  // (re)point it at this exe
}

// ------------------------------------------------------------------ worker ---

DWORD WINAPI Worker(LPVOID) {
    HANDLE evs[2] = {g_quit, g_wake};
    for (bool first = true;; first = false) {
        bool rescan = first;  // full scan at start and after plug/resume/manual refresh; cheap poll otherwise
        if (!first) {
            DWORD w = WaitForMultipleObjects(2, evs, FALSE, POLL_MS);
            if (w == WAIT_OBJECT_0) break;
            if (w == WAIT_OBJECT_0 + 1) {
                if (WaitForSingleObject(g_quit, 1500) == WAIT_OBJECT_0) break;  // debounce plug events
                rescan = true;
            }
        }
        auto* list = new std::vector<Device>(QueryAllDevices(rescan));
        if (!PostMessageW(g_ctl, WM_APP_DEVICES, 0, (LPARAM)list)) delete list;
    }
    return 0;
}

// --------------------------------------------------------------- rendering ---

bool FontExists(const wchar_t* face) {
    LOGFONTW lf{}; lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
    bool found = false;
    HDC dc = GetDC(nullptr);
    EnumFontFamiliesExW(dc, &lf, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) -> int { *(bool*)p = true; return 0; }, (LPARAM)&found, 0);
    ReleaseDC(nullptr, dc);
    return found;
}

void EnsureFonts(UINT dpi) {
    if (g_fonts.dpi == dpi) return;
    if (g_fonts.icon) DeleteObject(g_fonts.icon);
    if (g_fonts.text) DeleteObject(g_fonts.text);
    static const wchar_t* iconFace = FontExists(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    static const wchar_t* textFace = FontExists(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    g_fonts.icon = CreateFontW(-MulDiv(16, dpi, 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, iconFace);
    g_fonts.text = CreateFontW(-MulDiv(12, dpi, 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, textFace);
    g_fonts.dpi = dpi;
}

const wchar_t* Glyph(Kind k) {
    switch (k) {
        case Kind::Headset: return L"";
        case Kind::Mouse: return L"";
        case Kind::Keyboard: return L"";
        case Kind::Gamepad: return L"";
        default: return L"";
    }
}

int KindOrder(Kind k) { return k == Kind::Headset ? 0 : k == Kind::Mouse ? 1 : k == Kind::Keyboard ? 2 : k == Kind::Gamepad ? 3 : 4; }

std::vector<const Entry*> SortedEntries() {
    std::vector<const Entry*> v;
    for (auto& [_, e] : g_entries) v.push_back(&e);
    std::stable_sort(v.begin(), v.end(), [](auto a, auto b) { return KindOrder(a->dev.kind) < KindOrder(b->dev.kind); });
    return v;
}

HWND Taskbar() { return FindWindowW(L"Shell_TrayWnd", nullptr); }

// Where the overlay's right edge should be (screen coords): just left of the notification area.
bool TaskbarGeometry(RECT& tray, RECT& notify) {
    HWND t = Taskbar();
    if (!t || !GetWindowRect(t, &tray)) return false;
    HWND n = FindWindowExW(t, nullptr, L"TrayNotifyWnd", nullptr);
    if (!n || !GetWindowRect(n, &notify) || notify.right <= notify.left) {
        notify = tray;
        notify.left = tray.right - MulDiv(300, GetDpiForWindow(t), 96);
    }
    return true;
}

void Render(bool force = false) {
    if (!g_bar) return;
    RECT tray, notify;
    if (!TaskbarGeometry(tray, notify)) return;
    UINT dpi = GetDpiForWindow(Taskbar());
    bool light = IsLightTaskbar();
    auto items = SortedEntries();
    if (items.empty()) { ShowWindow(g_bar, SW_HIDE); return; }
    EnsureFonts(dpi);

    auto S = [dpi](int v) { return MulDiv(v, dpi, 96); };
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);

    // ---- measure
    struct Part { const wchar_t* glyph; std::wstring text; int x, iconW, textW, boltW; COLORREF color; BYTE opacity; };
    std::vector<Part> parts;
    const int pad = S(6), gap = S(4), spacing = S(12);
    int x = pad;
    for (auto* e : items) {
        Part p{Glyph(e->dev.kind), std::to_wstring(e->dev.percent) + L"%", x, 0, 0, 0, 0, BYTE(e->stale ? 110 : 255)};
        SIZE sz;
        SelectObject(dc, g_fonts.icon);
        GetTextExtentPoint32W(dc, p.glyph, 1, &sz); p.iconW = sz.cx;
        if (e->dev.charging) { GetTextExtentPoint32W(dc, L"", 1, &sz); p.boltW = sz.cx * 3 / 4; }
        SelectObject(dc, g_fonts.text);
        GetTextExtentPoint32W(dc, p.text.c_str(), (int)p.text.size(), &sz); p.textW = sz.cx;
        bool low = e->dev.percent <= LOW_BATTERY && !e->dev.charging;
        p.color = low ? (light ? RGB(196, 43, 28) : RGB(255, 99, 85)) : (light ? RGB(0, 0, 0) : RGB(255, 255, 255));
        x += p.iconW + gap + p.textW + p.boltW + spacing;
        parts.push_back(std::move(p));
    }
    const int W = x - spacing + pad;
    const int H = tray.bottom - tray.top;

    // ---- draw white-on-black, then turn luminance into per-pixel alpha
    BITMAPINFO bi{}; bi.bmiHeader = {sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB};
    DWORD* px = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&px, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    memset(px, 0, size_t(W) * H * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    std::vector<int> owner(W, -1);
    for (size_t i = 0; i < parts.size(); i++) {
        auto& p = parts[i];
        TEXTMETRICW tm;
        SelectObject(dc, g_fonts.icon); GetTextMetricsW(dc, &tm);
        TextOutW(dc, p.x, (H - tm.tmHeight) / 2, p.glyph, 1);
        SelectObject(dc, g_fonts.text); GetTextMetricsW(dc, &tm);
        int tx = p.x + p.iconW + gap;
        TextOutW(dc, tx, (H - tm.tmHeight) / 2 - S(1) / 2, p.text.c_str(), (int)p.text.size());
        if (p.boltW) {
            HFONT small = CreateFontW(-S(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0,
                                      FontExists(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets");
            SelectObject(dc, small); GetTextMetricsW(dc, &tm);
            TextOutW(dc, tx + p.textW, (H - tm.tmHeight) / 2, L"", 1);
            SelectObject(dc, g_fonts.text);
            DeleteObject(small);
        }
        int end = std::min(W, p.x + p.iconW + gap + p.textW + p.boltW + spacing / 2);
        for (int c = std::max(0, p.x - spacing / 2); c < end; c++) owner[c] = (int)i;
    }
    GdiFlush();
    static BYTE gamma[256];
    if (!gamma[255]) for (int i = 0; i < 256; i++) gamma[i] = BYTE(std::lround(255.0 * std::pow(i / 255.0, 0.8)));
    for (int yy = 0; yy < H; yy++) {
        DWORD* row = px + size_t(yy) * W;
        for (int xx = 0; xx < W; xx++) {
            DWORD v = row[xx];
            int a = std::max({int(v & 0xFF), int((v >> 8) & 0xFF), int((v >> 16) & 0xFF)});
            int o = owner[xx];
            if (a == 0 || o < 0) { row[xx] = 0x01000000; continue; }  // alpha 1: invisible but still clickable
            a = gamma[a] * parts[o].opacity / 255;
            if (a < 1) a = 1;
            COLORREF c = parts[o].color;
            row[xx] = (DWORD(a) << 24) | ((GetRValue(c) * a / 255) << 16) | ((GetGValue(c) * a / 255) << 8) | (GetBValue(c) * a / 255);
        }
    }

    // ---- place: right-aligned against the notification area, full taskbar height
    POINT pos{notify.left - W - S(4), tray.top};
    if (g_barIsChild) { pos.x -= tray.left; pos.y = 0; }
    SIZE size{W, H};
    POINT src{0, 0};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(g_bar, screen, nullptr, &size, dc, &src, 0, &bf, ULW_ALPHA);
    SetWindowPos(g_bar, g_barIsChild ? HWND_TOP : HWND_TOPMOST, pos.x, pos.y, W, H, SWP_NOACTIVATE | SWP_SHOWWINDOW);

    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    g_last = {tray, notify, dpi, light, size, pos};
    (void)force;
}

// Called every second: cheap checks, re-render only if the taskbar moved/resized or theme/DPI changed.
void CheckLayout() {
    if (!g_bar || !IsWindow(g_bar)) return;
    RECT tray, notify;
    if (!TaskbarGeometry(tray, notify)) return;
    if (!EqualRect(&tray, &g_last.tray) || !EqualRect(&notify, &g_last.notify) ||
        GetDpiForWindow(Taskbar()) != g_last.dpi || IsLightTaskbar() != g_last.light)
        Render();
    else if (!g_barIsChild)
        SetWindowPos(g_bar, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);  // stay above the taskbar
}

// ------------------------------------------------------------------ windows ---

void ShowMenu() {
    HMENU m = CreatePopupMenu();
    auto items = SortedEntries();
    if (items.empty()) AppendMenuW(m, MF_GRAYED, 0, L"No devices found");
    for (auto* e : items) {
        wchar_t line[256];
        swprintf(line, 256, L"%ls  —  %d%%%ls%ls", e->dev.name.c_str(), e->dev.percent,
                 e->dev.charging ? L"  (charging)" : L"", e->stale ? L"  (not responding)" : L"");
        AppendMenuW(m, MF_GRAYED, 0, line);
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_REFRESH, L"Refresh now");
    AppendMenuW(m, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), CMD_AUTOSTART, L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_EXIT, L"Exit");
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(g_ctl);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_ctl, nullptr);
    PostMessageW(g_ctl, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (cmd == CMD_REFRESH) SetEvent(g_wake);
    if (cmd == CMD_AUTOSTART) SetAutostart(!AutostartEnabled());
    if (cmd == CMD_EXIT) DestroyWindow(g_ctl);
}

LRESULT CALLBACK BarProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
            // Don't run a modal menu loop inside a message sent from Explorer's thread.
            PostMessageW(g_ctl, WM_APP + 2, 0, 0);
            return 0;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_DESTROY: if (h == g_bar) g_bar = nullptr; return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void CreateBar() {
    if (g_bar && IsWindow(g_bar)) DestroyWindow(g_bar);
    g_bar = nullptr;
    HWND tray = Taskbar();
    if (tray) {
        // Preferred: live inside the taskbar, so we follow auto-hide, full-screen apps and z-order for free.
        g_bar = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE, L"BatteryBarOverlay", kAppName,
                                WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 1, 1, tray, nullptr, g_inst, nullptr);
        g_barIsChild = g_bar != nullptr;
    }
    if (!g_bar) {
        // Fallback: a topmost tool window floating over the taskbar.
        g_bar = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"BatteryBarOverlay",
                                kAppName, WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, g_inst, nullptr);
        g_barIsChild = false;
    }
    g_last = {};
    Render(true);
}

LRESULT CALLBACK CtlProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_msgTaskbarCreated && msg) {  // Explorer restarted: our child window died with the old taskbar
        CreateBar();
        return 0;
    }
    switch (msg) {
        case WM_APP_DEVICES: {
            std::unique_ptr<std::vector<Device>> list((std::vector<Device>*)lp);
            DWORD now = GetTickCount();
            for (auto& [_, e] : g_entries) e.stale = true;
            for (auto& d : *list) g_entries[d.name] = {d, now, false};
            for (auto it = g_entries.begin(); it != g_entries.end();)
                it = (it->second.stale && now - it->second.lastSeen > STALE_KEEP_MS) ? g_entries.erase(it) : std::next(it);
            Render();
            return 0;
        }
        case WM_APP + 2: ShowMenu(); return 0;
        case WM_TIMER:
            if (!g_bar || !IsWindow(g_bar)) CreateBar(); else CheckLayout();
            return 0;
        case WM_SETTINGCHANGE: Render(); return 0;
        case WM_DEVICECHANGE:
            if (wp == DBT_DEVNODES_CHANGED) SetEvent(g_wake);
            return TRUE;
        case WM_POWERBROADCAST:
            if (wp == PBT_APMRESUMEAUTOMATIC) SetEvent(g_wake);
            return TRUE;
        case WM_DESTROY:
            if (g_bar && IsWindow(g_bar)) DestroyWindow(g_bar);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// `BatteryBar.exe --list` prints what it finds (handy for debugging).
int ListToConsole() {
    // Prefer an inherited (redirected) stdout; otherwise attach to the parent console.
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    FILE* f = nullptr;
    if (out && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN) {
        f = stdout;
    } else {
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
        f = _wfopen(L"CONOUT$", L"w");
    }
    DWORD t0 = GetTickCount();
    std::vector<Device> devs;
    QueryLogitech(devs, true);
    DWORD tFull = GetTickCount() - t0;
    devs.clear();
    t0 = GetTickCount();
    QueryLogitech(devs, false);  // what a regular poll costs once empty slots are known
    DWORD t1 = GetTickCount();
    QueryBluetooth(devs);
    fwprintf(f, L"(logitech: first scan %lums, regular poll %lums; bluetooth %lums)\n", tFull, t1 - t0, GetTickCount() - t1);
    if (devs.empty()) fwprintf(f, L"No devices with battery info found.\n");
    static const wchar_t* kinds[] = {L"headset", L"mouse", L"keyboard", L"gamepad", L"other"};
    for (auto& d : devs)
        fwprintf(f, L"%-9ls %3d%% %ls %ls\n", kinds[(int)d.kind], d.percent, d.charging ? L"(charging)" : L"          ", d.name.c_str());
    fflush(f);
    if (f != stdout) fclose(f);
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmd, int) {
    g_inst = inst;
    if (wcsstr(cmd, L"--list")) return ListToConsole();

    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\BatteryBar.Singleton");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    FirstRunSetup();
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc{};
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpfnWndProc = CtlProc; wc.lpszClassName = L"BatteryBarCtl"; RegisterClassW(&wc);
    wc.lpfnWndProc = BarProc; wc.lpszClassName = L"BatteryBarOverlay"; RegisterClassW(&wc);

    // A real (hidden) top-level window: it receives TaskbarCreated / device-change broadcasts.
    g_ctl = CreateWindowExW(WS_EX_TOOLWINDOW, L"BatteryBarCtl", kAppName, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    ChangeWindowMessageFilterEx(g_ctl, g_msgTaskbarCreated, MSGFLT_ALLOW, nullptr);

    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_quit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CreateBar();
    SetTimer(g_ctl, TIMER_LAYOUT, 1000, nullptr);
    HANDLE worker = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    SetThreadPriority(worker, THREAD_PRIORITY_BELOW_NORMAL);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }

    SetEvent(g_quit);
    WaitForSingleObject(worker, 3000);
    // Drop any result the worker posted after the window was gone.
    while (PeekMessageW(&m, nullptr, WM_APP_DEVICES, WM_APP_DEVICES, PM_REMOVE)) delete (std::vector<Device>*)m.lParam;
    ReleaseMutex(single);
    return 0;
}
