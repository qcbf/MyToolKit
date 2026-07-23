/*
 * MyToolKit.exe
 * Lightweight Win32 toolkit. Features:
 *   1. CapsRemap  — CapsLock -> Ctrl+Space
 *   2. EscClose   — Hold ESC 1.0s -> Alt+F4, with on-screen countdown overlay
 *                   (200ms activation delay; short taps pass through unchanged)
 *   3. WinBacktick — Win+` cycles windows of the same app (like macOS Cmd+`)
 *
 * Build (MSVC):    build_msvc.bat
 * Build (MinGW):   windres resource.rc -o resource.o
 *                  gcc -O2 -s -mwindows mytoolkit.c resource.o -o mytoolkit.exe
 *                       -luser32 -lshell32 -ladvapi32 -lgdi32 -ldwmapi -lpsapi
 */

#ifdef _MSC_VER
#   pragma comment(lib, "user32.lib")
#   pragma comment(lib, "shell32.lib")
#   pragma comment(lib, "advapi32.lib")
#   pragma comment(lib, "gdi32.lib")
#   pragma comment(lib, "dwmapi.lib")
#   pragma comment(lib, "psapi.lib")
#   pragma comment(linker, "/SUBSYSTEM:WINDOWS")
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <psapi.h>
#include <stdlib.h>

/* ── Message / timer IDs ── */
#define IDI_APP              1

#define WM_TRAY_MSG          (WM_APP + 1)
#define WM_ESC_DOWN          (WM_APP + 2)
#define WM_ESC_UP            (WM_APP + 3)
#define WM_CAPS_DOWN         (WM_APP + 4)
#define WM_CAPS_UP           (WM_APP + 5)

#define TIMER_ESC_DELAY      1    /* 200ms one-shot: ignores short taps        */
#define TIMER_ESC_TICK       2    /* 30ms recurring: drives progress animation */
#define TIMER_CAPS           3    /* one-shot: CapsLock long-press threshold   */
#define TIMER_HOTKEY_RETRY   4    /* recurring: recover Win+` registration     */

#define HOTKEY_WIN_BACKTICK  1    /* RegisterHotKey ID for Win+`               */

#define ESC_DELAY_MS         200  /* ms before overlay activates               */
#define ESC_HOLD_MS          1000 /* ms of hold required to fire Alt+F4        */
#define ESC_TICK_MS          30   /* repaint interval (ms)                     */
#define CAPS_HOLD_MS         400  /* ms to distinguish tap vs. long-press      */
#define HOTKEY_RETRY_MS      5000 /* retry registration after a conflict        */

/* ── Overlay geometry ── */
#define OV_W                 260
#define OV_H                 44
#define OV_PAD               14   /* horizontal margin */
#define OV_BAR_Y             32   /* bar top y         */
#define OV_BAR_H             6    /* bar height        */

/* ── Tray menu IDs ── */
#define ID_MENU_STARTUP      100
#define ID_MENU_EXIT         101
#define ID_MENU_ABOUT        102

#define TASK_NAME            L"MyToolKit"
#define MUTEX_NAME           L"Global\\MyToolKit_SingleInstance"
#define WND_CLASS_MAIN       L"MyToolKit_TrayWnd"
#define WND_CLASS_OVERLAY    L"MyToolKit_OverlayWnd"

/* ── Globals ── */
static HHOOK           s_kbHook;
static HINSTANCE       s_hInst;
static NOTIFYICONDATAW s_nid;
static HWND            s_hwndMain;
static HWND            s_hwndOverlay;
static BOOL            s_escDown;      /* ESC physically held      */
static BOOL            s_escActive;    /* countdown phase running  */
static int             s_escElapsedMs; /* ms elapsed in countdown  */
static BOOL            s_capsDown;     /* CapsLock physically held */
static BOOL            s_winBacktickRegistered;
static UINT            s_wmTaskbarCreated; /* "TaskbarCreated" msg ID */


