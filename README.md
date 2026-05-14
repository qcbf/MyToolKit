# MyToolKit

轻量级 Win32 工具集。纯 C 实现，零外部依赖，极低内存占用，系统托盘静默驻留。

## 功能

| 快捷键 | 功能 | 说明 |
|--------|------|------|
| `CapsLock` 短按 | Ctrl+Space | 切换输入法，屏蔽原始 CapsLock（LED 不亮） |
| `CapsLock` 长按 (≥400ms) | 大小写切换 | 保留原始 CapsLock 功能 |
| `ESC` 长按 (≥1s) | Alt+F4 | 关闭当前窗口，带倒计时进度条，松开取消 |
| Win+`` ` `` | 同程序窗口切换 | 类似 macOS Cmd+`，在同进程窗口间循环 |

### CapsRemap

将 CapsLock 重映射为 Ctrl+Space（输入法切换），完全屏蔽原始行为。长按 ≥400ms 恢复为大小写切换（macOS 风格）。

### EscClose

长按 ESC ≥1s 自动发送 Alt+F4 关闭当前窗口。按住期间屏幕中央显示半透明倒计时进度条，松开即取消。设有 200ms 激活延迟，避免短按误触。

### WinBacktick

Win+`` ` `` 在当前程序的多个窗口间循环切换，仅对同一进程的可见顶层窗口生效。

## 使用

双击 `mytoolkit.exe` 启动（会弹出 UAC 提权请求）。启动后驻留系统托盘，右键图标可设置开机启动或退出。

> **为什么需要管理员权限？**
> `WH_KEYBOARD_LL` 本身无需提权，但焦点在 UAC 对话框或管理员级窗口（如任务管理器）时，非提权钩子会被跳过，导致按键漏拦。

## 编译

### MSVC（推荐）

需要 Visual Studio Build Tools 或完整 VS：

```bat
build_msvc.bat
```

### MinGW-w64

```bat
windres resource.rc -o resource.o
gcc -O2 -s -mwindows mytoolkit.c resource.o -o mytoolkit.exe -luser32 -lshell32 -ladvapi32 -lgdi32
```

## 文件结构

```
mytoolkit.c      主逻辑（键盘钩子 + 热键 + 托盘 + 开机启动）
resource.rc      嵌入 icon + manifest
app.ico          托盘图标
app.manifest     UAC 提权配置
build_msvc.bat   MSVC 一键编译脚本
```

## 实现原理

```
CapsLock 按下
  └─ WH_KEYBOARD_LL 钩子
       ├─ 过滤注入事件（防 SendInput 反馈环）
       ├─ 短按 (<400ms): SendInput(Ctrl+Space)
       ├─ 长按 (≥400ms): SendInput(CapsLock) → 切换大小写
       └─ return 1 → 吞掉原始事件

ESC 按下
  └─ 200ms 延迟后激活倒计时
       ├─ 持续 1s → SendInput(Alt+F4)
       └─ 中途松开 → 取消

Win+` 按下
  └─ RegisterHotKey 触发 WM_HOTKEY
       ├─ 获取前台窗口所属进程
       ├─ EnumWindows 枚举同进程可见窗口
       └─ SetForegroundWindow 切换到下一个
```
