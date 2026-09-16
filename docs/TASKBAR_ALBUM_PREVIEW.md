# 任务栏与窗口切换预览显示专辑封面

日期：2026-09-16。这是按用户要求添加的重建版功能。

## 行为

- 当前歌曲有可解码的内嵌封面时，任务栏缩略图和 Peek 窗口预览使用封面，保持宽高比，显示完整图片。
- 暂停保留封面；切到无封面／封面损坏的歌曲、停止或播放失败时取消自定义预览，交回 Windows 显示主窗体。
- 独立于主窗口的视觉效果模式；主窗体显示频谱、梦幻等效果时，预览仍可显示当前歌曲封面。
- 原有上一首、播放／暂停、下一首缩略图按钮保留。最小化时可显示封面；迷你／全屏返回和 Explorer
  重建任务栏按钮时重新应用预览；DWM 合成恢复后也重新应用。
- 原有迷你窗口／托盘模式的任务栏显示规则继续生效。

## 实现

`TaskbarPreview` 在 UI 线程管理图片和 DWM 属性。每次 `PlayCurrent` 成功取得当前 reader 的元数据后装载一次。
预览请求只缩放缓存的图片，不打开文件、不启动网络查询，也不随播放进度反复解码。

封面复用 `player_window_visual.cpp` 的 WIC／OLE 解码器和内嵌图片解析策略：reader 实现
`ISoundThumbnail` 时使用其第 0 张图片，包括尊重空图片结果；没有此接口时使用既有 ID3/APIC／FLAC
解析器。不把 `Folder.jpg`、全屏备用背景或 Shell 通用图标当作歌曲封面。

缓存最长边为 1024 像素；WIC 在输出 DIB 前缩放。提交 DWM 的位图为 32 位 BGRA，透明内容叠在黑色底上。
缩略图画布采用每次 DWM 请求的完整尺寸；横向／竖向图片在画布内等比居中，多余区域填黑。
Peek 使用整个客户区作为画布，避免提交位图的长宽比随封面变化。DWM 复制像素后立即释放临时位图，换曲或停止释放封面缓存。

开启自定义预览时设置 `DWMWA_FORCE_ICONIC_REPRESENTATION` 和 `DWMWA_HAS_ICONIC_BITMAP`，响应
`WM_DWMSENDICONICTHUMBNAIL`／`WM_DWMSENDICONICLIVEPREVIEWBITMAP`。无封面时清除两个属性并使缓存失效，
由系统重新取得主窗口内容。设置属性或提交图片失败时也回退，避免强制显示空白缩略图。

最小化窗口可能返回零尺寸客户区，Peek 优先使用最小化前记录的真实客户区尺寸和偏移。
尚未观察到有效客户区时，才用 `WINDOWPLACEMENT.rcNormalPosition` 扣除非客户区重建尺寸。
缩略图请求宽高分别来自消息的 HIWORD／LOWORD，不额外按 DPI 倍数重复放大请求尺寸。

## 预览尺寸稳定性修复

此前提交位图的尺寸直接取缩放后的封面尺寸，同一个 `200×120` 请求下，横图可能提交 `200×100`，
竖图则提交 `60×120`。现在两者都提交 `200×120` 的画布，分别在中央绘制 `200×100`、`60×120` 的封面，
使画布不再随封面比例变化。对 `200×120 → 320×180 → 150×225 → 200×120` 的请求序列逐次响应，
不复用前一次请求的缩放尺寸。

封面之间的切换现在直接替换图片并更新 DWM 缓存。成功加载新封面前后不再关闭再开启自定义预览属性，
避免切歌过程中混入主窗体比例的预览；确认没有封面或播放失败时才恢复主窗体。

`WM_WINDOWPOSCHANGED`／`WM_SIZE` 会检查客户区大小、偏移、最小化状态和所在显示器，发生变化时使
DWM 缓存失效。相同尺寸的重复通知不重复刷新。最大化后再最小化保留已观察到的最大化客户区，
不再无条件使用代表普通窗口大小的 `rcNormalPosition`。

## 旧系统

所有新增 DWM 接口通过系统目录中的 `dwmapi.dll` 动态解析，不添加新的静态 DWM 导入。
Windows 7 开启桌面合成时支持封面预览；XP 或缺少接口／合成关闭时保留系统原有行为。

接口合同见微软文档：

- [DwmSetIconicThumbnail](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmseticonicthumbnail)：尺寸、32 位位图、DWM 复制及最低 Windows 7 要求。
- [DwmSetIconicLivePreviewBitmap](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmseticoniclivepreviewbitmap)：Peek、客户区尺寸限制及偏移。

## 验证

独立测试 `taskbar_preview_tests` 不需要私有资源、音频设备或真实歌曲：

- 生成横向／竖向 BMP、带 APIC 的文件，验证像素颜色、alpha、尺寸和 reader 优先级。
- 有封面→另一张封面→无封面／损坏封面的切换，允许／禁用 DWM 合成，属性部分设置失败和提交失败。
- 暂停保留；停止、失败、打开中清除；模拟任务栏重建后刷新；源文件删除后仍使用已解码缓存。
- 大图缓存尺寸、重复切换的 GDI 对象数量和提交后位图释放。
- 在启用合成的测试主机调用真实 DWM 接口，验证普通／最小化窗口的缩略图和 Peek 提交。
- 不同封面比例使用相同请求画布、画布内着色区域尺寸及居中留白、切歌时不短暂关闭封面属性。
- 连续变化的缩略图请求、零尺寸请求、窗口缩放后的缓存失效、最小化时换曲、最大化后最小化的 Peek 尺寸。

配置 `-DTTPLAYER_BUILD_PREVIEW_TESTS=ON`，构建 `taskbar_preview_tests`，运行：

```powershell
ctest --test-dir <构建目录> -C Release -R '^taskbar_preview_tests$' --output-on-failure
```

普通版同时回归 `taskbar_playback_tests` 和 `fullscreen_album_tests`。预览测试已移至本地 `tests/`，不上传且不在 Actions 中执行。
兼容构建另做 XP／Win7 静态导入审计；当前主机测试不替代 XP／Win7 实机验证。
本轮 Explorer 鼠标悬停自动化未枚举到缩略图按钮，未计入通过；真实 DWM 接口测试与此分开记录。

尺寸修复后，普通版的预览、任务栏按钮、换肤、全屏封面 4 项回归通过，兼容构建的预览回归通过；
兼容 EXE 的 18 个 DLL／602 项静态导入审计通过，
保持 x86、子系统 5.01。两个 `2026.09.16` ZIP 已重建并核对包内 EXE 与 SHA-256 清单。