/* ════════════════════════════════════════════════════════════════
 *  Feature 1: CapsRemap  (CapsLock -> Ctrl+Space)
 *  Feature 2: EscClose   (ESC held -> Alt+F4)
 * ════════════════════════════════════════════════════════════════ */
static LRESULT CALLBACK OnKeyboardEvent(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION)
    {
        const KBDLLHOOKSTRUCT* kb = (const KBDLLHOOKSTRUCT*)lParam;

        if (!(kb->flags & LLKHF_INJECTED))
        {
            /* ── CapsRemap / CapsToggle ──
             *   Tap  (<400ms): Ctrl+Space  (input method switch)
             *   Hold (>=400ms): real CapsLock toggle (macOS style)
             *   All non-injected CapsLock events are suppressed here;
             *   the actual actions are fired from WndProc timers.     */
            if (kb->vkCode == VK_CAPITAL)
            {
                if ((wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) && !s_capsDown)
                {
                    s_capsDown = TRUE;
                    PostMessageW(s_hwndMain, WM_CAPS_DOWN, 0, 0);
                }
                else if ((wParam == WM_KEYUP || wParam == WM_SYSKEYUP) && s_capsDown)
                {
                    s_capsDown = FALSE;
                    PostMessageW(s_hwndMain, WM_CAPS_UP, 0, 0);
                }
                return 1; /* always suppress physical CapsLock */
            }

            /* ── EscClose: track first down / up only ── */
            if (kb->vkCode == VK_ESCAPE)
            {
                if ((wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) && !s_escDown)
                {
                    s_escDown = TRUE;
                    PostMessageW(s_hwndMain, WM_ESC_DOWN, 0, 0);
                }
                else if ((wParam == WM_KEYUP || wParam == WM_SYSKEYUP) && s_escDown)
                {
                    s_escDown = FALSE;
                    PostMessageW(s_hwndMain, WM_ESC_UP, 0, 0);
                }
                /* ESC always passes through so apps receive it normally. */
            }
        }
    }
    return CallNextHookEx(s_kbHook, code, wParam, lParam);
}


/* ════════════════════════════════════════════════════════════════
 *  ESC countdown overlay (subtle, low-profile)
 * ════════════════════════════════════════════════════════════════ */
