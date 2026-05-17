using System.Drawing;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace MyToolKit;

/// <summary>
/// 真正的无窗体后台运行方案。
/// 继承 ApplicationContext 而非 Form，避免了创建任何窗口句柄（HWND），
/// 从而在内存占用、能耗和系统资源消耗上达到最优。
/// </summary>
public class TrayApplicationContext : ApplicationContext
{
    private const int HideCheckIntervalMs = 80;

    private NotifyIcon? _trayIcon;
    private ContextMenuStrip? _contextMenu;
    private ToolStripMenuItem? _capsLockMenuItem;
    private ToolStripMenuItem? _autoStartMenuItem;

    private AppSettings _settings = null!;
    private KeyboardHook? _keyboardHook;
    private LauncherForm? _launcherForm;
    private System.Windows.Forms.Timer? _hideCheckTimer;
    private Rectangle _trayAnchorBounds;
    private bool _suppressHoverShow;
    private bool _hoverTrackingActive;
    private IntPtr _foregroundHook = IntPtr.Zero;
    private WinEventDelegate? _foregroundHookDelegate;

    private const uint EVENT_SYSTEM_FOREGROUND = 0x0003;
    private const uint WINEVENT_OUTOFCONTEXT = 0x0000;
    private const uint WINEVENT_SKIPOWNPROCESS = 0x0002;

    private delegate void WinEventDelegate(IntPtr hWinEventHook, uint eventType, IntPtr hwnd, int idObject, int idChild, uint dwEventThread, uint dwmsEventTime);

    public TrayApplicationContext()
    {
        InitializeTrayIcon();
        LoadSettings();
        InitializeHooks();
        InitializeLauncher();
    }

    private void InitializeTrayIcon()
    {
        _contextMenu = new ContextMenuStrip();
        
        // 帮助菜单
        var helpMenuItem = new ToolStripMenuItem("帮助", null, OnHelpClick);
        _contextMenu.Items.Add(helpMenuItem);
        
        _contextMenu.Items.Add(new ToolStripSeparator());
        
        // CapsLock 功能
        _capsLockMenuItem = new ToolStripMenuItem("CapsLock键中英文", null, OnCapsLockToggle);
        _contextMenu.Items.Add(_capsLockMenuItem);
        
        _contextMenu.Items.Add(new ToolStripSeparator());
        
        // 开机自动启动
        _autoStartMenuItem = new ToolStripMenuItem("开机自动启动", null, OnAutoStartToggle);
        _contextMenu.Items.Add(_autoStartMenuItem);
        
        _contextMenu.Items.Add(new ToolStripSeparator());
        
        // 退出
        var exitMenuItem = new ToolStripMenuItem("退出", null, OnExitClick);
        _contextMenu.Items.Add(exitMenuItem);

        _trayIcon = new NotifyIcon
        {
            Icon = LoadEmbeddedIcon(),
            Text = "我的工具包",
            Visible = true,
            ContextMenuStrip = _contextMenu
        };

        _trayIcon.MouseMove += OnTrayMouseMove;
        _trayIcon.MouseClick += OnTrayMouseClick;
        _trayIcon.MouseDoubleClick += OnTrayMouseClick;
        _trayIcon.MouseDown += OnTrayMouseDown;
    }

    private void InitializeLauncher()
    {
        _launcherForm = new LauncherForm(_settings, LoadEmbeddedIcon());
        _launcherForm.RequestSettingsSave += OnLauncherSettingsSaveRequested;
        _launcherForm.RequestHide += (_, _) => HideLauncher();
        _launcherForm.ItemInvoked += (_, item) => LaunchItem(item);
        _launcherForm.Deactivate += OnLauncherDeactivate;

        _hideCheckTimer = new System.Windows.Forms.Timer
        {
            Interval = HideCheckIntervalMs
        };
        _hideCheckTimer.Tick += OnHideCheckTimerTick;

        _foregroundHookDelegate = OnForegroundChanged;
        _foregroundHook = SetWinEventHook(
            EVENT_SYSTEM_FOREGROUND,
            EVENT_SYSTEM_FOREGROUND,
            IntPtr.Zero,
            _foregroundHookDelegate,
            0,
            0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    }

    private Icon LoadEmbeddedIcon()
    {
        try
        {
            var assembly = System.Reflection.Assembly.GetExecutingAssembly();
            using var stream = assembly.GetManifestResourceStream("MyToolKit.app.ico");
            if (stream != null)
            {
                return new Icon(stream);
            }
        }
        catch
        {
            // 如果加载嵌入资源失败，尝试从文件加载
        }
        
        //  fallback: 从文件加载
        var iconPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "app.ico");
        if (File.Exists(iconPath))
        {
            return new Icon(iconPath);
        }
        
        // 最后fallback: 使用系统默认图标
        return SystemIcons.Application;
    }

