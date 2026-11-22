// TrayMeter: a very light CPU and memory meter for the Windows notification area.
//
// One tray icon holds two Task Manager-style gauges: CPU on the left in Task Manager's blue, memory on the right in
// green, yellow or red by load. Each tick reads two system-wide totals (GetSystemTimes or one PDH counter, plus
// GlobalMemoryStatusEx) and hands Explorer a new icon only when the gauges' pixels change. The tooltip carries the
// exact numbers and refreshes while the pointer is over the icon. Nothing enumerates processes, which is the
// expensive part of Task Manager itself.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <wtsapi32.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <wchar.h>
#include <stdio.h>

#define APP_NAME L"TrayMeter"
#define WINDOW_CLASS L"TrayMeterWindow"
#define SINGLE_INSTANCE_MUTEX L"Local\\TrayMeter.SingleInstance"
#define SETTINGS_KEY L"Software\\TrayMeter"
#define RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define UTILITY_COUNTER L"\\Processor Information(_Total)\\% Processor Utility"

#define WM_TRAY_CALLBACK (WM_APP + 1)
#define TICK_TIMER_ID 1
#define TRAY_ICON_ID 1

// Default for "Match Task Manager's CPU %". See README.md for the measured cost of each mode.
#define DEFAULT_MATCH_TASK_MANAGER 1

// The CPU bar uses Task Manager's tray blue at every load.
#define CPU_COLOR RGB(87, 192, 255)

// Memory fill colour by load: green below WARN_PERCENT, yellow from WARN_PERCENT, red from HIGH_PERCENT.
#define WARN_PERCENT 70
#define HIGH_PERCENT 90
#define OK_COLOR RGB(76, 200, 80)
#define WARN_COLOR RGB(240, 200, 40)
#define HIGH_COLOR RGB(232, 64, 64)

// Task Manager's gauge: a sunken border (shadow on the top and left, light on the right and bottom) around a dotted grid.
#define BORDER_SHADOW_COLOR RGB(128, 128, 128)
#define BORDER_LIGHT_COLOR RGB(255, 255, 255)
#define GRID_COLOR RGB(72, 73, 75)
#define GRID_DOT_COLOR RGB(31, 33, 35)

// How long after the pointer last moved over the icon its tooltip keeps being refreshed every tick.
#define HOVER_REFRESH_MS 5000

enum Command
{
    CMD_TASK_MANAGER = 100,
    CMD_MATCH_TASK_MANAGER,
    CMD_INTERVAL_1,
    CMD_INTERVAL_2,
    CMD_INTERVAL_5,
    CMD_START_WITH_WINDOWS,
    CMD_EXIT
};

static HWND g_window;
static UINT g_taskbarCreatedMessage;
static ULONGLONG g_hoverUntil;

// What Explorer currently shows: the icon, the key of the gauges drawn on it, and the tooltip.
static BOOL g_trayAdded;
static HICON g_trayIcon;
static int g_drawnKey = -1;
static int g_tipKey = -1;
static wchar_t g_tip[128];

// Gauge drawing resources, rebuilt when the taskbar's icon size changes.
static int g_iconSize;
static HBITMAP g_dib;
static HBITMAP g_mask;
static DWORD* g_pixels;
static RECT g_cpuBox;
static RECT g_memoryBox;
static int g_unit;

// Settings and sampling state.
static BOOL g_matchTaskManager;
static UINT g_intervalMs = 1000;
static BOOL g_paused;
static ULONGLONG g_lastIdleTime;
static ULONGLONG g_lastTotalTime;
static PDH_HQUERY g_utilityQuery;
static PDH_HCOUNTER g_utilityCounter;
static int g_cpuPercent;
static int g_memoryPercent;
static int g_memoryUsedTenthsGb;
static int g_memoryTotalTenthsGb;

// ---------------------------------------------------------------------------------------------------------------------
// Settings

