# Ctrl + Backspace 快捷键：可行性与原版行为

分析日期：2026-10-02。本文记录修复前的分析和独立测试；后续实现见 [Backspace 组合快捷键修复](HOTKEY_BACKSPACE_RECOVERY.md)。原分析阶段未修改播放器实现或用户快捷键配置。

## 结论

Ctrl + Backspace 可以实现。当前问题位于快捷键录入控件，而不是 Windows 禁止此组合，也不是重建版配置无法存储 Backspace。

原版资源页和重建版都采用标准 `HOTKEY_CLASS`，实际类名为 **`msctls_hotkey32`**。原版所检查的页面初始化、命令分发和控件读写路径没有对 Backspace 做额外录入处理；因此从资源、伪代码和独立控件实测判断，它也继承该标准控件的限制。这里不把独立控件测试说成原版 EXE 的现场键盘实测。

## 原版证据

原版资源 `ttpres.dll` 的 DIALOG 252 有两只 `msctls_hotkey32` 控件：应用内快捷键 `2054 / 0x806`、全局键 `2055 / 0x807`。主程序清单启用 Common Controls 6。

| 原版函数 | 行为 | 与本问题的关系 |
| --- | --- | --- |
| `004932A1` | 取得两个控件句柄、初始化快捷键列表 | 未见替换控件窗口过程或专门捕获 Backspace |
| `0048F1BB` | 页面消息分发，“设置”按钮进入 `004935C3` | 无 Ctrl + Backspace 专门消息处理 |
| `004A0C3E` | 发送 `0x401 / HKM_SETHOTKEY` | 将按键与修饰位组合后设置到控件 |
| `004A0C14` | 发送 `0x402 / HKM_GETHOTKEY` | 返回值低八位为按键，高八位为修饰位；不额外过滤 Backspace |
| `004935C3` | 将控件读值存入当前 12 字节记录 | 输入阶段已经变成零时，设置按钮只能保存“无” |
| `0046228D`、`00452E88` | 生成应用内 ACCEL 表，转换 Ctrl／Shift／Alt 位 | 只跳过零按键，没有排除 `VK_BACK=8` |
| `004038DC` | 转换修饰位，调用 `RegisterHotKey` 注册全局快捷键 | 未专门排除 Backspace，并且不检查注册返回值 |

源码依据：[原版伪代码](../../reverse/decompiled/TTPlayer.exe.pseudo.c)。

这需要区分两件事：原版界面能否通过敲键录入，以及原版执行层能否接受已有的键值。后者未发现禁止 `Ctrl=2 / Backspace=8` 的检查，但前者受系统控件限制。

## 独立实测

宿主机上创建不可见的标准 HotKey 控件，以线程私有键盘状态及控件消息模拟输入；没有向用户桌面发送按键，也没有改动播放器配置。分别测试默认控件及显式启用 Common Controls 6 的控件，主要结果相同。

| 操作 | `HKM_GETHOTKEY`／API 结果 |
| --- | --- |
| 直接 `HKM_SETHOTKEY(0x0208)` | 读回 `0x0208`，即 Ctrl + Backspace |
| 模拟 Ctrl + A，包括持续重复输入 | 保持 `0x0241` |
| 模拟 Ctrl + Backspace | 变为 `0x0000` |
| Ctrl + Backspace 再重复十次，随后松键 | 仍为 `0x0000` |
| `RegisterHotKey(..., MOD_CONTROL, VK_BACK)` | 成功，随后立即注销 |

因此“按住后无法绑定”不是等得不够久，自动重复也不会补出有效值。程序设置可以保留该组合，说明控件的键盘录入路径与其存储值能力并不相同。

微软文档将 Backspace 列入 HotKey 控件对 `WM_KEYDOWN / WM_SYSKEYDOWN` 特殊处理的按键；`HKM_SETRULES` 控制的是修饰键组合规则，不能替代这一按键处理。[Hot Key 控件文档](https://learn.microsoft.com/en-us/windows/win32/controls/hot-key-controls)

全局快捷键能否注册还取决于是否已被其它程序占用；本机这一次成功不保证每台机器都成功。应检查 `RegisterHotKey` 的返回值与错误码。[RegisterHotKey 文档](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-registerhotkey)

## 重建版对应位置

- [player_window_options.cpp](../src/ui/player_window_options.cpp)：`SetHotKeyControl`、`GetHotKeyControl`；252 页的“设置”直接读取两只标准控件，没有 Backspace 捕获补丁。
- [settings.cpp](../src/settings/settings.cpp)：`ParseHotKeyBinding`、`HotKeyBindingText` 保存按键数值和修饰位，没有将键值 8 禁用。
- [player_window_playlist.cpp](../src/ui/player_window_playlist.cpp)：应用内快捷键在消息过滤器匹配按键与 Ctrl／Shift／Alt；没有 Backspace 黑名单。
- [player_window.cpp](../src/ui/player_window.cpp)：`RegisterConfiguredHotKeys` 将键值传给系统；当前与原版一样忽略注册失败，属于另一个可改善点，不能解释本次录入值为零。

## 建议实现

1. 保留原资源、配置结构和标准控件，仅对子控件增加消息处理。
2. 有 Ctrl 修饰的 Backspace 作为可绑定组合捕获，调用 `HKM_SETHOTKEY` 设置；处理重复按下、字符消息和松键，避免默认流程再次清空它。
3. 裸 Backspace 保留清除当前绑定的行为；Tab、Esc 等对话框操作不顺带改变。
4. 应用内、全局两只控件一致处理；验证点击设置、切换行、保存重开后值不丢失。
5. 全局注册失败单独给出可理解的提示。绑定到全局后会影响其它应用中 Ctrl + Backspace 的删除词语操作，因此保留用户显式选择，不加入默认全局键。

这属于在原版录入能力之上的兼容扩展，不应描述为“原版已有该输入处理而重建版漏抄”。本轮未重新测试 XP／Win7 的交互桌面；后续实现应覆盖这些系统，以及文本编辑焦点与快捷键重复触发行为。

独立测试代码及资源快照位于本地 `rebuild/tests/ui`，不加入发行包或 Actions。