static BYTE LerpByte(BYTE a, BYTE b, float t)
{
    return (BYTE)(a + (int)((b - a) * t));
}

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        /* Double-buffer */
        HDC     memDC  = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, OV_W, OV_H);
        if (!memDC || !memBmp)
        {
            if (memBmp) DeleteObject(memBmp);
            if (memDC) DeleteDC(memDC);
            EndPaint(hwnd, &ps);
            return 0;
        }
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

        /* Background */
        RECT rc = { 0, 0, OV_W, OV_H };
        HBRUSH bgBrush = CreateSolidBrush(RGB(36, 36, 36));
        FillRect(memDC, &rc, bgBrush);
        DeleteObject(bgBrush);

        /* Bar track */
        RECT trackRc = { OV_PAD, OV_BAR_Y, OV_W - OV_PAD, OV_BAR_Y + OV_BAR_H };
        HBRUSH trackBrush = CreateSolidBrush(RGB(62, 62, 62));
        FillRect(memDC, &trackRc, trackBrush);
        DeleteObject(trackBrush);

        /* Bar fill — muted teal -> amber */
        float progress = (float)s_escElapsedMs / (float)ESC_HOLD_MS;
        if (progress < 0.0f) progress = 0.0f;
        if (progress > 1.0f) progress = 1.0f;

        int fillW = (int)((OV_W - OV_PAD * 2) * progress);
        if (fillW > 0)
        {
            BYTE r = LerpByte(70,  200, progress);
            BYTE g = LerpByte(150, 70,  progress);
            BYTE b = LerpByte(120, 50,  progress);
            HBRUSH fillBrush = CreateSolidBrush(RGB(r, g, b));
            RECT fillRc = { OV_PAD, OV_BAR_Y, OV_PAD + fillW, OV_BAR_Y + OV_BAR_H };
            FillRect(memDC, &fillRc, fillBrush);
            DeleteObject(fillBrush);
        }

        /* Text */
        int remainSec = (ESC_HOLD_MS - s_escElapsedMs + 999) / 1000;
        if (remainSec < 1) remainSec = 1;
        WCHAR text[32];
        wsprintfW(text, L"Hold ESC · %ds", remainSec);

        HFONT hFont = CreateFontW(
            14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HFONT oldFont = (HFONT)SelectObject(memDC, hFont);

        SetBkMode(memDC, TRANSPARENT);
        SetTextColor(memDC, RGB(170, 170, 170));
        RECT textRc = { OV_PAD, 8, OV_W - OV_PAD, OV_BAR_Y - 2 };
        DrawTextW(memDC, text, -1, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        SelectObject(memDC, oldFont);
        DeleteObject(hFont);

        BitBlt(hdc, 0, 0, OV_W, OV_H, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);

        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ShowEscOverlay(void)
{
    /* Center on the monitor that contains the current foreground window. */
    HWND hFg = GetForegroundWindow();
    HMONITOR hMon = MonitorFromWindow(hFg ? hFg : s_hwndMain, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfo(hMon, &mi)) return;
    int x = mi.rcMonitor.left + (mi.rcMonitor.right  - mi.rcMonitor.left - OV_W) / 2;
    int y = mi.rcMonitor.top  + (mi.rcMonitor.bottom - mi.rcMonitor.top  - OV_H) / 2;

    s_hwndOverlay = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        WND_CLASS_OVERLAY, NULL, WS_POPUP,
        x, y, OV_W, OV_H,
        NULL, NULL, s_hInst, NULL);

    if (s_hwndOverlay)
    {
        SetLayeredWindowAttributes(s_hwndOverlay, 0, 148, LWA_ALPHA); /* ~58% opacity */
        ShowWindow(s_hwndOverlay, SW_SHOWNOACTIVATE);
        UpdateWindow(s_hwndOverlay);
    }
}

static void HideEscOverlay(void)
{
    if (s_hwndOverlay)
    {
        DestroyWindow(s_hwndOverlay);
        s_hwndOverlay = NULL;
    }
}


/* ════════════════════════════════════════════════════════════════
 *  Elevation helpers
 * ════════════════════════════════════════════════════════════════ */
static BOOL IsRunningElevated(void)
{
    BOOL elevated = FALSE;
    HANDLE token  = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    {
        TOKEN_ELEVATION elev;
        DWORD size = sizeof(elev);
        if (GetTokenInformation(token, TokenElevation, &elev, size, &size))
            elevated = elev.TokenIsElevated;
        CloseHandle(token);
    }
    return elevated;
}

static void EnsureElevated(void)
{
    WCHAR exePath[MAX_PATH];
    SHELLEXECUTEINFOW sei;
    if (IsRunningElevated()) return;
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = exePath;
    sei.nShow  = SW_HIDE;
    ShellExecuteExW(&sei);
    ExitProcess(0);
}


/* ════════════════════════════════════════════════════════════════
 *  Single-instance guard
 * ════════════════════════════════════════════════════════════════ */
static BOOL IsAlreadyRunning(void)
{
    static HANDLE s_hMutex = NULL;
    s_hMutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        MessageBoxW(NULL,
            L"MyToolKit 已在运行中。\n\n"
            L"请勿重复启动，程序会在后台持续工作。",
            TASK_NAME, MB_OK | MB_ICONWARNING);
        return TRUE;
    }
    return FALSE;
}


/* ════════════════════════════════════════════════════════════════
 *  Auto-start (Task Scheduler)
 * ════════════════════════════════════════════════════════════════ */