static DWORD ReadSetting(const wchar_t* name, DWORD fallback)
{
    DWORD value = fallback;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, SETTINGS_KEY, name, RRF_RT_REG_DWORD, NULL, &value, &size) != ERROR_SUCCESS)
    {
        return fallback;
    }
    return value;
}

static void WriteSetting(const wchar_t* name, DWORD value)
{
    RegSetKeyValueW(HKEY_CURRENT_USER, SETTINGS_KEY, name, REG_DWORD, &value, sizeof(value));
}

static void GetQuotedExePath(wchar_t* buffer, size_t count)
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    swprintf(buffer, count, L"\"%s\"", exe);
}

static BOOL IsStartWithWindowsEnabled(void)
{
    wchar_t expected[MAX_PATH + 3];
    wchar_t actual[MAX_PATH + 3];
    DWORD size = sizeof(actual);
    GetQuotedExePath(expected, ARRAYSIZE(expected));
    if (RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, APP_NAME, RRF_RT_REG_SZ, NULL, actual, &size) != ERROR_SUCCESS)
    {
        return FALSE;
    }
    return _wcsicmp(actual, expected) == 0;
}

// A TrayMeter shortcut in the all-users Startup folder starts it for everyone; the per-user menu item then only reports that.
static BOOL IsInstalledForAllUsers(void)
{
    PWSTR folder = NULL;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_CommonStartup, 0, NULL, &folder)))
    {
        return FALSE;
    }
    wchar_t shortcut[MAX_PATH];
    swprintf(shortcut, ARRAYSIZE(shortcut), L"%s\\%s.lnk", folder, APP_NAME);
    CoTaskMemFree(folder);
    return GetFileAttributesW(shortcut) != INVALID_FILE_ATTRIBUTES;
}

static void SetStartWithWindows(BOOL enable)
{
    if (!enable)
    {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, RUN_KEY, APP_NAME);
        return;
    }
    wchar_t command[MAX_PATH + 3];
    GetQuotedExePath(command, ARRAYSIZE(command));
    DWORD bytes = (DWORD)((wcslen(command) + 1) * sizeof(wchar_t));
    RegSetKeyValueW(HKEY_CURRENT_USER, RUN_KEY, APP_NAME, REG_SZ, command, bytes);
}

static void LoadSettings(void)
{
    g_matchTaskManager = ReadSetting(L"MatchTaskManager", DEFAULT_MATCH_TASK_MANAGER) != 0;
    DWORD interval = ReadSetting(L"IntervalMs", 1000);
    g_intervalMs = (interval == 2000 || interval == 5000) ? interval : 1000;
}

// ---------------------------------------------------------------------------------------------------------------------
// Sampling

static ULONGLONG FileTimeToTicks(FILETIME time)
{
    return ((ULONGLONG)time.dwHighDateTime << 32) | time.dwLowDateTime;
}

// Busy share of all processor time since the previous call. Kernel time includes idle time.
static int SampleCpuTime(void)
{
    FILETIME idleTime;
    FILETIME kernelTime;
    FILETIME userTime;
    if (!GetSystemTimes(&idleTime, &kernelTime, &userTime))
    {
        return g_cpuPercent;
    }
    ULONGLONG idle = FileTimeToTicks(idleTime);
    ULONGLONG total = FileTimeToTicks(kernelTime) + FileTimeToTicks(userTime);
    ULONGLONG totalDelta = total - g_lastTotalTime;
    ULONGLONG idleDelta = idle - g_lastIdleTime;
    g_lastIdleTime = idle;
    g_lastTotalTime = total;
    if (totalDelta == 0 || idleDelta > totalDelta)
    {
        return g_cpuPercent;
    }
    return (int)(((totalDelta - idleDelta) * 100 + totalDelta / 2) / totalDelta);
}

// pdh.dll is delay-loaded, so it is only mapped into the process once this mode is used.
static BOOL OpenUtilityQuery(void)
{
    if (g_utilityQuery)
    {
        return TRUE;
    }
    PDH_HQUERY query;
    if (PdhOpenQueryW(NULL, 0, &query) != ERROR_SUCCESS)
    {
        return FALSE;
    }
    if (PdhAddEnglishCounterW(query, UTILITY_COUNTER, 0, &g_utilityCounter) != ERROR_SUCCESS)
    {
        PdhCloseQuery(query);
        return FALSE;
    }
    PdhCollectQueryData(query);
    g_utilityQuery = query;
    return TRUE;
}

