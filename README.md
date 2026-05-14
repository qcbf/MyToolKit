# MyToolKit

轻量级 Win32 工具集。纯 C，无依赖，极低内存占用，系统托盘驻留。

---

## 功能列表

### CapsRemap — CapsLock → Ctrl+Space

将 CapsLock 键重映射为 Ctrl+Space（输入法切换），完全屏蔽原始 CapsLock 行为（不亮 LED，不切换大小写）。

### EscClose — 长按 ESC 3s 关闭窗口

长按 ESC 键 3 秒自动发送 Alt+F4 关闭当前窗口。按住期间屏幕中央显示半透明倒计时进度条，松开即取消。

---

## 使用方式

双击 `mytoolkit.exe` 启动（会弹出 UAC 提权请求）。

启动后程序驻留托盘，右键托盘图标弹出菜单：

| 菜单项 | 说明 |
|--------|------|
| ✓ 开机启动 | 切换开机自启状态（任务计划程序，保留管理员权限） |
| 退出 | 结束程序，CapsLock 恢复正常 |

---

## 文件结构

```
mytoolkit.c      主逻辑（CapsRemap + 托盘 + 开机启动管理）
app.ico          托盘/窗口图标
app.manifest     UAC 配置（MinGW 用；MSVC 通过 resource.rc 嵌入）
resource.rc      嵌入 icon + manifest
build_msvc.bat   MSVC 一键编译
```

---

## 编译

### MSVC（推荐，二进制最小 ~12KB）

需要 **Visual Studio Build Tools** 或完整 VS。

```bat
build_msvc.bat
```

### MinGW-w64

```bat
windres resource.rc -o resource.o
gcc -O2 -s -mwindows mytoolkit.c resource.o -o mytoolkit.exe -luser32 -lshell32 -ladvapi32
```

---

## 原理

```
CapsLock 按下
  └── WH_KEYBOARD_LL 钩子触发
        ├── 检测 VK_CAPITAL + 非注入（防止自身 SendInput 形成反馈环）
        ├── SendInput([LCtrl↓, Space↓, Space↑, LCtrl↑])
        └── return 1  →  吞掉原始 CapsLock（LED 不亮，不切换大小写）
```

**为什么需要管理员权限：**  
`WH_KEYBOARD_LL` 无需提权也能运行，但焦点在 UAC 对话框或其他管理员级窗口（如任务管理器）时，非提权钩子会被跳过，导致 CapsLock 漏拦。