static DWORD RunCmd(LPCWSTR cmdLine)
{
    WCHAR buf[1024];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD exitCode = 1;

    wsprintfW(buf, L"/c %s", cmdLine);
    ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (CreateProcessW(L"C:\\Windows\\System32\\cmd.exe", buf,
                       NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
    {
        WaitForSingleObject(pi.hProcess, 10000);
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    return exitCode;
}

static BOOL IsStartupEnabled(void)
{
    WCHAR cmd[256];
    wsprintfW(cmd, L"schtasks /query /tn \"%s\" >nul 2>&1", TASK_NAME);
    return RunCmd(cmd) == 0;
}

static void ToggleStartup(void)
{
    if (IsStartupEnabled())
    {
        WCHAR cmd[256];
        wsprintfW(cmd, L"schtasks /delete /tn \"%s\" /f", TASK_NAME);
        if (RunCmd(cmd) != 0)
            MessageBoxW(NULL, L"移除开机启动失败。", TASK_NAME, MB_OK | MB_ICONERROR);
    }
    else
    {
        WCHAR exePath[MAX_PATH];
        WCHAR cmd[1024];
        GetModuleFileNameW(NULL, exePath, MAX_PATH);
        wsprintfW(cmd,
            L"schtasks /create /tn \"%s\" /tr \"\\\"%s\\\"\" /sc ONLOGON /rl HIGHEST /f",
            TASK_NAME, exePath);
        if (RunCmd(cmd) != 0)
            MessageBoxW(NULL, L"添加开机启动失败。\n请确保以管理员权限运行。",
                        TASK_NAME, MB_OK | MB_ICONERROR);
    }
}


/* ════════════════════════════════════════════════════════════════
 *  Feature 3: WinBacktick — Win+` cycles windows of the same app.
 *
 *  Design rule: RegisterHotKey is the only owner of Win+`. The low-level
 *  keyboard hook must not suppress any part of this chord. Suppressing ` or Win
 *  forces us to fake key-up/key-down state with SendInput, which races with the
 *  shell and causes Start menu popups, stuck logical Win, or missed switches.
 *
 *  "Same app" is matched by executable path instead of PID so multi-process
 *  apps such as Chrome, VS Code, Windows Terminal, and Electron apps still cycle
 *  together.
 * ════════════════════════════════════════════════════════════════ */
typedef struct {
    WCHAR  exePath[MAX_PATH];
    HWND*  list;
    int    count;
    int    capacity;
    BOOL   failed;
} EnumSameAppCtx;

static HWND GetRootAppWindow(HWND hwnd)
{
    HWND root = hwnd ? GetAncestor(hwnd, GA_ROOT) : NULL;
    return root ? root : hwnd;
}

static BOOL GetWindowExePath(HWND hwnd, WCHAR* exePath, DWORD cchExePath)
{
    DWORD pid = 0;
    HANDLE process;
    DWORD size = cchExePath;

    exePath[0] = L'\0';
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return FALSE;

    process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process)
    {
        if (QueryFullProcessImageNameW(process, 0, exePath, &size))
        {
            CloseHandle(process);
            return TRUE;
        }
        CloseHandle(process);
    }

    process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) return FALSE;
    if (!GetModuleFileNameExW(process, NULL, exePath, cchExePath))
    {
        CloseHandle(process);
        exePath[0] = L'\0';
        return FALSE;
    }
    CloseHandle(process);
    return TRUE;
}

static BOOL IsSwitchableWindow(HWND hwnd)
{
    LONG style;
    LONG exStyle;
    HWND owner;
    BOOL cloaked = FALSE;

    if (!IsWindowVisible(hwnd)) return FALSE;
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return FALSE;

    style = GetWindowLongW(hwnd, GWL_STYLE);
    exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (style & WS_CHILD) return FALSE;
    if (exStyle & WS_EX_TOOLWINDOW) return FALSE;
    if (GetWindowTextLengthW(hwnd) == 0) return FALSE;

    owner = GetWindow(hwnd, GW_OWNER);
    if (owner && !(exStyle & WS_EX_APPWINDOW)) return FALSE;

    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
        return FALSE;

    return TRUE;
}

static BOOL CALLBACK EnumSameAppProc(HWND hwnd, LPARAM lParam)
{
    EnumSameAppCtx* ctx = (EnumSameAppCtx*)lParam;
    WCHAR exePath[MAX_PATH];

    if (ctx->failed) return FALSE;
    if (!IsSwitchableWindow(hwnd)) return TRUE;
    if (!GetWindowExePath(hwnd, exePath, MAX_PATH)) return TRUE;
    if (lstrcmpiW(exePath, ctx->exePath) != 0) return TRUE;

    if (ctx->count >= ctx->capacity)
    {
        int newCapacity = ctx->capacity ? ctx->capacity * 2 : 16;
        HWND* newList = (HWND*)realloc(ctx->list, newCapacity * sizeof(HWND));
        if (!newList)
        {
            ctx->failed = TRUE;
            return FALSE;
        }
        ctx->list = newList;
        ctx->capacity = newCapacity;
    }
    ctx->list[ctx->count++] = hwnd;
    return TRUE;
}

static void ActivateWindow(HWND hwnd)
{
    DWORD currentThread = GetCurrentThreadId();
    DWORD targetThread = GetWindowThreadProcessId(hwnd, NULL);
    HWND foreground = GetForegroundWindow();
    DWORD foregroundThread = foreground ? GetWindowThreadProcessId(foreground, NULL) : 0;

    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);

    if (foregroundThread) AttachThreadInput(currentThread, foregroundThread, TRUE);
    if (targetThread) AttachThreadInput(currentThread, targetThread, TRUE);

    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);

    if (targetThread) AttachThreadInput(currentThread, targetThread, FALSE);
    if (foregroundThread) AttachThreadInput(currentThread, foregroundThread, FALSE);
}

