// ==WindhawkMod==
// @id              berlian-desktop-clock
// @name            Berlian Desktop Clock
// @version         1.0
// @author          Muhammad Althaf Mahmudin
// @github          mahmudalthaf
// @include explorer.exe
// @architecture x86-64
// @compilerOptions -lgdi32 -luser32
// @license MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Berlian Desktop Clock 1.0
Native Windows 11 wallpaper clock, visually matching the earlier Python clock.

- Large Segoe UI Semibold time (24-hour format), optional Indonesian date.
- Transparent background, no always-on-top overlay, no Python or panel process.
- Position/color/size adjusted via Windhawk **Settings**.
- X/Y are **screen pixel coordinates**; -1 means center on primary monitor.
- Detects changes in Explorer's wallpaper host and recovers when possible.
- Only redraws when minutes/date/settings change, while checking the host periodically.

**Limitations:** Explorer's WorkerW/Progman desktop arrangement is undocumented.
Compatibility after Windows updates cannot be guaranteed. Test the mod before relying
on it. If your desktop acts strangely, disable the mod in Windhawk.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- fontSize: 112
  $name: Ukuran angka jam (point)
- showDate: true
  $name: Tampilkan tanggal bahasa Indonesia
- color: white
  $name: Warna jam
  $options:
  - white: Putih
  - black: Hitam
- x: -1
  $name: Posisi X di layar (-1 = tengah)
- y: -1
  $name: Posisi Y di layar (-1 = tengah)
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <string>

