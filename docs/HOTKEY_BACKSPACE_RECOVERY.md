# Backspace 组合快捷键修复

日期：2026-10-02。依据 [原版分析](HOTKEY_BACKSPACE_ANALYSIS.md)，按用户确认，仅扩展 Ctrl、Alt、Shift 修饰键组合。

## 已实现的行为

应用内“快捷键”和“全局键”两个输入框均支持：

| 组合 | 配置中的修饰位 |
| --- | --- |
| Shift + Backspace | 1 |
| Ctrl + Backspace | 2 |
| Ctrl + Shift + Backspace | 3 |
| Alt + Backspace | 4 |
| Alt + Shift + Backspace | 5 |
| Ctrl + Alt + Backspace | 6 |
| Ctrl + Alt + Shift + Backspace | 7 |

按键值仍是 `VK_BACK=8`，沿用现有 `a(按键,修饰位),g(按键,修饰位)` 配置结构。没有新增默认绑定，也不支持 Backspace + A 这类两个普通按键的组合。

- 先按修饰键或先按 Backspace，均可录入上述组合。
- 长按产生的重复消息保持绑定；先松修饰键或先松 Backspace 均不会清空。
- 单独按 Backspace 保留清除绑定的行为。
- 切换输入框、列表行及重新打开选项窗口后，不残留上一轮按键状态。
- 点击“设置”后按原流程保存；Ctrl + A 等普通快捷键仍由系统控件处理。
- Tab、Esc 等对话框键沿用原行为，没有为了 Backspace 而替换整个输入控件。

## 实现方式与原版关系

原版及原重建版都使用 `msctls_hotkey32`。它能通过 `HKM_SETHOTKEY` 存储 Ctrl + Backspace，但默认键盘录入过程会将其清空。本次保留原资源和控件绘制，只对子控件增加 Backspace 相关消息处理。

新增 [hotkey_control.h](../src/ui/hotkey_control.h)：

1. 在 `WM_KEYDOWN / WM_SYSKEYDOWN` 捕获实际 Ctrl／Alt／Shift 状态。
2. 将组合写入原 HotKey 控件，并在值变化时通知父页面。
3. 消化 Backspace 对应的 BS／DEL 字符消息，防止默认处理再次清空值。
4. 处理长按、两种松键顺序、焦点切换及外部设置控件值的状态复位。
5. 控件销毁时解除子类化并释放状态。

[player_window_options.cpp](../src/ui/player_window_options.cpp) 在 DIALOG 252 的控件 2054、2055 上安装该处理。应用内消息匹配和全局注册继续使用既有执行路径。这是扩展原版录入能力，不是声称原版已实现该输入分支。

## 全局注册失败提示

[player_window.cpp](../src/ui/player_window.cpp) 现在检查 `RegisterHotKey` 的返回值，只记录成功注册的 ID。离开快捷键页或通过正常选项关闭流程应用设置时，若有失败，则集中显示组合键名称及 Windows 返回的原因。

失败不清除用户配置；释放占用或更换组合后可以重新应用。进入快捷键页仍注销播放器自己的全局键，避免抢走录入按键。启动和内部清理不额外弹出该提示。

## 测试范围

本地 `tests/ui/hotkey_backspace_tests.inc` 使用实际生产属性页面及消息过滤器，覆盖：

- 两个控件 × 七种修饰组合 × 两种按下顺序 × 两种松开顺序。
- 每组连续十次自动重复及字符消息；裸 Backspace 清除、焦点切换、随后输入普通组合。
- 点击“设置”、切换列表行、保存 XML 后重新加载、关闭并重开页面。
- 七种组合的应用内命令分发及系统全局注册。
- 独立进程的测试窗口占用全局组合，验证失败提示；释放占用后验证注册恢复。

测试使用私有测试配置和线程键盘状态，没有向用户桌面注入按键。测试代码和日志留在 `rebuild/tests`，不上传，不加入发行包或 Actions。

## 实测结果

2026-10-02 使用同一份 Release 专项测试程序运行：

| 环境 | 录入／保存／应用内分发 | 七种全局注册 | 跨进程冲突提示及恢复 |
| --- | --- | --- | --- |
| 本地主机 | 通过 | 通过 | 通过 |
| Windows XP 虚拟机 | 通过 | 通过 | 通过 |
| Windows 7 虚拟机 | 通过 | 通过 | 通过 |

日志为本地 `tests/hotkey-host.log`、`tests/hotkey-XP.log`、`tests/hotkey-Win7.log`。虚拟机通过 Guest Control 运行测试，验证真实控件消息、保存及系统 API，不将其等同于人工键盘操作测试。

XP 的初版测试采用 `RegisterHotKey(nullptr, ...)` 在辅助线程／进程上制造占用，但本次实测仍允许播放器窗口注册相同组合，未形成预期冲突。最终测试改为独立进程创建窗口并绑定组合，三套系统均确实触发注册失败、只提示一次且不记录失败 ID，解除占用后恢复。此处调整的是测试条件，未为播放器增加 XP 特例。

统一 Release 构建成功，主程序的 20 个 DLL／714 个静态导入及更新器的 13 个 DLL／291 个静态导入均通过 XP／Win7 清单检查。

## 发行输出

统一 Release 输出到 `rebuild/build/Release`，发行包名为 `TTPlayerRebuild-2026.10.02.zip`。保持 XP／Win7 静态导入检查和正式构建 `BUILD_TESTING=OFF`。

打包沿用已校验的 HTTPS 组件 2026.09.25；使用隔离暂存目录核对组件哈希，不改动当前运行目录中的插件。压缩包仅包含主程序、更新器、HTTPS 插件及 `SHA256SUMS.txt`。