static void SwitchToNextAppWindow(void)
{
    HWND fgWnd = GetRootAppWindow(GetForegroundWindow());
    WCHAR exePath[MAX_PATH];
    EnumSameAppCtx ctx;
    int idx = -1;
    HWND target;

    if (!fgWnd) return;
    if (!GetWindowExePath(fgWnd, exePath, MAX_PATH)) return;

    ZeroMemory(&ctx, sizeof(ctx));
    lstrcpynW(ctx.exePath, exePath, MAX_PATH);
    EnumWindows(EnumSameAppProc, (LPARAM)&ctx);

    if (ctx.failed || ctx.count < 2) { free(ctx.list); return; }

    for (int i = 0; i < ctx.count; i++)
    {
        if (ctx.list[i] == fgWnd) { idx = i; break; }
    }

    if (idx < 0) { free(ctx.list); return; }

    target = ctx.list[(idx + 1) % ctx.count];
    ActivateWindow(target);

    free(ctx.list);
}

static void RegisterWinBacktickHotkey(HWND hwnd)
{
    s_winBacktickRegistered = RegisterHotKey(
        hwnd, HOTKEY_WIN_BACKTICK, MOD_WIN | MOD_NOREPEAT, VK_OEM_3);

    if (s_winBacktickRegistered)
        KillTimer(hwnd, TIMER_HOTKEY_RETRY);
    else
        SetTimer(hwnd, TIMER_HOTKEY_RETRY, HOTKEY_RETRY_MS, NULL);
}


/* ════════════════════════════════════════════════════════════════
 *  Tray icon & context menu
 * ════════════════════════════════════════════════════════════════ */