static void CloseUtilityQuery(void)
{
    if (g_utilityQuery)
    {
        PdhCloseQuery(g_utilityQuery);
        g_utilityQuery = NULL;
    }
}

// PDH caps formatted values at 100 unless PDH_FMT_NOCAP100 is passed, as Task Manager's display does.
static int SampleCpuUtility(void)
{
    PDH_FMT_COUNTERVALUE value;
    if (PdhCollectQueryData(g_utilityQuery) != ERROR_SUCCESS)
    {
        return g_cpuPercent;
    }
    if (PdhGetFormattedCounterValue(g_utilityCounter, PDH_FMT_DOUBLE, NULL, &value) != ERROR_SUCCESS)
    {
        return g_cpuPercent;
    }
    if (value.CStatus != PDH_CSTATUS_VALID_DATA && value.CStatus != PDH_CSTATUS_NEW_DATA)
    {
        return g_cpuPercent;
    }
    double percent = value.doubleValue;
    if (percent < 0.0)
    {
        percent = 0.0;
    }
    if (percent > 100.0)
    {
        percent = 100.0;
    }
    return (int)(percent + 0.5);
}

static int SampleCpu(void)
{
    if (g_utilityQuery)
    {
        return SampleCpuUtility();
    }
    return SampleCpuTime();
}

static void ResetCpuBaseline(void)
{
    if (g_utilityQuery)
    {
        PdhCollectQueryData(g_utilityQuery);
        return;
    }
    SampleCpuTime();
}

static int ToTenthsOfGb(ULONGLONG bytes)
{
    return (int)((bytes * 10 + (1ull << 29)) >> 30);
}

static void SampleMemory(void)
{
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status) || status.ullTotalPhys == 0)
    {
        return;
    }
    ULONGLONG used = status.ullTotalPhys - status.ullAvailPhys;
    g_memoryPercent = (int)((used * 100 + status.ullTotalPhys / 2) / status.ullTotalPhys);
    g_memoryUsedTenthsGb = ToTenthsOfGb(used);
    g_memoryTotalTenthsGb = ToTenthsOfGb(status.ullTotalPhys);
}

// ---------------------------------------------------------------------------------------------------------------------
// Gauge drawing

static void ReleaseGaugeResources(void)
{
    if (g_dib)
    {
        DeleteObject(g_dib);
        g_dib = NULL;
        g_pixels = NULL;
    }
    if (g_mask)
    {
        DeleteObject(g_mask);
        g_mask = NULL;
    }
    g_iconSize = 0;
}

// At 16 px: two 7 x 13 px bars side by side, 1 px in from the left and right and 2 px from the top, like Task Manager's
// 12 x 13 box split in two. Larger icons scale the layout.
static void LayoutGauges(int size)
{
    g_unit = size >= 32 ? size / 16 : 1;
    int left = (size + 8) / 16;
    int right = (size * 15 + 8) / 16;
    int top = (size * 2 + 8) / 16;
    int bottom = (size * 15 + 8) / 16;
    int barWidth = (right - left) / 2;
    SetRect(&g_cpuBox, left, top, left + barWidth, bottom);
    SetRect(&g_memoryBox, left + barWidth, top, left + 2 * barWidth, bottom);
}

