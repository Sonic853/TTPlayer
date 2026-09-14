# 换肤时的窗口生命周期与重绘边界

## 5.7.9 伪代码依据

`reverse/decompiled/TTPlayer.exe.pseudo.c` 的 `0046D0C1` 主窗口绑定顺序是：

1. 记录原有 `WS_VISIBLE`，仅对原先可见的窗口发送 `WM_SETREDRAW(FALSE)`。
2. 更新背景与控件绑定。
3. 发送 `WM_SETREDRAW(TRUE)`，然后才 `SetWindowPos(..., 0x16)`。
4. 更新窗口区域、位置并使最终内容失效重绘。

`00468363` 在主窗口绑定返回后，再分别绑定歌词、均衡器和播放列表。
这里没有销毁／创建主 HWND，也没有用 `ShowWindow(SW_HIDE/SW_SHOW)` 模拟普通换肤。
迷你模式转换 `00464B6C` 中明确存在的隐藏／显示不属于本次删除范围。

## 确认的差异和修正

重建版的主窗口 `ScopedSkinRedraw` 原先覆盖整个 `ApplyLoadedSkin`，包括尺寸、区域、
附属窗口绑定以及新增的置顶同步。标准 `WM_SETREDRAW(FALSE)` 会临时清除 `WS_VISIBLE`，
因此这些操作会检查、调整一个暂时不可见的 owner，而不是原版此时已经恢复可见的主窗口。
以前只检查换肤前后的 HWND、`WM_SHOWWINDOW` 和销毁事件，不能发现这个中间状态错误。

新增的原生消息回归在修复前报告：
`skin geometry applied before WM_SETREDRAW(TRUE), unlike 0046D0C1`。

现在将恢复重绘移到首次原生尺寸／区域操作之前，并让这个恢复步骤不提前使整组窗口
失效；完成绑定后仍由已有的末尾 `RedrawWindow` 提交最终内容。保留上一轮选项窗口、
附属窗口的置顶规则，不靠销毁 HWND、清除 `WS_EX_LAYERED` 或额外隐藏／显示窗口换肤。

需要区分事实与推断：修复前在默认皮肤与 TT2012 的实机右键往返对照中，原版和重建版
均未丢失原有 HWND，也未收到主 HWND 的创建／销毁／显示／隐藏事件。因此本次确认的是
可见性／重绘时序不一致，不能把用户看到的“像重建一样刷新”直接断言成已经捕获到
`DestroyWindow`。对于测试覆盖之外的具体皮肤组合，仍需用相同的生命周期观测验证。

## 回归

将换肤测试纳入受 Git 跟踪的 `src/ui/skin_rebind_tests.cpp`，CMake 使用该文件：

- 以 HWND 属性标记验证窗口没有被销毁后复用句柄；保留原有 40 个窗口／控件。
- 禁止在主窗口暂停重绘期间调整主窗口尺寸、Z 顺序或开始附属窗口的重绘事务。
- 覆盖默认皮肤、LX-iPlay、TT2012、Let's Vista；本机存在时还覆盖 Classic、
  DEFAULT_SKIN_579、DEFAULT_SKIN__6120 和 `Skin/new/BaiduMusic8209.skn`。
- 保留播放列表选择、歌词编辑器未保存内容、选区，以及正常／迷你置顶状态。
- 扩展 `window_topmost_tests`：选项窗口跨换肤和模式转换也必须保留同一 HWND 属性标记。

所有配置、播放列表和测试编辑内容均使用独立临时目录，不改写用户原有配置或皮肤包。

2026-09-13：Release 构建通过，十项针对性宿主机回归全部通过（55.77 秒）。
本机上述八种皮肤均执行了生命周期检查，选项窗口身份检查及原有置顶回归也通过。

实际右键菜单对照另外覆盖原版／修正版各三组：默认皮肤与 LX-iPlay、TT2012、
Let's Vista 往返切换（主窗口及歌词均置顶）。六组都保留原有句柄，主窗口没有
创建／销毁／显示／隐藏事件，并正常关闭。

已仅替换 `build/Release/TTPlayerRebuild.exe`，旧 EXE 备份于 `out/png-6120/`。
`TTPlayerRebuild.xml`、旧 `TTPlayer.xml` 和 `Skin/Default.xml` 的校验值均未改变。
