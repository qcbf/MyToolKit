/*
 * CapsRemap.exe
 * Maps CapsLock -> Ctrl(Left) + Space
 *
 * Build (MSVC / Visual Studio Build Tools):
 *   cl /O1 /Os /GS- capsremap.c /link /LTCG /OPT:REF /OPT:ICF
 *
 * Build (MinGW-w64):
 *   windres resource.rc -o resource.o
 *   gcc -O2 -s -mwindows capsremap.c resource.o -o capsremap.exe -luser32 -lshell32 -ladvapi32
 *
 * Usage:
 *   capsremap.exe              Start the keyboard hook (requires admin)
 *   capsremap.exe startup      Start hook + add to auto-start (Task Scheduler)
 *   capsremap.exe unstartup    Remove from auto-start (does not start hook)
 *   capsremap.exe help         Show help
 */

/* ── MSVC: embed manifest + set subsystem inline, no .rc file needed ── */
#ifdef _MSC_VER
#   pragma comment(lib, "user32.lib")
#   pragma comment(lib, "shell32.lib")
#   pragma comment(lib, "advapi32.lib")
#   pragma comment(linker, "/SUBSYSTEM:WINDOWS")
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>   /* ShellExecuteExW, SHELLEXECUTEINFOW */

/* ── Single global hook handle ── */
static HHOOK s_kbHook;

/* ── Low-level keyboard hook callback ── */
static LRESULT CALLBACK OnKeyboardEvent(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION)
    {
        const KBDLLHOOKSTRUCT* kb = (const KBDLLHOOKSTRUCT*)lParam;

        /*
         * Only intercept real CapsLock events.
         * LLKHF_INJECTED: skip synthetic keystrokes (including our own SendInput)
         * to prevent any feedback loop.
         */
        if (kb->vkCode == VK_CAPITAL && !(kb->flags & LLKHF_INJECTED))
        {
            /* Fire Ctrl+Space only on key-down (ignore auto-repeat via LLKHF_UP). */
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
            {
                INPUT seq[4];
                ZeroMemory(seq, sizeof(seq));

                /* LCtrl down */
                seq[0].type          = INPUT_KEYBOARD;
                seq[0].ki.wVk        = VK_LCONTROL;

                /* Space down */
                seq[1].type          = INPUT_KEYBOARD;
                seq[1].ki.wVk        = VK_SPACE;

                /* Space up */
                seq[2].type          = INPUT_KEYBOARD;
                seq[2].ki.wVk        = VK_SPACE;
                seq[2].ki.dwFlags    = KEYEVENTF_KEYUP;

                /* LCtrl up */
                seq[3].type          = INPUT_KEYBOARD;
                seq[3].ki.wVk        = VK_LCONTROL;
                seq[3].ki.dwFlags    = KEYEVENTF_KEYUP;

                SendInput(4, seq, sizeof(INPUT));
            }

            /* Return non-zero to suppress the original CapsLock keystroke
             * (prevents LED toggle and blocks the key from reaching any app). */
            return 1;
        }
    }

    return CallNextHookEx(s_kbHook, code, wParam, lParam);
}

/* ── Check current process elevation ── */
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

/* ── Re-launch self with UAC elevation if not already elevated ── */
static void EnsureElevated(void)
{
    /* C89: all declarations at the top of the block */
    WCHAR exePath[MAX_PATH];
    SHELLEXECUTEINFOW sei;

    if (IsRunningElevated()) return;

    GetModuleFileNameW(NULL, exePath, MAX_PATH);

    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize  = sizeof(sei);
    sei.lpVerb  = L"runas";
    sei.lpFile  = exePath;
    sei.nShow   = SW_HIDE;

    ShellExecuteExW(&sei);
    ExitProcess(0);
}

/* ── Re-launch self with UAC, passing the same arguments through ── */
static void RelaunchElevated(LPWSTR cmdArgs)
{
    WCHAR exePath[MAX_PATH];
    SHELLEXECUTEINFOW sei;

    GetModuleFileNameW(NULL, exePath, MAX_PATH);

    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize  = sizeof(sei);
    sei.lpVerb  = L"runas";
    sei.lpFile  = exePath;
    sei.lpParameters = cmdArgs;
    sei.nShow   = SW_HIDE;

    ShellExecuteExW(&sei);
    ExitProcess(0);
}

#define TASK_NAME L"CapsRemap"
#define MUTEX_NAME L"Global\\CapsRemap_SingleInstance"

/* ── Check if another instance is already running ── */
static BOOL IsAlreadyRunning(void)
{
    static HANDLE s_hMutex = NULL;  /* keep alive for process lifetime */
    s_hMutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        MessageBoxW(NULL,
            L"CapsRemap 已在运行中。\n\n"
            L"请勿重复启动，程序会在后台持续工作。",
            TASK_NAME, MB_OK | MB_ICONWARNING);
        return TRUE;
    }
    return FALSE;
}