static BOOL CreateGaugeResources(int size)
{
    ReleaseGaugeResources();
    BITMAPINFO info;
    ZeroMemory(&info, sizeof(info));
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    g_dib = CreateDIBSection(NULL, &info, DIB_RGB_COLORS, &bits, NULL, 0);
    g_pixels = (DWORD*)bits;

    // An all-zero AND mask: the colour bitmap's alpha decides transparency.
    int maskBytes = ((size + 15) / 16) * 2 * size;
    void* zeros = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, maskBytes);
    g_mask = zeros ? CreateBitmap(size, size, 1, 1, zeros) : NULL;
    if (zeros)
    {
        HeapFree(GetProcessHeap(), 0, zeros);
    }
    if (!g_dib || !g_mask)
    {
        ReleaseGaugeResources();
        return FALSE;
    }
    LayoutGauges(size);
    g_iconSize = size;
    return TRUE;
}

static DWORD ToPixel(COLORREF color)
{
    return 0xFF000000 | ((DWORD)GetRValue(color) << 16) | ((DWORD)GetGValue(color) << 8) | GetBValue(color);
}

static RECT InnerOf(RECT box)
{
    RECT inner = { box.left + g_unit, box.top + g_unit, box.right - g_unit, box.bottom - g_unit };
    return inner;
}

static int FillRows(RECT box, int percent)
{
    RECT inner = InnerOf(box);
    return ((inner.bottom - inner.top) * percent + 50) / 100;
}

static int MemoryLoadLevel(int percent)
{
    if (percent >= HIGH_PERCENT)
    {
        return 2;
    }
    if (percent >= WARN_PERCENT)
    {
        return 1;
    }
    return 0;
}

// Equal keys draw identical pixels, so Explorer is only called when the key changes.
static int GaugeKey(int cpuPercent, int memoryPercent)
{
    return FillRows(g_cpuBox, cpuPercent) * 1000 + FillRows(g_memoryBox, memoryPercent) * 4 + MemoryLoadLevel(memoryPercent);
}

static void DrawBar(RECT box, int percent, DWORD fill)
{
    RECT inner = InnerOf(box);
    int fillTop = inner.bottom - FillRows(box, percent);
    DWORD shadow = ToPixel(BORDER_SHADOW_COLOR);
    DWORD light = ToPixel(BORDER_LIGHT_COLOR);
    DWORD grid = ToPixel(GRID_COLOR);
    DWORD dot = ToPixel(GRID_DOT_COLOR);
    for (int y = box.top; y < box.bottom; y++)
    {
        DWORD* row = g_pixels + y * g_iconSize;
        for (int x = box.left; x < box.right; x++)
        {
            DWORD color;
            if (x >= inner.right)
            {
                color = light;
            }
            else if (x < inner.left || y < inner.top)
            {
                color = shadow;
            }
            else if (y >= inner.bottom)
            {
                color = light;
            }
            else if (y >= fillTop)
            {
                color = fill;
            }
            else
            {
                BOOL isDot = ((x - inner.left) / g_unit) % 2 == 0 && ((y - inner.top) / g_unit) % 2 == 0;
                color = isDot ? dot : grid;
            }
            row[x] = color;
        }
    }
}

static HICON RenderGauges(int cpuPercent, int memoryPercent)
{
    static const COLORREF memoryColors[3] = { OK_COLOR, WARN_COLOR, HIGH_COLOR };
    if (!g_pixels || g_iconSize <= 0)
    {
        return NULL;
    }
    ZeroMemory(g_pixels, (SIZE_T)g_iconSize * g_iconSize * sizeof(DWORD));
    DrawBar(g_cpuBox, cpuPercent, ToPixel(CPU_COLOR));
    DrawBar(g_memoryBox, memoryPercent, ToPixel(memoryColors[MemoryLoadLevel(memoryPercent)]));
    ICONINFO iconInfo = { TRUE, 0, 0, g_mask, g_dib };
    return CreateIconIndirect(&iconInfo);
}

static int QueryTaskbarIconSize(void)
{
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", NULL);
    UINT dpi = taskbar ? GetDpiForWindow(taskbar) : 0;
    if (dpi == 0)
    {
        dpi = GetDpiForSystem();
    }
    return GetSystemMetricsForDpi(SM_CXSMICON, dpi);
}