static void ShowTrayMenu(HWND hwnd)
{
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu,
                MF_STRING | (IsStartupEnabled() ? MF_CHECKED : MF_UNCHECKED),
                ID_MENU_STARTUP, L"开机启动");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_MENU_ABOUT, L"About");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_MENU_EXIT, L"退出");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenuEx(hMenu,
                     TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RIGHTALIGN,
                     pt.x, pt.y, hwnd, NULL);
    DestroyMenu(hMenu);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    /* ── Tray ── */
    case WM_TRAY_MSG:
        if (lParam == WM_RBUTTONUP) ShowTrayMenu(hwnd);
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == ID_MENU_STARTUP) ToggleStartup();
        else if (LOWORD(wParam) == ID_MENU_ABOUT)
            ShellExecuteW(NULL, L"open", L"https://github.com/qcbf/MyToolKit", NULL, NULL, SW_SHOWNORMAL);
        else if (LOWORD(wParam) == ID_MENU_EXIT) DestroyWindow(hwnd);
        return 0;

    /* ── Win+` hotkey ── */
    case WM_HOTKEY:
        if (wParam == HOTKEY_WIN_BACKTICK) SwitchToNextAppWindow();
        return 0;

    /* ── CapsRemap/Toggle: CapsLock pressed ──
     *   Start threshold timer. Fire on release or expiry.          */
    case WM_CAPS_DOWN:
        SetTimer(hwnd, TIMER_CAPS, CAPS_HOLD_MS, NULL);
        return 0;

    /* ── CapsLock released ── */
    case WM_CAPS_UP:
        if (KillTimer(hwnd, TIMER_CAPS))
        {
            /* Released before threshold → short tap → Ctrl+Space */
            INPUT seq[4];
            ZeroMemory(seq, sizeof(seq));
            seq[0].type       = INPUT_KEYBOARD; seq[0].ki.wVk     = VK_LCONTROL;
            seq[1].type       = INPUT_KEYBOARD; seq[1].ki.wVk     = VK_SPACE;
            seq[2].type       = INPUT_KEYBOARD; seq[2].ki.wVk     = VK_SPACE;
            seq[2].ki.dwFlags = KEYEVENTF_KEYUP;
            seq[3].type       = INPUT_KEYBOARD; seq[3].ki.wVk     = VK_LCONTROL;
            seq[3].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(4, seq, sizeof(INPUT));
        }
        /* If KillTimer returns FALSE the timer already fired (long press handled there). */
        return 0;

    /* ── EscClose: ESC physically pressed ──
     *   Start 200ms delay timer. If released before it fires,
     *   the keystroke passes through untouched (normal ESC).      */
    case WM_ESC_DOWN:
        SetTimer(hwnd, TIMER_ESC_DELAY, ESC_DELAY_MS, NULL);
        return 0;

    /* ── EscClose: ESC released ── */
    case WM_ESC_UP:
        if (s_escActive)
        {
            /* Cancel active countdown */
            KillTimer(hwnd, TIMER_ESC_TICK);
            s_escActive = FALSE;
            HideEscOverlay();
        }
        else
        {
            /* Released during delay window — just cancel the delay timer */
            KillTimer(hwnd, TIMER_ESC_DELAY);
        }
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_CAPS)
        {
            KillTimer(hwnd, TIMER_CAPS);

            /* Long press threshold reached → toggle real CapsLock.
             * Send injected VK_CAPITAL so it bypasses our hook filter
             * and reaches the system (toggles LED + caps state).      */
            INPUT cap[2];
            ZeroMemory(cap, sizeof(cap));
            cap[0].type   = INPUT_KEYBOARD;
            cap[0].ki.wVk = VK_CAPITAL;
            cap[1].type   = INPUT_KEYBOARD;
            cap[1].ki.wVk = VK_CAPITAL;
            cap[1].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(2, cap, sizeof(INPUT));
        }
        else if (wParam == TIMER_ESC_DELAY)
        {
            /* Delay elapsed and ESC still held — activate countdown */
            KillTimer(hwnd, TIMER_ESC_DELAY);
            if (s_escDown)
            {
                s_escActive    = TRUE;
                s_escElapsedMs = 0;
                ShowEscOverlay();
                SetTimer(hwnd, TIMER_ESC_TICK, ESC_TICK_MS, NULL);
            }
        }
        else if (wParam == TIMER_ESC_TICK)
        {
            s_escElapsedMs += ESC_TICK_MS;

            if (s_escElapsedMs >= ESC_HOLD_MS)
            {
                /* Threshold reached — fire Alt+F4 */
                KillTimer(hwnd, TIMER_ESC_TICK);
                s_escActive = FALSE;
                s_escDown   = FALSE;
                HideEscOverlay();

                INPUT seq[4];
                ZeroMemory(seq, sizeof(seq));
                seq[0].type       = INPUT_KEYBOARD; seq[0].ki.wVk     = VK_MENU;
                seq[1].type       = INPUT_KEYBOARD; seq[1].ki.wVk     = VK_F4;
                seq[2].type       = INPUT_KEYBOARD; seq[2].ki.wVk     = VK_F4;
                seq[2].ki.dwFlags = KEYEVENTF_KEYUP;
                seq[3].type       = INPUT_KEYBOARD; seq[3].ki.wVk     = VK_MENU;
                seq[3].ki.dwFlags = KEYEVENTF_KEYUP;
                SendInput(4, seq, sizeof(INPUT));
            }
            else if (s_hwndOverlay)
            {
                InvalidateRect(s_hwndOverlay, NULL, FALSE);
            }
        }
        else if (wParam == TIMER_HOTKEY_RETRY)
        {
            RegisterWinBacktickHotkey(hwnd);
        }
        return 0;

    case WM_DESTROY:
        UnregisterHotKey(hwnd, HOTKEY_WIN_BACKTICK);
        KillTimer(hwnd, TIMER_CAPS);
        KillTimer(hwnd, TIMER_ESC_DELAY);
        KillTimer(hwnd, TIMER_ESC_TICK);
        KillTimer(hwnd, TIMER_HOTKEY_RETRY);
        HideEscOverlay();
        Shell_NotifyIconW(NIM_DELETE, &s_nid);
        PostQuitMessage(0);
        return 0;

    default:
        /* Explorer 重启后会广播 TaskbarCreated，需要重新添加托盘图标 */
        if (msg == s_wmTaskbarCreated && s_wmTaskbarCreated != 0)
        {
            Shell_NotifyIconW(NIM_ADD, &s_nid);
            return 0;
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}


