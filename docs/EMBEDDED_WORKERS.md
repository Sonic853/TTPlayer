# 移除旁侧辅助 EXE 的运行时依赖

2026-09-11。

## 原因与范围

上一轮只修复了文件属性和 metadata 共用客户端。选项窗口仍用
`FindRuntimePath` 寻找 `ttplayer_dsp_probe.exe` 和
`ttplayer_output_device_probe.exe`。仅复制播放器 EXE 时，DSP 扫描/配置启动失败，
完整设备快照不可用；设备页回退到有限目录，因此容易误以为程序其余功能都正常。
三个辅助 EXE 也都是 `ttplayer_rebuild` 的构建依赖。

本次检查了 `src`/`include` 下进程创建、Shell 调用以及 EXE 路径常量。
播放器现在没有额外的同目录 EXE 运行依赖，也不调用原版 `TTPlayer.exe`。
保留 Windows 的资源管理器、默认应用设置和浏览器等系统 Shell 功能；
它们不是需要随播放器复制的辅助程序。

## 实现

- `main.cpp` 在加载 ttpcomm/ttpres、建立单实例 IPC、读取设置和创建窗口之前，
  分派三个私有模式：`--ttplayer-file-info-worker`、`--ttplayer-dsp-worker`、
  `--ttplayer-output-device-worker`。识别到模式后，即使参数无效也直接返回错误，
  不能继续启动另一个播放器或向已运行实例转发命令。
- `src/app/file_info_worker.cpp`、`dsp_worker.cpp`、`output_device_worker.cpp`
  链入主程序，开发用独立 probe 仅保留薄入口并复用相同实现。
- `worker_process.h` 统一取得当前 EXE 的绝对路径和 Windows 参数转义。
  不通过当前工作目录、PATH、固定播放器文件名或旁侧旧辅助程序定位工作进程。
  支持主程序改名、中文/空格路径及参数结尾的反斜杠。
- DSP 扫描内部的逐 DLL 子进程也携带私有模式，否则会错误启动播放器 UI。
  保留 1500 ms 单插件超时、合法结果保留、扫描 Job、取消以及配置回调原有 ABI。
- 音频设备保持原有枚举顺序/设备键、独立 COM 初始化和选中设备能力探测协议；
  保留 4000 ms 等待及故障后的最多 1000 ms 回收。未将它改为异步设备页刷新。
- metadata 的调用处不再拼接旧辅助 EXE 文件名；空 helper 表示使用自身工作模式。
  原有读写协议、Job、超时、取消和封面路径不变。
- 取消播放器目标对三个 probe 的 `add_dependencies`，probe 标记为
  `EXCLUDE_FROM_ALL`。完整开发构建中的测试仍可按需构建这些工具，发布时无需携带。

与原版一致的是不额外依赖旁侧 EXE 的部署方式，以及本次未改变的插件/设备数据契约。
为防旧 DLL 卡死主窗口，重建版仍使用自身 EXE 的隔离子进程；这不是声称原版也有
这些私有参数或进程结构，更不是逐字编译执行 Ghidra 伪代码。
`ttpcomm.dll`、`ttpres.dll`、AddIn、皮肤等原运行资源仍需保留。

## 验证

- Release 完整构建成功；CTest **27/27** 通过（17.79 秒）。
- `portable_worker_tests` 将仅有的主 EXE 复制并改名为中文/空格路径，使用不同 CWD。
  无配置、无插件目录、无播放器 DLL 时，三个模式的缺参数请求直接返回；
  内建 MP3 标签成功读出；DSP 配置回调收到 owner/HMODULE；
  设备目录的 8 项顺序、键、名称、模块、详情以及选中 waveOut 详情与独立入口一致。
  私有模式未生成 `TTPlayer.xml` 或 `PlayList`。
- `embedded_dsp_probe_tests` 用无辅助 EXE 的改名副本逐个探测合法、旧版本和卡死 DLL，
  仅接纳合法 DLL，扫描约 1.62 秒完成。这同时检查了扫描进程的下一级私有分派。
- 宿主机 `tools/probe_portable_file_properties.ps1 -CheckOptions` 使用
  `D:\Programs\TTPlayer` 的 DLL 副本和提供的 FLAC 副本，不改原文件。
  只复制一个 `Portable Player.exe`，不复制任何辅助 EXE，CWD 为 `%TEMP%`：
  DSP 页约 1712 ms 得到 1 项（另一个卡死插件被拒绝），期间主窗口响应消息；
  设备页约 328 ms 得到 8 项，两页均正常关闭；
  FLAC 封面显示及六种文件属性关闭检查全部通过（关闭约 60–77 ms）。
- 宿主 UI 报告：
  `%TEMP%\TTPlayer-portable-properties-91d8df1635f541c48cc9abb69315ed25\options-report.json`
  及同目录 `report.json`。
- 主程序生成的 VS 项目仅引用 `ZERO_CHECK`、`ttpcomm_api`、`ttplayer_core`、
  `ttpres_resources`，不再引用三个 probe 项目。

测试源码/脚本沿用现有 `tests`/`tools` 忽略规则；生产 worker 源码均位于受版本管理的
`src/app` 和 `include/ttplayer/app`，没有依赖被忽略目录中的实现代码。

## 交付

已更新 `rebuild/build/Release/ttplayer_rebuild.exe` 和
`D:\Programs\TTPlayer\ttplayer_rebuild.exe`（2388480 字节），两者 SHA-256 均为
`73E0DCA97CC47B94B181D7A476A56C82B3BB13A3755CB96B6BF5663FC909CD06`。
目标目录旧版备份为 `ttplayer_rebuild.exe.before-embedded-workers-20260911-055954.bak`。
未删除目录内旧工具、替换 DLL 或改写目标目录配置/歌曲；构建前备份的 27 个 Release
配置和列表文件已经恢复并逐个验证 SHA-256。
