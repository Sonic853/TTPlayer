# 通用版 SMTC 支持

通用版在 Windows 10／11 上接入系统媒体传输控件（System Media Transport Controls，SMTC）。
歌曲成功打开后，系统媒体面板可显示当前曲目信息并控制播放；不需要额外安装组件或开启选项。
同一 EXE 在 XP／Win7 上跳过 SMTC 初始化，不创建 WinRT 对象。

## 已实现

- 显示曲名、歌手、专辑。优先使用当前解码器返回的标签，其次使用播放条目的标签；没有标题时使用文件名。
- 提供专辑歌手 `AlbumArtist` 和曲风列表 `Genres`，供 Wallpaper Engine 等媒体会话读取方使用。
  专辑歌手优先使用真实标签，缺失时回退到歌曲歌手；曲风缺失时保持空列表。
  支持 `Album Artist`、`ALBUM_ARTIST`、`WM/AlbumArtist`、`TPE2` 等别名，以及
  `Genre`、`Genres`、`WM/Genre`、`TCON` 等曲风字段。多曲风按分号、逗号、NUL 或换行分隔，
  去除首尾空白和重复值，保留 `Pop/Rock`、`R&B` 等名称。
  解码器明确返回的空字段覆盖播放列表旧缓存；字段未提供时才使用当前条目的缓存。
- 显示歌曲内嵌封面，复用任务栏预览已解码、限制尺寸后的位图，不再打开媒体文件读取封面。
  没有封面或封面编码失败时清除上一首的封面。
- 同步播放、暂停、停止状态及上一首／下一首的可用状态。
- 接收播放、暂停、停止、上一首、下一首和进度跳转请求。
  播放与暂停为独立命令，重复请求不会切换状态或重新打开正在播放的歌曲。
- 上一首／下一首使用已有的 `SelectRelative`，继承当前播放模式和媒体库的导航规则。
- 发布总时长和当前位置，播放中约每 5 秒更新；暂停、恢复、跳转和时长变化立即刷新。
  未知时长不提供可跳转范围。
- 空列表启动时不建立媒体会话；关闭文件、播放失败时清理信息；退出时禁用会话并注销事件。
  正常停止保留曲目信息，允许从系统面板重新播放。

系统决定媒体面板的展示位置、时机及具体按钮。当前接入不增加系统面板里的播放速度、随机或循环模式设置。

## 实现与兼容边界

`SystemMediaControls` 使用 Windows SDK 自带的 C++/WinRT 头文件，通过
`ISystemMediaTransportControlsInterop::GetForWindow` 将会话关联到播放器主窗口。
它只负责系统界面与命令转发，音频仍由现有 `AudioEngine` 播放。

WinRT 事件可能来自后台线程。回调仅将请求写入有长度上限的队列并发送窗口消息，
由主线程验证状态、执行命令。队列带歌曲代次，换曲或清理后丢弃旧请求；
销毁时先在锁内撤销目标 HWND，避免延迟回调访问已销毁的播放器。
歌词保存对话框、停止淡出和退出期间不执行新的控制请求。

SMTC 初始化失败不阻止播放。通用构建编入本模块并链接 `runtimeobject`、`shcore`，
由优先链接的 YY-Thunks 对象处理新 API，EXE 不产生对旧系统缺失 WinRT DLL 的启动依赖。
`CurrentWindowsFeatures()` 使用 `RtlGetVersion` 识别实际系统；Win10 以前在入口返回。
Win10 及以后仍需实际创建 SMTC 工厂成功才启用。子系统 5.01 和 XP／Win7 导入审计保持启用。
2026-09-25 的通用构建变更及验证见 [合并构建记录](UNIFIED_WINDOWS_BUILD.md)。

## 本地验证

2026-09-21 补充：兼容版已在 XP SP3 / Win7 SP1 虚拟机通过启动回归和静态导入审计，
见 [虚拟机记录](XP_WIN7_VM_VALIDATION.md)。这验证了排除 SMTC 后的旧系统包，
不代表在旧系统中运行 WinRT/SMTC 功能。

测试源码位于本地 `tests/ui/system_media_controls_tests.cpp`。
`BUILD_TESTING=ON` 时创建该测试目标；Actions 继续使用 `BUILD_TESTING=OFF`。

```powershell
cmake --build build --config Release --target system_media_controls_tests
ctest --test-dir build -C Release -R "^system_media_controls_tests$" --output-on-failure
```

原生测试通过 Windows 的 `GlobalSystemMediaTransportControlsSessionManager` 查找本程序会话并发送命令，
覆盖元数据、内存封面、状态、按钮启用、64 位进度请求、切歌清除旧封面及旧请求、清理与销毁。
播放器侧另验证播放／暂停幂等性、单曲循环的首尾导航及退出／歌词保存期间的命令保护。
该原生测试需要有桌面 Shell 的 Windows 10／11 环境。

2026-09-16 验证：普通版 Release 构建及上述原生 SMTC 测试通过，
任务栏按钮、任务栏封面预览和播放导航三个回归测试通过。
XP／Win7 版 Release 构建通过，18 个 DLL、641 个静态导入通过旧系统导出清单审计，
没有新增 WinRT／SMTC 导入。本次运行验证在当前 Windows 宿主完成，未在 XP／Win7 实机运行。

2026-09-17 验证：新增的可选标签测试通过，覆盖解码器／列表缓存优先级、专辑歌手回退、
多曲风分隔和去重、空值覆盖缓存，以及从 Windows 全局会话实际读取 `AlbumArtist`／`Genres`
和切歌清空；带真实专辑歌手、多曲风标签的 MP3 解码读取测试通过。
Media Foundation 路径同时读取 Windows 的专辑歌手、曲风属性，继续复用解码器打开阶段，
不增加 UI 线程上的媒体文件读取。

## 官方接口依据

- [GetForWindow：为桌面顶层窗口获取 SMTC](https://learn.microsoft.com/en-us/windows/win32/api/systemmediatransportcontrolsinterop/nf-systemmediatransportcontrolsinterop-isystemmediatransportcontrolsinterop-getforwindow)
- [手动控制 SMTC：事件线程、媒体信息与时间轴](https://learn.microsoft.com/en-us/windows/apps/develop/media-playback/system-media-transport-controls)
- [AlbumArtist](https://learn.microsoft.com/en-us/uwp/api/windows.media.musicdisplayproperties.albumartist)
- [Genres](https://learn.microsoft.com/en-us/uwp/api/windows.media.musicdisplayproperties.genres)
