# 主窗口专辑封面闪烁修复

## 原版调用链

`TTPlayer.exe.pseudo.c` 中的封面刷新顺序为：

1. `CPlayerWnd_Run` 在视觉类型为 4 时，仅在 reader 缩略图状态变化时调用
   `FUN_00457BCF` 和 `FUN_004AD83E` 重新取得第 0 张图片；
2. `FUN_00457B11` 在 `CVisualCtrl+0x154C` 临界区内执行
   `FUN_00457C33`，随后取得 `VisualCtrl` 的 DC；
3. `FUN_00457E2D` 先恢复视觉区域的皮肤背景，再由 `FUN_00457D90`
   绘制已解码图片或资源字符串 `0x821B`。

因此，图片资源替换、渲染状态和视觉绘制必须串行；主窗口的定时刷新也不能把
`VisualCtrl` 已提交的内容覆盖掉。

## 闪烁原因

重建版此前存在两个额外的可见中间状态：

- 封面模式先把缓存背景 `StretchBlt` 到窗口 DC，再把 WIC 位图
  `AlphaBlend` 到同一个窗口 DC。视觉线程按 `FramesPerSec` 重复执行时，DWM
  有机会采样到两次 GDI 操作之间的纯背景；
- 主窗口每 250 ms 生成整张皮肤缓冲并覆盖整个父窗口。由于该父绘制未排除
  独立的 `VisualCtrl` 子窗口，封面会被父窗口背景短暂覆盖，直到视觉线程再次
  绘制。

互斥锁只能保护 C++ 对象，不能把两次屏幕 DC 操作合并为一次显示提交，也不能
阻止另一个 HWND 的父窗口绘制。

## 修复

- 类型 4 先在 `VisualRuntime` 已有的持久 32 位 DIB 中恢复背景，然后在该 DIB
  上完成 WIC `AlphaBlend`、OLE `IPicture::Render` 或无封面文字绘制，最后只用
  一次 `StretchBlt` 提交完整帧；
- 主皮肤最终传送到屏幕 DC 前，排除仍由主窗口拥有且可见的 `VisualCtrl`
  矩形；全屏分离或隐藏状态不排除；
- WIC 仍是第一解码器，只有 WIC 失败才调用 `OleLoadPicture`。reader 的
  `ISoundThumbnail`、第 0 张图片、缓存失效、点击命中和菜单行为均未改变。

## 插件皮肤中的封面比例修复

主窗口复用 `VisualRuntime` 时，其分析缓冲仍按原生皮肤的 `visual` 尺寸初始化，
插件则通过 `PaintSkinPluginVisual` 传入自身的主窗口、播放列表或折叠区域。此前
`PaintCover` 在分析缓冲中完成等比例缩放后，又整体 `StretchBlt` 到插件区域；
两处宽高比不同就会再次横向或纵向缩放封面。像素回归复现了正方形封面在
`72 × 16` 区域被压成 `15 × 16` 的情况。

现在普通封面按最终绘制区域的尺寸，在独立的 32 位缓冲中恢复背景并合成图片，
再一次 `BitBlt` 提交完整帧。保持原版 `FUN_00455175` 的居中、等比例缩小、
小图不放大策略；WIC/GDI+ 位图与 OLE 回退使用同一目标尺寸。无封面提示的绘制
和命中矩形也直接使用目标坐标。全屏专辑背景仍沿用自身的裁剪与透明度逻辑。

缓冲使用 WTL 管理 GDI 资源，容量足够时复用，避免主窗口与播放列表交替刷新时
重复分配；切换视觉类型或重新配置时释放。修复位于重建版宿主，无插件 ABI 改动。

本地回归位于 `tests/waskin/host_tests.cpp` 的 `--visual-only` 分支，覆盖正方形、
横图、竖图、小图、折叠区、双倍尺寸的 DC 映射、区域裁剪、暂停和反复切换尺寸，
并检查原生／插件皮肤切换。测试不进入分发包或 Actions。

## Windows Sandbox 验证（原闪烁修复）

运行：

```powershell
.\rebuild\tools\windows_sandbox\run_cover_sandbox.ps1
```

隔离环境使用默认皮肤，将参考 FLAC 的异常封面记录仅在临时副本中转换为原版和
重建版都能解码的 640x640 JPEG。探针先记录无视觉背景哈希，再以约 20 ms 间隔
持续采集 12 秒的 `VisualCtrl`：

| 运行时 | 捕获帧 | 唯一封面帧 | 背景闪回帧 | 闪回转换 | 结果 |
| --- | ---: | ---: | ---: | ---: | --- |
| 原版 | 384 | 1 | 0 | 0 | 通过 |
| 重建版 Debug | 385 | 1 | 0 | 0 | 通过 |

报告由 Sandbox 账户 `WDAGUtilityAccount` 写出，位于
`build/sandbox-cover/20260904-154727/cover-flicker-report.json`。