    private void LoadSettings()
    {
        _settings = AppSettings.Load();
        if (_settings.LauncherGroups is null || _settings.LauncherGroups.Count == 0)
        {
            _settings.LauncherGroups = LauncherDefaults.Create();
            _settings.Save();
        }
        UpdateMenuCheckStates();
    }

    private void UpdateMenuCheckStates()
    {
        if (_capsLockMenuItem != null)
            _capsLockMenuItem.Checked = _settings.CapsLockEnabled;
        if (_autoStartMenuItem != null)
            _autoStartMenuItem.Checked = IsAutoStartEnabled();
    }

    private bool IsAutoStartEnabled()
    {
        try
        {
            using var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(
                @"Software\Microsoft\Windows\CurrentVersion\Run", false);
            
            if (key == null) return false;
            
            var appName = "MyToolKit";
            var value = key.GetValue(appName);
            return value != null && value.ToString() == Application.ExecutablePath;
        }
        catch
        {
            return false;
        }
    }

    private void InitializeHooks()
    {
        _keyboardHook = new KeyboardHook();
        _keyboardHook.CapsLockShortPress += () => SimulateInput.SendCtrlSpace();
        _keyboardHook.CapsLockLongPress += () => SimulateInput.ToggleCapsLock();

        if (_settings.CapsLockEnabled)
            _keyboardHook.Start();
    }

    private void OnHelpClick(object? sender, EventArgs e)
    {
        var message = @"我的工具包 - 功能说明：

• CapsLock键中英文：短按切换中英文输入法，长按切换大小写锁定
• 开机自动启动：随系统启动时自动运行本程序
• 快捷启动器：鼠标经过或左键单击托盘图标显示，支持分组、增删项目、快速启动
• 退出：关闭程序";
        
        MessageBox.Show(message, "帮助", MessageBoxButtons.OK, MessageBoxIcon.Information);
    }

    private void OnCapsLockToggle(object? sender, EventArgs e)
    {
        _settings.CapsLockEnabled = !_settings.CapsLockEnabled;
        _settings.Save();
        UpdateMenuCheckStates();
        
        if (_settings.CapsLockEnabled)
            _keyboardHook?.Start();
        else
            _keyboardHook?.Stop();
    }

    private void OnAutoStartToggle(object? sender, EventArgs e)
    {
        var isEnabled = IsAutoStartEnabled();
        ApplyAutoStart(!isEnabled);
        UpdateMenuCheckStates();
    }

    private void ApplyAutoStart(bool enable)
    {
        try
        {
            using var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(
                @"Software\Microsoft\Windows\CurrentVersion\Run", true);
            
            if (key == null) return;
            
            var appName = "MyToolKit";
            var appPath = Application.ExecutablePath;
            
            if (enable)
            {
                key.SetValue(appName, appPath);
            }
            else
            {
                key.DeleteValue(appName, false);
            }
        }
        catch
        {
            // 忽略注册表操作错误
        }
    }

    private void OnExitClick(object? sender, EventArgs e)
    {
        ExitThread();
    }

    private void OnTrayMouseMove(object? sender, MouseEventArgs e)
    {
        if (e.Button != MouseButtons.None || _suppressHoverShow)
        {
            return;
        }

        if (_hoverTrackingActive)
        {
            return;
        }

        _hoverTrackingActive = true;
        UpdateTrayAnchorBounds();
        ShowLauncher();
    }

    private void OnTrayMouseDown(object? sender, MouseEventArgs e)
    {
        UpdateTrayAnchorBounds();

        if (e.Button == MouseButtons.Right)
        {
            _suppressHoverShow = true;
            _hoverTrackingActive = false;
            HideLauncher();
        }
    }

    private void OnTrayMouseClick(object? sender, MouseEventArgs e)
    {
        if (e.Button == MouseButtons.Left)
        {
            _suppressHoverShow = false;
            _hoverTrackingActive = false;
            UpdateTrayAnchorBounds();
            ToggleLauncher();
        }
    }

    private void ToggleLauncher()
    {
        if (_launcherForm is { Visible: true })
        {
            HideLauncher();
            return;
        }

        ShowLauncher();
    }

