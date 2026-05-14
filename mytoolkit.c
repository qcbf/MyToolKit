/*
 * MyToolKit.exe
 * A lightweight Win32 toolkit. Features:
 *   - CapsRemap: CapsLock -> Ctrl+Space
 *   - System tray icon with right-click menu
 *
 * Build (MSVC):
 *   build_msvc.bat
 *
 * Build (MinGW-w64):
 *   windres resource.rc -o resource.o
 *   gcc -O2 -s -mwindows mytoolkit.c resource.o -o mytoolkit.exe -luser32 -lshell32 -ladvapi32
 */

#ifdef _MSC_VER
#   pragma comment(lib, "user32.lib")
#   pragma comment(lib, "shell32.lib")
#   pragma comment(lib, "advapi32.lib")
#   pragma comment(linker, "/SUBSYSTEM:WINDOWS")
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

/* ── Resource / message IDs ── */
#define IDI_APP          1
#define WM_TRAY_MSG      (WM_APP + 1)
#define ID_MENU_STARTUP  100
#define ID_MENU_EXIT     101

#define TASK_NAME   L"MyToolKit"
#define MUTEX_NAME  L"Global\\MyToolKit_SingleInstance"
#define WND_CLASS   L"MyToolKit_TrayWnd"

static HHOOK           s_kbHook;
static HINSTANCE       s_hInst;
static NOTIFYICONDATAW s_nid;

/* ════════════════════════════════════════════
 *  Feature: CapsRemap
 *  CapsLock -> Ctrl+Space (low-level keyboard hook)
 * ════════════════════════════════════════════ */
static LRESULT CALLBACK OnKeyboardEvent(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION)
    {
        const KBDLLHOOKSTRUCT* kb = (const KBDLLHOOKSTRUCT*)lParam;

        /*
         * Only intercept real CapsLock (skip injected/synthetic keystrokes
         * to prevent feedback loops with our own SendInput).
         */
        if (kb->vkCode == VK_CAPITAL && !(kb->flags & LLKHF_INJECTED))
        {
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
            {
                INPUT seq[4];
                ZeroMemory(seq, sizeof(seq));

                seq[0].type       = INPUT_KEYBOARD;
                seq[0].ki.wVk     = VK_LCONTROL;

                seq[1].type       = INPUT_KEYBOARD;
                seq[1].ki.wVk     = VK_SPACE;

                seq[2].type       = INPUT_KEYBOARD;
                seq[2].ki.wVk     = VK_SPACE;
                seq[2].ki.dwFlags = KEYEVENTF_KEYUP;

                seq[3].type       = INPUT_KEYBOARD;
                seq[3].ki.wVk     = VK_LCONTROL;
                seq[3].ki.dwFlags = KEYEVENTF_KEYUP;

                SendInput(4, seq, sizeof(INPUT));
            }

            /* Suppress original CapsLock: no LED toggle, no caps-state change. */
            return 1;
        }
    }
    return CallNextHookEx(s_kbHook, code, wParam, lParam);
}

/* ════════════════════════════════════════════
 *  Elevation helpers
 * ════════════════════════════════════════════ */
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

/* ════════════════════════════════════════════
 *  Single-instance guard
 * ════════════════════════════════════════════ */
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

/* ════════════════════════════════════════════
 *  Auto-start (Task Scheduler)
 * ════════════════════════════════════════════ */

/* Run `cmd.exe /c <cmdLine>` silently; return process exit code. */
static DWORD RunCmd(LPCWSTR cmdLine)
{
    WCHAR buf[1024];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD exitCode = 1;

    wsprintfW(buf, L"/c %s", cmdLine);
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
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

/* ════════════════════════════════════════════
 *  Tray icon & context menu
 * ════════════════════════════════════════════ */
static void ShowTrayMenu(HWND hwnd)
{
    HMENU hMenu = CreatePopupMenu();
    BOOL  startupOn = IsStartupEnabled();

    AppendMenuW(hMenu,
                MF_STRING | (startupOn ? MF_CHECKED : MF_UNCHECKED),
                ID_MENU_STARTUP, L"开机启动");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_MENU_EXIT, L"退出");

    POINT pt;
    GetCursorPos(&pt);

    /* Required so the menu dismisses when clicking elsewhere. */
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
    case WM_TRAY_MSG:
        if (lParam == WM_RBUTTONUP)
            ShowTrayMenu(hwnd);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case ID_MENU_STARTUP:
            ToggleStartup();
            break;
        case ID_MENU_EXIT:
            DestroyWindow(hwnd);
            break;
        }
        return 0;

    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &s_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* ════════════════════════════════════════════
 *  Entry point
 * ════════════════════════════════════════════ */
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow)
{
    WNDCLASSEXW wc;
    HWND        hwnd;
    HICON       hIcon;
    MSG         msg;

    (void)hPrev; (void)lpCmd; (void)nShow;
    s_hInst = hInst;

    EnsureElevated();
    if (IsAlreadyRunning()) return 1;

    /* Register hidden message-only window class */
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = WND_CLASS;
    RegisterClassExW(&wc);

    hwnd = CreateWindowExW(0, WND_CLASS, NULL, 0,
                           0, 0, 0, 0,
                           HWND_MESSAGE, NULL, hInst, NULL);

    /* ── Tray icon ── */
    hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP));
    if (!hIcon)
        hIcon = LoadIconW(NULL, IDI_APPLICATION);

    ZeroMemory(&s_nid, sizeof(s_nid));
    s_nid.cbSize           = sizeof(s_nid);
    s_nid.hWnd             = hwnd;
    s_nid.uID              = 1;
    s_nid.uFlags           = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    s_nid.uCallbackMessage = WM_TRAY_MSG;
    s_nid.hIcon            = hIcon;
    lstrcpyW(s_nid.szTip, L"MyToolKit");
    Shell_NotifyIconW(NIM_ADD, &s_nid);

    /* ── CapsRemap keyboard hook ── */
    s_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, OnKeyboardEvent, NULL, 0);

    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (s_kbHook) UnhookWindowsHookEx(s_kbHook);
    return 0;
}
