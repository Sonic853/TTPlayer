# 高分屏 / 高 DPI 适配评估

评估日期：2026-09-16。源码基线：`7fc9644`。本次检查源码、实际 EXE 清单及隔离启动后的 DPI 状态，没有修改播放器实现、用户显示缩放或用户配置。

## 1. 结论

**当前重建版可在高分辨率桌面运行，但尚未实现原生高 DPI 适配；从工程结构看，可以改造成支持高 DPI 的版本。**

需要区分三个目标：

| 目标 | 当前情况 |
|---|---|
| 在 2K / 4K 桌面启动、使用播放器 | 没有由分辨率本身造成的架构限制；不能据此认定界面已适配高 DPI。 |
| 在 125%～200% 缩放下保持合适尺寸和清晰文字 | 尚未实现自主 DPI 缩放；当前依赖 Windows 对旧程序的兼容处理。 |
| 在不同缩放比例的显示器间移动时即时清晰重绘 | 没有逐窗口 DPI 状态和 DPI 变化处理，尚不支持。 |

Windows 对 DPI Unaware 程序通常按 96 DPI 提供布局环境，再在高缩放显示器上放大结果；这会降低清晰度。高分辨率和高 DPI 不是同一个条件，例如 4K 桌面设为 100% 时，仍然不会自动产生本程序缺失的界面缩放逻辑。[Microsoft DPI 模式说明](https://learn.microsoft.com/en-us/windows/win32/hidpi/high-dpi-desktop-application-development-on-windows)

## 2. 已核对的事实

### 2.1 源码与实际构建一致

`src/app/ttplayer.manifest` 只有 Common Controls v6 和权限声明，没有 `dpiAware`、`dpiAwareness` 或 `gdiScaling`。播放器启动路径没有调用设置 DPI 感知的 API。

已从两个 EXE 的 `RT_MANIFEST / 1` 资源提取合并后的清单，均未发现额外 DPI 声明：

| 文件 | SHA-256 |
|---|---|
| `build/Release/TTPlayerRebuild.exe` | `e2d03b8fd516a7702e1a0992d2340b6caaf067cde459b705fc97a22d17f5ff63` |
| `out/legacy-distribution/Release/TTPlayerRebuild.exe` | `34bad01bf469976743ec879655ca726d4611ef71da4cedd22dba5b6beca47303` |

XP/Win7 构建切换运行库、系统 API 后备实现和导入检查，没有引入另一套 DPI 布局。因此“兼容旧系统”与“适配高分屏”是两项独立能力。

### 2.2 运行时查询

分别在独立目录复制两个 EXE 和原版资源 DLL，以空配置启动后查询，结果一致：

```text
GetProcessDpiAwareness = 0  (PROCESS_DPI_UNAWARE)

TTPlayer_PlayerWnd     awareness=0  dpi=96
TTPlayer_LyricWnd      awareness=0  dpi=96
TTPlayer_PlayListWnd   awareness=0  dpi=96
TTPlayer_EqualizerWnd  awareness=0  dpi=96
```

内置默认皮肤的主窗口实测为 **327×141 像素**。探测线程使用 PMv2 坐标环境读取窗口矩形，播放器进程保持自己的默认 DPI 模式。

当前宿主有两台 1920×1200 显示器，均为 100% 缩放。因此，本次确认了运行模式、清单和布局逻辑；**没有把 150% / 200%、真实 4K 或混合 DPI 的视觉效果当作已实测结果**。无 DPI 感知窗口的 `GetDpiForWindow` 返回 96，也不能据此推断它所在显示器的实际缩放比例。[GetDpiForWindow 定义](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getdpiforwindow)

原始本地探测输出：`out/dpi-audit/manifests.json`、`out/dpi-audit/runtime.json`；探测脚本为 `out/dpi-audit/probe.py`。

## 3. 主要缺口

### 3.1 皮肤像素直接决定窗口和控件大小

- `src/skin/skin.cpp:782`：`window_size_` 直接取背景位图的 `bmWidth / bmHeight`。
- `src/skin/skin.cpp:803`：XML 的 `position` 直接成为控件矩形。
- `src/skin/skin.cpp:823`：四状态按钮的宽高直接取图片宽度的四分之一和图片高度。
- `src/ui/player_window.cpp:3758`：按钮绘制继续采用图片原始帧尺寸。
- `src/ui/player_window.cpp:3802`：鼠标命中判断直接使用这些矩形。
- `src/skin/skin.cpp:1291`：异形窗口区域由原始位图逐像素生成。

这套方式可以复现原版皮肤的像素布局，但缺少“皮肤逻辑坐标 → 当前窗口实际像素”的统一转换。只放大图片会使显示区域与点击区域不一致；只放大窗口会留下小按钮和小字体；只放大字体又会出现裁剪。

以 327×141 的主窗口为例，200% 时合适的显示面积大约是 654×282 像素。当前系统兼容缩放可以把旧画面放到相近面积，但程序没有在该尺寸下重新布局、重新绘制字体。

### 3.2 可调整窗口大小，不等于 DPI 缩放

`DrawResizableSkinBitmap`（`src/ui/player_window.cpp:771`）按九块切分皮肤，用于列表和歌词窗口的拉伸、平铺。它保留原始边角尺寸，仅扩展相应区域，没有按 DPI 放大所有边距、按钮、拖拽区域和字体。

`MakePlaylistGeometry`（同文件 `:927`）将行高写死为 **16**；分组列表的多处滚动和命中计算也使用 16。列表字体虽然允许配置，但没有与 DPI 同步的统一行高。仅增大列表字体可能导致文字被行框裁切。

### 3.3 字体与歌词：已有可利用的基础，尚无完整 DPI 链路

`CreateSkinFont`（`src/ui/player_window.cpp:1028`）将皮肤 `font_size` 直接写入 `LOGFONT.lfHeight`。普通歌词及桌面歌词也主要使用保存的 LOGFONT，而不是按所在显示器重新计算字高。

已有的能力包括：

- 普通、全屏、桌面歌词分别配置字体。
- 全屏歌词按可用宽度检查最长歌词，在需要时缩小字体。
- 透明全屏歌词使用灰度字形覆盖和预乘 Alpha。
- RichEdit 字号通过 `LOGPIXELSY` 在像素与 twip 之间转换。

其中，全屏 AutoFont 是内容适配；Alpha 改进是边缘合成；RichEdit 的换算是字号单位转换。它们都没有实现逐显示器 DPI 变化后的整窗重建。`GetDeviceCaps(LOGPIXELSX/Y)` 的显示 DPI 值也不是逐显示器 DPI 查询。[GetDeviceCaps 文档](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-getdevicecaps)

### 3.4 多显示器全屏支持不等于混合 DPI 支持

当前已有显示器枚举、指定全屏显示器、显示拓扑变化后的重排：

- `player_window_visual.cpp:1908`：全屏布局。
- `player_window.cpp:2586`：处理 `WM_DISPLAYCHANGE`。
- `player_window.cpp:2596`：处理全屏工作区变化。

但没有 `WM_DPICHANGED` 处理，也没有按目标 DPI 重新生成字体、窗口区域、图标及离屏绘制表面的流程。已有的多屏能力可以继续复用，但不能作为混合 DPI 验证已经完成的证据。

### 3.5 对话框有部分基础，图片资源仍需处理

选项和部分编辑界面使用原版对话框模板、对话框单位及 `MapDialogRect`，比完全固定像素的皮肤主窗更容易适配。

不过 `InstallButtonBitmap`（`player_window_options.cpp:1302`）仍按原始位图尺寸建立 ImageList，例如 16×15 的搜索按钮；还存在固定图标尺寸、自绘间距和字体。即使标准对话框布局得到系统帮助，这些自绘内容仍需单独处理。

### 3.6 PNG 支持没有增加图片本身的细节

现有皮肤仍是栅格图片。`SkinImage::Draw`（`src/skin/skin_image.cpp:158`）的 GDI+ 路径使用最近邻插值，以符合原版像素效果；BMP 路径使用 StretchBlt / TransparentBlt。未发现基于目标 DPI 选择 1× / 2× / 3× 资源的机制。

因此：

- 重新按目标 DPI 绘制字体，可以使文字清晰。
- 将旧按钮图片放大，只能得到更大的原始像素或插值结果。
- PNG 的无损和透明能力，不代表它是矢量图片或高分辨率资源。

原版像素皮肤和高清皮肤可以共存。后续可为旧皮肤保留像素风格缩放，同时允许高清资源或可重绘的简单图形；不必修改所有既有皮肤 XML。

## 4. 为什么不能只打开“高 DPI 感知”

启用感知声明会改变 Windows 对应用的缩放责任和坐标处理，但不会自动改写本程序的按钮帧、列表行高、窗口区域及自绘字体。逐显示器模式下，应用需要消费新 DPI 和建议窗口矩形，并更新自己的布局。[WM_DPICHANGED 契约](https://learn.microsoft.com/en-us/windows/win32/hidpi/wm-dpichanged)

若现在只改 manifest，预计主要问题是：

1. 系统停止替主窗放大，原始 327×141 界面在高密度屏上显得过小。
2. 标准控件与自绘皮肤可能采用不同的尺寸处理，产生不一致。
3. 若仅局部补上缩放，按钮命中、列表选中、歌词拖拽定位和窗口吸附容易与画面错位。
4. 保存的像素矩形、LOGFONT 和缓存若再次按新 DPI 相乘，会产生重复缩放。

这些是未完成改造时的风险分析，不是声称当前默认兼容缩放已经逐项发生上述错误。

## 5. 可行的实施路径

建议保留现有 Win32、旧皮肤和 x86 架构，以统一坐标转换逐步改造。

### 第一阶段：建立坐标与字体规则

- 将原皮肤尺寸定义为 96 DPI 逻辑尺寸，保留原始值。
- 每个顶层窗口保存自己的 DPI；窗口、控件、字体和间距通过同一套换算获得实际像素。
- 源图片裁剪仍使用源像素，目标矩形采用目标 DPI；四状态帧边界不能混用这两种坐标。
- 绘制与命中使用一致的边界和舍入规则，窗口区域、拖拽阈值和吸附距离同步处理。
- 明确旧配置迁移规则；保存逻辑尺寸或记录对应 DPI，不直接累乘上次已经放大的数值。

### 第二阶段：整窗重建与窗口间协作

- 在 DPI 变化时统一更新主窗、歌词、列表、均衡器和桌面歌词的字体、布局、区域、工具提示及绘制缓存。
- 处理窗口组吸附、从属窗跨屏、迷你切换、全屏进入/退出和位置恢复。
- 字体直接按目标大小渲染；图片按资源类型选择像素风格或平滑缩放。
- 保持 100% 时现有绘制和交互回归通过。

### 第三阶段：按系统能力启用模式

现代系统可以采用 Per-Monitor V2；旧系统使用启动时的系统 DPI 布局，并保留兼容后备路径。manifest 可以同时声明旧系统支持的模式和较新系统支持的模式，但声明必须建立在布局已实现的基础上。[默认 DPI 模式与版本回退](https://learn.microsoft.com/en-us/windows/win32/hidpi/setting-the-default-dpi-awareness-for-a-process)

不能把 `GetDpiForWindow`、`GetSystemMetricsForDpi` 等较新 API 变成 XP/Win7 版的强制静态依赖。应复用 `optional_windows_api` 的动态解析方式，并继续执行现有 XP/Win7 导入检查，避免再次出现类似 CreateFile2 的入口缺失问题。`GetDpiForWindow` 的最低客户端为 Windows 10 1607。[API 系统要求](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getdpiforwindow)

### 第四阶段：图像质量及性能

为主要皮肤提供更高分辨率资源，或重绘适合矢量化的简单图形。现有歌词文本不必为了高 DPI 全部改用另一套 UI 框架。

4K 的一张 32 位全屏表面约占 31.6 MiB。透明歌词、背景、字形遮罩可能同时持有多张表面，因此应按尺寸/DPI 缓存并控制重建，避免在高频歌词定时器里反复分配。x86 不是不能适配，但内存和 GDI 对象用量需要纳入验证。

## 6. 临时使用方式的边界

保持系统兼容缩放可以优先保证界面尺寸；手动调整歌词字体能够改善阅读，但不会同时放大主窗按钮和列表行高。

“系统（增强）”可能改善部分 GDI 文字，但它不是本程序已支持高清的证明。本程序同时使用 DIB、离屏位图、GDI+ 及分层透明窗口，实际效果需要逐界面检查。微软对增强 GDI 缩放的说明特别列出了 DIB 与离屏位图的限制，也建议比较启用前后的实际结果。[增强 GDI 缩放及限制](https://blogs.windows.com/windowsdeveloper/2017/05/19/improving-high-dpi-experience-gdi-based-desktop-apps/)

不建议将“应用程序负责缩放”的兼容覆盖当作现版通用修复方案：它可能暴露尚未缩放的原始像素布局。

## 7. 高 DPI 验收要求

本次没有替代以下后续测试：

| 维度 | 应覆盖场景 |
|---|---|
| 缩放比例 | 100%、125%、150%、175%、200%、250%；特别关注非整数比例。 |
| 显示器 | 单高 DPI 屏；100% 与 150%/200% 混用；副屏位于左侧或上方。 |
| 生命周期 | 在各屏启动、跨屏移动、运行时改缩放、断开/重连屏幕、重启恢复位置。 |
| 皮肤 | 默认、迷你、BMP 色键、PNG Alpha、可拉伸歌词/列表窗口。 |
| 操作 | 播放/音量拖动、歌词拖动定位、列表命中和滚动、窗口吸附、菜单/提示位置。 |
| 全屏与透明 | 不同显示器全屏、透明歌词边缘、桌面歌词点击穿透、退出恢复。 |
| 系统兼容 | 现代版 DPI 功能验证；XP/Win7 构建导入检查及对应系统实测。 |

**综合判断：适配是可行的，但当前完成的是旧版界面重建及部分显示能力增强；完整高 DPI 支持需要覆盖皮肤布局、字体、输入坐标和窗口生命周期的一轮改造。**