// Returns TRUE when the size changed, after which the icon is redrawn on the next Present.
static BOOL EnsureIconSize(void)
{
    int size = QueryTaskbarIconSize();
    if (size == g_iconSize && g_pixels)
    {
        return FALSE;
    }
    CreateGaugeResources(size);
    g_drawnKey = -1;
    return TRUE;
}

// ---------------------------------------------------------------------------------------------------------------------
// Tray icon

static void InitNotifyData(NOTIFYICONDATAW* data, UINT flags)
{
    ZeroMemory(data, sizeof(*data));
    data->cbSize = sizeof(*data);
    data->hWnd = g_window;
    data->uID = TRAY_ICON_ID;
    data->uFlags = flags;
    data->uCallbackMessage = WM_TRAY_CALLBACK;
    data->hIcon = g_trayIcon;
    wcscpy_s(data->szTip, ARRAYSIZE(data->szTip), g_tip);
}

static void AddTrayIcon(void)
{
    NOTIFYICONDATAW data;
    InitNotifyData(&data, NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP);
    // NIM_ADD fails when the icon already exists, for example when an earlier call timed out but still went through.
    g_trayAdded = Shell_NotifyIconW(NIM_ADD, &data) || Shell_NotifyIconW(NIM_MODIFY, &data);
    if (g_trayAdded)
    {
        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
    }
}

static void RemoveTrayIcon(void)
{
    if (g_trayAdded)
    {
        NOTIFYICONDATAW data;
        InitNotifyData(&data, 0);
        Shell_NotifyIconW(NIM_DELETE, &data);
        g_trayAdded = FALSE;
    }
    if (g_trayIcon)
    {
        DestroyIcon(g_trayIcon);
        g_trayIcon = NULL;
    }
    g_drawnKey = -1;
    g_tipKey = -1;
}

static void FormatTip(void)
{
    swprintf(g_tip, ARRAYSIZE(g_tip), L"CPU %d%%\nMemory %d%% (%d.%d of %d.%d GB)", g_cpuPercent, g_memoryPercent,
        g_memoryUsedTenthsGb / 10, g_memoryUsedTenthsGb % 10, g_memoryTotalTenthsGb / 10, g_memoryTotalTenthsGb % 10);
}

// Sends Explorer only what changed. A failed call marks the icon as missing, and the next tick adds it again.
// Tooltip text goes along with an icon change, or every tick while the pointer is over the icon; otherwise a number
// that moved by 1 % would cost a cross-process call each second for a tooltip nobody is looking at.
static void Present(BOOL refreshTip)
{
    int key = GaugeKey(g_cpuPercent, g_memoryPercent);
    int tipKey = g_cpuPercent * 10000000 + g_memoryPercent * 100000 + g_memoryUsedTenthsGb;
    BOOL iconChanged = key != g_drawnKey || !g_trayIcon;
    BOOL tipChanged = tipKey != g_tipKey && (iconChanged || refreshTip || !g_trayAdded);
    if (g_trayAdded && !iconChanged && !tipChanged)
    {
        return;
    }

    HICON oldIcon = NULL;
    if (iconChanged)
    {
        HICON icon = RenderGauges(g_cpuPercent, g_memoryPercent);
        if (!icon)
        {
            return;
        }
        oldIcon = g_trayIcon;
        g_trayIcon = icon;
        g_drawnKey = key;
    }
    if (tipChanged)
    {
        FormatTip();
        g_tipKey = tipKey;
    }

    if (g_trayAdded)
    {
        NOTIFYICONDATAW data;
        InitNotifyData(&data, NIF_TIP | NIF_SHOWTIP | (iconChanged ? NIF_ICON : 0));
        g_trayAdded = Shell_NotifyIconW(NIM_MODIFY, &data);
    }
    if (!g_trayAdded)
    {
        AddTrayIcon();
    }
    if (oldIcon)
    {
        DestroyIcon(oldIcon);
    }
}

static BOOL IsPointerOverIcon(void)
{
    return GetTickCount64() < g_hoverUntil;
}

// ---------------------------------------------------------------------------------------------------------------------
// Ticking

