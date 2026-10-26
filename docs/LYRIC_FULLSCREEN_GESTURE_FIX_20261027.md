# 歌词秀双击恢复全屏效果修复

## 1. 现象与原因

复现流程：使用「节拍星环」后退出全屏，在歌词秀双击再次进入，结果显示「星环 lazer」。

`FullScreen/LastMode` 和 `FullScreen/LastVisualEffect` 的读取、保存原本正常。问题发生在一次双击的最后一条鼠标消息：

1. 歌词窗口收到按下、松开、双击消息。
2. `HandleLyricDoubleClick` 恢复上次全屏模式和效果，例如效果 7「节拍星环」。进入时歌词窗口隐藏，视觉窗口脱离主窗口并扩大。
3. 鼠标位置下的窗口随布局变化，第二次松开的 `WM_LBUTTONUP` 可能交给视觉窗口。
4. 视觉窗口原有的单击切换逻辑直接处理每一次松开消息，调用 `InvokeVisualAction` → `SetVisualType`。效果 7 因而立即变为 8「星环 lazer」，同时改写全屏历史。

修复前新增的完整消息序列回归可稳定触发 `lyric double-click trailing release replaced restored fullscreen effect`。仅发送 `WM_LBUTTONDBLCLK` 的旧测试不能发现最后一步。

## 2. 修复

- 成功通过歌词双击进入或退出全屏时，记录尚未结束的这一次鼠标手势；带退场动画的退出也记录。
- 统一消费它剩余的一次松开消息，覆盖 WTL 消息过滤器、视觉窗口、歌词窗口/控件、原生组合内容及皮肤插件内容回调。
- 如果旧手势在其他程序中结束，下次按下或双击会清除旧标记，不影响新的单击。
- 不使用固定延时；第二次按下后长时间按住再松开也属于原来的双击。
- 未启用歌词双击全屏、播放状态不允许进入等情况不会留下消费标记。

全屏历史、正常单击切换规则、普通窗口效果、可选插件的缺失回退逻辑保持原有设置。改动位于播放器主程序，不需要更新 `ttp_osu.dll`。

## 3. 验证

Windows 11 本机完成：

- `options_drawing_tests --fullscreen-effect-memory`：20 组普通/迷你模式、视觉全屏/歌词同屏、频谱/封面/水波/两种星环组合；完整按下、松开、双击、跨窗口松开序列；确认恢复效果及历史不变，下一次主动单击仍切换。
- 同一测试覆盖歌词外框、原生组合歌词、皮肤内容回调、直接 HWND 消息与 WTL 过滤路径、双击退出、在其他窗口松开后开始新手势，以及历史保存/重启。
- 不包含 `ttp_osu.dll` 的隔离目录：12 组普通效果组合和原有可选插件缺失回退检查通过。
- `fullscreen_lyric_drag_tests --messages-only`：拖动定位、取消/捕获丢失、双击菜单和开关、正常/迷你模式、透明歌词输入代理及退出检查通过。同步补齐旧测试夹具所需的 `ttpcomm.dll` 和扩展资源加载。
- Release 构建及 XP/Win7 静态导入审计通过；ZIP 内主程序字节与 Release 文件一致，内部 SHA256 校验通过，测试文件没有进入 ZIP。

原有真实鼠标 `SendInput` 测试曾尝试运行，但共享桌面的实际鼠标位置与预期位置不一致，未完成，因此不计入通过结果；本轮使用确定性的 HWND 消息回归验证输入逻辑。没有进行 XP/Win7 虚拟机实测。

测试和日志只保存在私有 `tests` 下，不加入 Action、不上传：

- `tests/ui/lyric_fullscreen_effect_gesture_tests.inc`
- `tests/fullscreen_gesture_20261027/before.log`
- `tests/fullscreen_gesture_20261027/effect-memory.log`
- `tests/fullscreen_gesture_20261027/no-osu.log`
- `tests/fullscreen_gesture_20261027/lyric-input.log`
- `tests/fullscreen_gesture_20261027/lyric-input-native-attempt.log`
- `tests/fullscreen_gesture_20261027/release-verification.json`

## 4. 发行文件

沿用当前本地配置的版本 `2026.10.26`，更新 `build/Release` 的主程序、发行 ZIP 和 `SHA256SUMS.txt`。

| 文件 | 字节数 | SHA256 |
| --- | ---: | --- |
| `TTPlayerRebuild.exe` | 4441088 | `7501f18e0f1ada1ed048d76043b155b1a903367ed329344914ff5da1c7705675` |
| `TTPlayerRebuild-2026.10.26.zip` | 3441419 | `2236b9c7c54e579439417597f723526d4ff994840d2e17d9a9b4833f4e3c0d5f` |

替换主程序并重启后，可重新选择一次「节拍星环」，退出后在歌词秀双击验证。若旧版本已把历史改成 lazer，修复不会猜测或强制改写用户最后保存的选择。
