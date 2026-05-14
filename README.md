# 让ai用c开发的, 要求他极致节能高效, 低内存占用. 可以直接用编译好的`capsremap.exe` 直接运行就行, 也可以用 `startup` 参数一键添加开机自启.
## 管理员运行的目的是 如果不是管理员权限在某些程序比如`任务管理器`会失效.

# CapsRemap — CapsLock → Ctrl+Space

极简 Win32 键盘钩子程序。纯 C，无依赖，~12KB 二进制，~400KB 内存。

---

## 文件结构

```
capsremap.c      主逻辑
app.manifest     UAC 提权声明（MinGW 用）
resource.rc      资源文件，嵌入 manifest（MinGW 用）
build_msvc.bat   MSVC 一键编译
build_mingw.bat  MinGW 一键编译
```

---

## 编译

### 方式 A — MSVC（推荐，二进制最小）

需要 **Visual Studio Build Tools**（免费）或完整 VS。

```bat
build_msvc.bat
```

手动命令：
```bat
cl /O1 /Os /GS- /W3 capsremap.c /link /LTCG /OPT:REF /OPT:ICF
```

生成：`capsremap.exe` ≈ 10–14 KB

### 方式 B — MinGW-w64

```bat
build_mingw.bat
```

手动命令：
```bat
windres resource.rc -o resource.o
gcc -O2 -s -mwindows capsremap.c resource.o -o capsremap.exe -luser32 -lshell32
```

生成：`capsremap.exe` ≈ 25–35 KB（含 MinGW CRT）

---

## 命令行参数

```
capsremap.exe              无参数，直接启动键盘钩子（需要管理员权限）
capsremap.exe startup      启动键盘钩子，并通过任务计划程序添加到开机自启
capsremap.exe unstartup    从开机自启中移除（不启动键盘钩子）
capsremap.exe help         显示帮助信息
```

示例：
```bat
:: 首次使用：启动 + 添加开机自启
capsremap.exe startup

:: 不再需要开机自启
capsremap.exe unstartup
```

> `startup` 和 `unstartup` 会自动请求管理员权限（UAC 弹窗），因为任务计划程序操作需要提升权限。

---

## 开机自启

程序内置了命令行参数管理开机自启（基于任务计划程序，保留管理员权限）。

也可以手动创建任务计划：

```
任务计划程序 → 创建任务
  常规 → 勾选"使用最高权限运行"
  触发器 → 登录时
  操作 → 启动程序 → capsremap.exe
  条件 → 取消"仅在交流电源下运行"
```

> 注意：不建议将 exe 直接放入 `shell:startup`（启动文件夹），因为该方式以普通用户权限运行，无法在提升权限的窗口（如任务管理器）中拦截按键。

---

## 原理

```
CapsLock 按下
  └── WH_KEYBOARD_LL 钩子触发
        ├── 检测 VK_CAPITAL + 非注入
        ├── SendInput([LCtrl↓, Space↓, Space↑, LCtrl↑])
        └── return 1  →  吞掉原始 CapsLock（LED 不亮，不切换大小写）
```

**为什么需要管理员权限：**  
`WH_KEYBOARD_LL` 可不提权运行，但当焦点在 UAC 对话框或其他管理员级别窗口时，非提权钩子会被跳过，导致 CapsLock 漏拦。

---

## 退出

任务管理器 → 结束 `capsremap.exe` 进程，CapsLock 恢复正常。