    private void ShowLauncher()
    {
        if (_launcherForm == null || _trayIcon == null)
        {
            return;
        }

        UpdateTrayAnchorBounds();
        _launcherForm.BindSettings(_settings);
        _launcherForm.ShowNearTray(_trayAnchorBounds);
        _hideCheckTimer?.Start();
    }

    private void HideLauncher()
    {
        _hoverTrackingActive = false;
        _hideCheckTimer?.Stop();
        _launcherForm?.HideLauncher();
    }

    private void UpdateTrayAnchorBounds()
    {
        var cursor = Cursor.Position;
        const int size = 40;
        _trayAnchorBounds = new Rectangle(cursor.X - (size / 2), cursor.Y - (size / 2), size, size);
    }

    private void OnLauncherDeactivate(object? sender, EventArgs e)
    {
        if (_launcherForm is not { Visible: true })
        {
            return;
        }

        var foreground = GetForegroundWindow();
        if (_launcherForm.Handle == foreground || IsChildWindow(_launcherForm.Handle, foreground))
        {
            return;
        }

        if (_launcherForm.ShouldRemainVisible(Cursor.Position))
        {
            return;
        }

        HideLauncher();
    }

    private void OnForegroundChanged(IntPtr hWinEventHook, uint eventType, IntPtr hwnd, int idObject, int idChild, uint dwEventThread, uint dwmsEventTime)
    {
        if (eventType != EVENT_SYSTEM_FOREGROUND || _launcherForm is not { Visible: true })
        {
            return;
        }

        if (_launcherForm.IsDisposed || hwnd == IntPtr.Zero)
        {
            return;
        }

        if (_launcherForm.Handle == hwnd || IsChildWindow(_launcherForm.Handle, hwnd))
        {
            return;
        }

        _launcherForm.BeginInvoke(new Action(() =>
        {
            if (_launcherForm is not { Visible: true })
            {
                return;
            }

            if (!_launcherForm.ShouldRemainVisible(Cursor.Position))
            {
                HideLauncher();
            }
        }));
    }

    private void OnHideCheckTimerTick(object? sender, EventArgs e)
    {
        if (_launcherForm is not { Visible: true })
        {
            _hideCheckTimer?.Stop();
            return;
        }

        if (!_launcherForm.ShouldRemainVisible(Cursor.Position))
        {
            HideLauncher();
        }
    }

    private void OnLauncherSettingsSaveRequested(object? sender, EventArgs e)
    {
        _settings.Save();
    }

    private void LaunchItem(LauncherItem item)
    {
        try
        {
            var targetPath = item.TargetPath?.Trim();
            if (string.IsNullOrWhiteSpace(targetPath))
            {
                return;
            }

            var workingDirectory = item.WorkingDirectory?.Trim();
            if (string.IsNullOrWhiteSpace(workingDirectory) && File.Exists(targetPath))
            {
                workingDirectory = Path.GetDirectoryName(targetPath) ?? string.Empty;
            }

            var startInfo = new System.Diagnostics.ProcessStartInfo
            {
                FileName = targetPath,
                Arguments = item.Arguments ?? string.Empty,
                UseShellExecute = true,
                WorkingDirectory = string.IsNullOrWhiteSpace(workingDirectory) ? Environment.CurrentDirectory : workingDirectory
            };

            System.Diagnostics.Process.Start(startInfo);
        }
        catch (Exception ex)
        {
            MessageBox.Show($"启动失败：{ex.Message}", "我的工具包", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
        finally
        {
            HideLauncher();
        }
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _hideCheckTimer?.Dispose();

            if (_foregroundHook != IntPtr.Zero)
            {
                UnhookWinEvent(_foregroundHook);
                _foregroundHook = IntPtr.Zero;
            }

            _launcherForm?.Dispose();
            _keyboardHook?.Dispose();
            _trayIcon?.Dispose();
            _contextMenu?.Dispose();
        }
        base.Dispose(disposing);
    }

    [DllImport("user32.dll")]
    private static extern IntPtr SetWinEventHook(uint eventMin, uint eventMax, IntPtr hmodWinEventProc, WinEventDelegate lpfnWinEventProc, uint idProcess, uint idThread, uint dwFlags);

    [DllImport("user32.dll")]
    private static extern bool UnhookWinEvent(IntPtr hWinEventHook);

    [DllImport("user32.dll")]
    private static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsChild(IntPtr hWndParent, IntPtr hWnd);

    private static bool IsChildWindow(IntPtr parentHandle, IntPtr candidateHandle)
    {
        return candidateHandle != IntPtr.Zero && IsChild(parentHandle, candidateHandle);
    }
}
