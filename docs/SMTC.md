# 普通版 SMTC 支持

普通版在 Windows 10／11 上接入系统媒体传输控件（System Media Transport Controls，SMTC）。
歌曲成功打开后，系统媒体面板可显示当前曲目信息并控制播放；不需要额外安装组件或开启选项。
XP／Win7 版不编译此模块，也不链接新增的 WinRT 库。

## 已实现

- 显示曲名、歌手、专辑。优先使用当前解码器返回的标签，其次使用播放条目的标签；没有标题时使用文件名。
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

SMTC 初始化失败不阻止播放。只有普通版链接 `runtimeobject`、`shcore`，
兼容版通过 CMake 条件和 `TTPLAYER_LEGACY_WINDOWS` 条件编译排除该功能。
不改变兼容版的子系统版本或现有导入检查。

## 本地验证

测试源码仅位于被 Git 忽略的 `tests/ui/system_media_controls_tests.cpp`。
`BUILD_TESTING=ON` 且为普通版时才创建该测试目标；Actions 继续使用 `BUILD_TESTING=OFF`。

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

## 官方接口依据

- [GetForWindow：为桌面顶层窗口获取 SMTC](https://learn.microsoft.com/en-us/windows/win32/api/systemmediatransportcontrolsinterop/nf-systemmediatransportcontrolsinterop-isystemmediatransportcontrolsinterop-getforwindow)
- [手动控制 SMTC：事件线程、媒体信息与时间轴](https://learn.microsoft.com/en-us/windows/apps/develop/media-playback/system-media-transport-controls)