namespace {
constexpr wchar_t kClassClock[] = L"BerlianClockV1Window";
constexpr wchar_t kClassController[] = L"BerlianClockV1Controller";
constexpr UINT kReload = WM_APP + 8;
constexpr UINT_PTR kTimer = 1;
constexpr UINT kHostIntervalMs = 2000;
constexpr COLORREF kKey = RGB(1, 2, 3);

struct Settings {
    int fontSize = 112;
    bool showDate = true;
    bool black = false;
    int x = -1;
    int y = -1;
};

SRWLOCK g_lock = SRWLOCK_INIT;
Settings g_settings;
std::atomic<bool> g_stopping{false};
std::atomic<HWND> g_controller{nullptr};
HANDLE g_thread = nullptr;
DWORD g_threadId = 0;

// These are only read/written by the window thread.
HWND g_clock = nullptr;
HWND g_host = nullptr;
HFONT g_timeFont = nullptr;
HFONT g_dateFont = nullptr;
int g_fontSize = -1;
UINT g_dpi = 0;
SYSTEMTIME g_drawnTime{};
bool g_hasDrawnTime = false;
bool g_dirty = true;
DWORD64 g_lastTick = 0;

Settings ReadSettings() {
    Settings s;
    s.fontSize = std::max(56, std::min(180, (int)Wh_GetIntSetting(L"fontSize")));
    s.showDate = Wh_GetIntSetting(L"showDate") != 0;
    PCWSTR col = Wh_GetStringSetting(L"color");
    s.black = col && lstrcmpiW(col, L"black") == 0;
    if (col) Wh_FreeStringSetting(col);
    s.x = Wh_GetIntSetting(L"x");
    s.y = Wh_GetIntSetting(L"y");
    return s;
}
Settings GetSettings() {
    AcquireSRWLockShared(&g_lock);
    Settings s = g_settings;
    ReleaseSRWLockShared(&g_lock);
    return s;
}
void LoadSettings() {
    Settings s = ReadSettings();
    AcquireSRWLockExclusive(&g_lock);
    g_settings = s;
    ReleaseSRWLockExclusive(&g_lock);
}

HWND FindHost() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) return nullptr;
    // First handle nested WorkerW on recent Windows 11 builds.
    HWND worker = nullptr;
    while ((worker = FindWindowExW(progman, worker, L"WorkerW", nullptr))) {
        if (!FindWindowExW(worker, nullptr, L"SHELLDLL_DefView", nullptr))
            return worker;
    }
    // Then handle the classic top-level WorkerW that follows the icon view.
    struct Context { bool afterIcons = false; HWND worker = nullptr; } ctx;
    EnumWindows([](HWND wnd, LPARAM value) -> BOOL {
        auto* c = reinterpret_cast<Context*>(value);
        if (FindWindowExW(wnd, nullptr, L"SHELLDLL_DefView", nullptr)) {
            c->afterIcons = true;
        } else if (c->afterIcons) {
            wchar_t name[40]{};
            GetClassNameW(wnd, name, 40);
            if (lstrcmpW(name, L"WorkerW") == 0) {
                c->worker = wnd;
                return FALSE;
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.worker) return ctx.worker;
    if (FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr))
        return progman;
    return nullptr;
}

void DeleteFonts() {
    if (g_timeFont) DeleteObject(g_timeFont);
    if (g_dateFont) DeleteObject(g_dateFont);
    g_timeFont = nullptr;
    g_dateFont = nullptr;
    g_fontSize = -1;
    g_dpi = 0;
}

UINT WindowDpi(HWND hwnd) {
    // GetDpiForWindow is available on modern Windows 10/11.
    auto proc = reinterpret_cast<UINT (WINAPI*)(HWND)>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    UINT result = proc ? proc(hwnd) : 0;
    if (!result) {
        HDC dc = GetDC(hwnd);
        result = dc ? (UINT)GetDeviceCaps(dc, LOGPIXELSY) : 96;
        if (dc) ReleaseDC(hwnd, dc);
    }
    return result ? result : 96;
}

void EnsureFonts(HWND hwnd, const Settings& s) {
    UINT dpi = WindowDpi(hwnd);
    if (g_timeFont && g_dateFont && g_fontSize == s.fontSize && g_dpi == dpi)
        return;
    DeleteFonts();
    g_timeFont = CreateFontW(-MulDiv(s.fontSize, (int)dpi, 72), 0, 0, 0,
                             FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI Semibold");
    g_dateFont = CreateFontW(-MulDiv(18, (int)dpi, 72), 0, 0, 0,
                             FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI Semibold");
    g_fontSize = s.fontSize;
    g_dpi = dpi;
}

std::wstring DateString(const SYSTEMTIME& t) {
    static const wchar_t* days[] = {L"Minggu", L"Senin", L"Selasa", L"Rabu", L"Kamis", L"Jumat", L"Sabtu"};
    static const wchar_t* months[] = {L"", L"Januari", L"Februari", L"Maret", L"April", L"Mei", L"Juni",
                                      L"Juli", L"Agustus", L"September", L"Oktober", L"November", L"Desember"};
    wchar_t buf[128]{};
    wsprintfW(buf, L"%s, %u %s %u", days[t.wDayOfWeek % 7], t.wDay,
              months[(t.wMonth >= 1 && t.wMonth <= 12) ? t.wMonth : 1], t.wYear);
    return std::wstring(buf);
}

void DrawClock(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    if (!dc) return;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    HBRUSH brush = CreateSolidBrush(kKey);
    FillRect(dc, &rc, brush);
    DeleteObject(brush);
    SetBkMode(dc, TRANSPARENT);
    Settings s = GetSettings();
    EnsureFonts(hwnd, s);
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t timeText[16]{};
    wsprintfW(timeText, L"%02u:%02u", now.wHour, now.wMinute);
    SetTextColor(dc, s.black ? RGB(0, 0, 0) : RGB(247, 251, 255));
    if (g_timeFont) {
        HGDIOBJ previous = SelectObject(dc, g_timeFont);
        RECT timeRect = rc;
        timeRect.bottom = std::max(1, (int)rc.bottom - (s.showDate ? 42 : 0));
        DrawTextW(dc, timeText, -1, &timeRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, previous);
    }
    if (s.showDate && g_dateFont) {
        SetTextColor(dc, s.black ? RGB(25, 25, 25) : RGB(214, 231, 244));
        HGDIOBJ previous = SelectObject(dc, g_dateFont);
        RECT dateRect = rc;
        dateRect.top = rc.bottom - 50;
        std::wstring date = DateString(now);
        DrawTextW(dc, date.c_str(), -1, &dateRect,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, previous);
    }
    g_drawnTime = now;
    g_hasDrawnTime = true;
    g_dirty = false;
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK ClockProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_PAINT) { DrawClock(hwnd); return 0; }
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_NCDESTROY && hwnd == g_clock) {
        g_clock = nullptr;
        g_host = nullptr;
        g_dirty = true;
        g_hasDrawnTime = false;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

void RemoveClock() {
    HWND old = g_clock;
    g_clock = nullptr;
    g_host = nullptr;
    if (old && IsWindow(old)) DestroyWindow(old);
    g_dirty = true;
    g_hasDrawnTime = false;
}

void UpdateClock(bool settingsChanged) {
    if (g_stopping.load()) return;
    Settings s = GetSettings();
    HWND host = FindHost();
    if (!host || !IsWindow(host)) {
        RemoveClock();
        return;
    }
    bool recreated = false;
    if (!g_clock || !IsWindow(g_clock) || GetParent(g_clock) != host) {
        RemoveClock();
        g_host = host;
        g_clock = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED,
            kClassClock, L"Berlian Desktop Clock", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
            0, 0, 500, 180, host, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!g_clock) { g_host = nullptr; return; }
        SetLayeredWindowAttributes(g_clock, kKey, 255, LWA_COLORKEY);
        recreated = true;
        g_dirty = true;
    }
    int width = std::max(430, s.fontSize * 6);
    int height = std::max(140, s.fontSize * 2 + (s.showDate ? 22 : 0));
    int sx = s.x < 0 ? (GetSystemMetrics(SM_CXSCREEN) - width) / 2 : s.x;
    int sy = s.y < 0 ? (GetSystemMetrics(SM_CYSCREEN) - height) / 2 : s.y;
    POINT pos{sx, sy};
    MapWindowPoints(nullptr, host, &pos, 1);
    HWND after = HWND_TOP;
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (host == progman) {
        HWND icons = FindWindowExW(host, nullptr, L"SHELLDLL_DefView", nullptr);
        if (icons) after = icons;
    }
    RECT actual{};
    GetWindowRect(g_clock, &actual);
    POINT current{actual.left, actual.top};
    MapWindowPoints(nullptr, host, &current, 1);
    bool changedGeometry = recreated || settingsChanged ||
        current.x != pos.x || current.y != pos.y ||
        actual.right - actual.left != width || actual.bottom - actual.top != height;
    // Reassert stacking against wallpaper after Explorer rearranges its WorkerW children.
    if (changedGeometry) {
        SetWindowPos(g_clock, after, pos.x, pos.y, width, height,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
        g_dirty = true;
    } else {
        SetWindowPos(g_clock, after, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    }
    SYSTEMTIME now{};
    GetLocalTime(&now);
    bool changedMinute = !g_hasDrawnTime || now.wMinute != g_drawnTime.wMinute ||
                         now.wHour != g_drawnTime.wHour || now.wDay != g_drawnTime.wDay ||
                         now.wMonth != g_drawnTime.wMonth || now.wYear != g_drawnTime.wYear;
    if (settingsChanged) { DeleteFonts(); g_dirty = true; }
    if (g_dirty || changedMinute) InvalidateRect(g_clock, nullptr, FALSE);
}

LRESULT CALLBACK ControllerProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
        case WM_CREATE:
            if (!SetTimer(hwnd, kTimer, kHostIntervalMs, nullptr)) return -1;
            return 0;
        case WM_TIMER:
            if (w == kTimer) {
                DWORD64 current = GetTickCount64();
                bool resumed = g_lastTick && current - g_lastTick > 10000;
                g_lastTick = current;
                UpdateClock(resumed);
            }
            return 0;
        case kReload:
            UpdateClock(true);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kTimer);
            RemoveClock();
            DeleteFonts();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

DWORD WINAPI ClockThread(LPVOID) {
    HINSTANCE module = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.hInstance = module;
    wc.lpfnWndProc = ClockProc;
    wc.lpszClassName = kClassClock;
    if (!RegisterClassW(&wc)) return 1;
    WNDCLASSW controllerClass{};
    controllerClass.hInstance = module;
    controllerClass.lpfnWndProc = ControllerProc;
    controllerClass.lpszClassName = kClassController;
    if (!RegisterClassW(&controllerClass)) {
        UnregisterClassW(kClassClock, module);
        return 2;
    }
    HWND controller = CreateWindowExW(0, kClassController, L"", 0,
                          0, 0, 0, 0, HWND_MESSAGE, nullptr, module, nullptr);
    if (controller) {
        g_controller.store(controller);
        UpdateClock(false);
        if (g_stopping.load()) PostMessageW(controller, WM_CLOSE, 0, 0);
        MSG m{};
        while (GetMessageW(&m, nullptr, 0, 0) > 0) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if (IsWindow(controller)) DestroyWindow(controller);
    }
    RemoveClock();
    DeleteFonts();
    g_controller.store(nullptr);
    UnregisterClassW(kClassController, module);
    UnregisterClassW(kClassClock, module);
    return 0;
}
} // namespace

BOOL Wh_ModInit() {
    g_stopping.store(false);
    LoadSettings();
    g_thread = CreateThread(nullptr, 0, ClockThread, nullptr, 0, &g_threadId);
    if (!g_thread) Wh_Log(L"Clock thread could not start: %lu", GetLastError());
    return g_thread != nullptr;
}
void Wh_ModSettingsChanged() {
    LoadSettings();
    HWND hwnd = g_controller.load();
    if (hwnd) PostMessageW(hwnd, kReload, 0, 0);
}
void Wh_ModUninit() {
    g_stopping.store(true);
    HWND hwnd = g_controller.load();
    if (hwnd) PostMessageW(hwnd, WM_CLOSE, 0, 0);
    // Worker thread uses only bounded/nonblocking operations on its event loop.
    // Do not unload code while that thread might still execute it.
    if (g_thread) {
        WaitForSingleObject(g_thread, INFINITE);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
}