static void StartTicking(void)
{
    // Coalescing lets Windows batch this wake-up with others; 10 % slack is invisible on a meter.
    if (!SetCoalescableTimer(g_window, TICK_TIMER_ID, g_intervalMs, NULL, g_intervalMs / 10))
    {
        SetTimer(g_window, TICK_TIMER_ID, g_intervalMs, NULL);
    }
}

static void Tick(void)
{
    g_cpuPercent = SampleCpu();
    SampleMemory();
    Present(IsPointerOverIcon());
}

// Nobody can see the tray while the session is locked, so stop sampling until it unlocks.
static void PauseTicking(void)
{
    if (g_paused)
    {
        return;
    }
    g_paused = TRUE;
    KillTimer(g_window, TICK_TIMER_ID);
}

static void ResumeTicking(void)
{
    if (!g_paused)
    {
        return;
    }
    g_paused = FALSE;
    ResetCpuBaseline();
    StartTicking();
}

// ---------------------------------------------------------------------------------------------------------------------
// Menu and commands

static void OpenTaskManager(void)
{
    ShellExecuteW(NULL, L"open", L"taskmgr.exe", NULL, NULL, SW_SHOWNORMAL);
}

static void SetMatchTaskManager(BOOL match)
{
    if (match && !OpenUtilityQuery())
    {
        MessageBoxW(g_window, L"The processor utility counter is not available on this PC.", APP_NAME, MB_OK | MB_ICONWARNING);
        return;
    }
    if (!match)
    {
        CloseUtilityQuery();
        SampleCpuTime();
    }
    g_matchTaskManager = match;
    WriteSetting(L"MatchTaskManager", match ? 1 : 0);
}

static void SetInterval(UINT intervalMs)
{
    g_intervalMs = intervalMs;
    WriteSetting(L"IntervalMs", intervalMs);
    if (!g_paused)
    {
        StartTicking();
    }
}

static void OnCommand(UINT command)
{
    switch (command)
    {
    case CMD_TASK_MANAGER:
        OpenTaskManager();
        break;
    case CMD_MATCH_TASK_MANAGER:
        SetMatchTaskManager(!g_matchTaskManager);
        break;
    case CMD_INTERVAL_1:
        SetInterval(1000);
        break;
    case CMD_INTERVAL_2:
        SetInterval(2000);
        break;
    case CMD_INTERVAL_5:
        SetInterval(5000);
        break;
    case CMD_START_WITH_WINDOWS:
        SetStartWithWindows(!IsStartWithWindowsEnabled());
        break;
    case CMD_EXIT:
        DestroyWindow(g_window);
        break;
    }
}

static UINT CheckedIf(BOOL condition)
{
    return condition ? MF_CHECKED : MF_UNCHECKED;
}

static void ShowMenu(int x, int y)
{
    HMENU intervals = CreatePopupMenu();
    AppendMenuW(intervals, MF_STRING, CMD_INTERVAL_1, L"1 second");
    AppendMenuW(intervals, MF_STRING, CMD_INTERVAL_2, L"2 seconds");
    AppendMenuW(intervals, MF_STRING, CMD_INTERVAL_5, L"5 seconds");
    UINT selected = g_intervalMs == 5000 ? CMD_INTERVAL_5 : (g_intervalMs == 2000 ? CMD_INTERVAL_2 : CMD_INTERVAL_1);
    CheckMenuRadioItem(intervals, CMD_INTERVAL_1, CMD_INTERVAL_5, selected, MF_BYCOMMAND);

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, CMD_TASK_MANAGER, L"Open Task Manager");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | CheckedIf(g_matchTaskManager), CMD_MATCH_TASK_MANAGER, L"Match Task Manager's CPU %");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)intervals, L"Update every");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    if (IsInstalledForAllUsers())
    {
        AppendMenuW(menu, MF_STRING | MF_CHECKED | MF_GRAYED, CMD_START_WITH_WINDOWS, L"Start with Windows (all users)");
    }
    else
    {
        AppendMenuW(menu, MF_STRING | CheckedIf(IsStartWithWindowsEnabled()), CMD_START_WITH_WINDOWS, L"Start with Windows");
    }
    AppendMenuW(menu, MF_STRING, CMD_EXIT, L"Exit");

    // The foreground switch and the WM_NULL afterwards make the menu close when the user clicks elsewhere.
    SetForegroundWindow(g_window);
    UINT align = GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    UINT command = (UINT)TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | align, x, y,
        g_window, NULL);
    PostMessageW(g_window, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (command != 0)
    {
        OnCommand(command);
    }
}