/* ── Add to startup via Task Scheduler (requires elevation) ── */
static void AddStartup(void)
{
    WCHAR exePath[MAX_PATH];
    WCHAR cmdArgs[1024];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD exitCode = 1;

    GetModuleFileNameW(NULL, exePath, MAX_PATH);

    /*
     * Build: schtasks /create /tn "CapsRemap" /tr "\"<exePath>\"" /sc ONLOGON /rl HIGHEST /f
     */
    wsprintfW(cmdArgs,
        L"/c schtasks /create /tn \"%s\" /tr \"\\\"%s\\\"\" /sc ONLOGON /rl HIGHEST /f",
        TASK_NAME, exePath);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (CreateProcessW(L"C:\\Windows\\System32\\cmd.exe", cmdArgs,
                       NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
    {
        WaitForSingleObject(pi.hProcess, 10000);
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    if (exitCode == 0)
        MessageBoxW(NULL, L"已添加到开机启动（任务计划程序）。\n"
                          L"程序将以管理员权限在登录时自动运行。",
                    TASK_NAME, MB_OK | MB_ICONINFORMATION);
    else
        MessageBoxW(NULL, L"添加开机启动失败。\n"
                          L"请确保以管理员权限运行。",
                    TASK_NAME, MB_OK | MB_ICONERROR);
}

/* ── Remove from startup via Task Scheduler (requires elevation) ── */
static void RemoveStartup(void)
{
    WCHAR cmdArgs[256];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD exitCode = 1;

    wsprintfW(cmdArgs, L"/c schtasks /delete /tn \"%s\" /f", TASK_NAME);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (CreateProcessW(L"C:\\Windows\\System32\\cmd.exe", cmdArgs,
                       NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
    {
        WaitForSingleObject(pi.hProcess, 10000);
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    if (exitCode == 0)
        MessageBoxW(NULL, L"已从开机启动中移除。",
                    TASK_NAME, MB_OK | MB_ICONINFORMATION);
    else
        MessageBoxW(NULL, L"未找到开机启动任务，或移除失败。",
                    TASK_NAME, MB_OK | MB_ICONWARNING);
}

/* ── Show help message box ── */
static void ShowHelp(void)
{
    MessageBoxW(NULL,
        L"CapsRemap — CapsLock → Ctrl+Space\n\n"
        L"用法：\n"
        L"  capsremap.exe              启动键盘钩子（需要管理员权限）\n"
        L"  capsremap.exe startup      启动并添加到开机启动\n"
        L"  capsremap.exe unstartup    从开机启动中移除（不启动功能）\n"
        L"  capsremap.exe help         显示此帮助信息\n\n"
        L"开机启动使用任务计划程序，以管理员权限在登录时自动运行。",
        L"CapsRemap 帮助", MB_OK | MB_ICONINFORMATION);
}

/* ── Entry point (no console window) ── */
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow)
{
    int argc = 0;
    LPWSTR* argv = NULL;
    MSG msg;

    (void)hInst; (void)hPrev; (void)lpCmd; (void)nShow;

    /* Parse command line */
    argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    if (argc >= 2)
    {
        if (lstrcmpiW(argv[1], L"help") == 0)
        {
            ShowHelp();
            LocalFree(argv);
            return 0;
        }
        if (lstrcmpiW(argv[1], L"startup") == 0)
        {
            /* Need elevation for schtasks; relaunch with the same flag */
            if (!IsRunningElevated())
            {
                RelaunchElevated(argv[1]);
                LocalFree(argv);
                return 0;
            }
            AddStartup();
            /* fall through to start the hook */
        }
        else if (lstrcmpiW(argv[1], L"unstartup") == 0)
        {
            if (!IsRunningElevated())
            {
                RelaunchElevated(argv[1]);
                LocalFree(argv);
                return 0;
            }
            RemoveStartup();
            LocalFree(argv);
            return 0;
        }
    }

    if (argv) LocalFree(argv);

    /*
     * Programmatic elevation fallback for MinGW builds that cannot embed
     * the requireAdministrator manifest easily. MSVC builds hit the manifest
     * before WinMain and Windows shows the UAC prompt automatically.
     */
    EnsureElevated();

    /* Single-instance guard: notify user if already running */
    if (IsAlreadyRunning()) return 1;

    s_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, OnKeyboardEvent, NULL, 0);
    if (!s_kbHook) return 1;

    /* Minimal message pump — no window needed; the hook runs on this thread. */
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
        DispatchMessageW(&msg);

    UnhookWindowsHookEx(s_kbHook);
    return 0;
}