/* ════════════════════════════════════════════════════════════════
 *  Entry point
 * ════════════════════════════════════════════════════════════════ */
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow)
{
    WNDCLASSEXW wc;
    MSG         msg;

    (void)hPrev; (void)lpCmd; (void)nShow;
    s_hInst = hInst;

    EnsureElevated();
    if (IsAlreadyRunning()) return 1;

    /* ── Main (message-only) window ── */
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = WND_CLASS_MAIN;
    RegisterClassExW(&wc);

    s_hwndMain = CreateWindowExW(0, WND_CLASS_MAIN, NULL, 0,
                                 0, 0, 0, 0,
                                 HWND_MESSAGE, NULL, hInst, NULL);

    /* ── Overlay window class ── */
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = OverlayWndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = WND_CLASS_OVERLAY;
    RegisterClassExW(&wc);

    /* ── Register TaskbarCreated message (Explorer restart recovery) ── */
    s_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    /* ── Tray icon ── */
    HICON hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP));
    if (!hIcon)
        hIcon = LoadIconW(NULL, MAKEINTRESOURCEW(32512)); /* IDI_APPLICATION */

    ZeroMemory(&s_nid, sizeof(s_nid));
    s_nid.cbSize           = sizeof(s_nid);
    s_nid.hWnd             = s_hwndMain;
    s_nid.uID              = 1;
    s_nid.uFlags           = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    s_nid.uCallbackMessage = WM_TRAY_MSG;
    s_nid.hIcon            = hIcon;
    lstrcpyW(s_nid.szTip, L"MyToolKit");

    /* 开机自启时 Explorer 可能尚未就绪，重试几次确保图标添加成功 */
    for (int retry = 0; retry < 10; retry++)
    {
        if (Shell_NotifyIconW(NIM_ADD, &s_nid))
            break;
        Sleep(500);
    }

    /* ── Keyboard hook (CapsRemap + EscClose) ── */
    s_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, OnKeyboardEvent, NULL, 0);
    if (!s_kbHook)
        MessageBoxW(NULL, L"键盘钩子安装失败，CapsLock/ESC/Win+` 功能可能不可用。", TASK_NAME, MB_OK | MB_ICONERROR);

    /* ── Win+` hotkey (WinBacktick: cycle same-app windows) ── */
    RegisterWinBacktickHotkey(s_hwndMain);

    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (s_kbHook) UnhookWindowsHookEx(s_kbHook);
    return 0;
}