// NOTIFYICON_VERSION_4 layout: the event is in LOWORD(lParam) and the anchor point in wParam.
static void OnTrayCallback(WPARAM wParam, LPARAM lParam)
{
    switch (LOWORD(lParam))
    {
    case WM_MOUSEMOVE:
        g_hoverUntil = GetTickCount64() + HOVER_REFRESH_MS;
        Present(TRUE);
        break;
    case WM_CONTEXTMENU:
        ShowMenu(GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam));
        break;
    }
}

// Explorer restarted: its tray starts empty, so add the icon again at the taskbar's current size.
static void OnTaskbarCreated(void)
{
    g_trayAdded = FALSE;
    EnsureIconSize();
    Present(FALSE);
}

static void Shutdown(void)
{
    KillTimer(g_window, TICK_TIMER_ID);
    WTSUnRegisterSessionNotification(g_window);
    RemoveTrayIcon();
    CloseUtilityQuery();
    ReleaseGaugeResources();
}

static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == g_taskbarCreatedMessage && message != 0)
    {
        OnTaskbarCreated();
        return 0;
    }
    switch (message)
    {
    case WM_TIMER:
        if (wParam == TICK_TIMER_ID)
        {
            Tick();
        }
        return 0;
    case WM_TRAY_CALLBACK:
        OnTrayCallback(wParam, lParam);
        return 0;
    case WM_WTSSESSION_CHANGE:
        if (wParam == WTS_SESSION_LOCK)
        {
            PauseTicking();
        }
        else if (wParam == WTS_SESSION_UNLOCK)
        {
            ResumeTicking();
        }
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
    case WM_DPICHANGED:
        if (EnsureIconSize())
        {
            Present(FALSE);
        }
        return 0;
    case WM_DESTROY:
        Shutdown();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

#ifndef TRAYMETER_NO_MAIN
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previousInstance, PWSTR commandLine, int showCommand)
{
    UNREFERENCED_PARAMETER(previousInstance);
    UNREFERENCED_PARAMETER(commandLine);
    UNREFERENCED_PARAMETER(showCommand);

    HANDLE mutex = CreateMutexW(NULL, FALSE, SINGLE_INSTANCE_MUTEX);
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    LoadSettings();

    g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSW windowClass;
    ZeroMemory(&windowClass, sizeof(windowClass));
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = WINDOW_CLASS;
    RegisterClassW(&windowClass);
    // A hidden top-level window, not a message-only one: TaskbarCreated is broadcast to top-level windows only.
    g_window = CreateWindowExW(WS_EX_TOOLWINDOW, WINDOW_CLASS, APP_NAME, WS_POPUP, 0, 0, 0, 0, NULL, NULL, instance, NULL);
    if (!g_window)
    {
        return 1;
    }
    ChangeWindowMessageFilterEx(g_window, g_taskbarCreatedMessage, MSGFLT_ALLOW, NULL);
    WTSRegisterSessionNotification(g_window, NOTIFY_FOR_THIS_SESSION);

    if (g_matchTaskManager && !OpenUtilityQuery())
    {
        g_matchTaskManager = FALSE;
    }
    SampleCpuTime();
    SampleMemory();
    EnsureIconSize();
    Present(FALSE);
    StartTicking();

    MSG message;
    while (GetMessageW(&message, NULL, 0, 0) > 0)
    {
        DispatchMessageW(&message);
    }
    CloseHandle(mutex);
    return 0;
}
#endif
