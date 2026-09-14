# 复制到原版安装目录后的文件属性故障

2026-09-11，目标目录：`D:\Programs\TTPlayer`。

## 已确认的两个原因

1. 目标目录中的旧重建 EXE（2026-09-07，2284032 字节）依赖旁侧的
   `ttplayer_file_info_probe.exe`。目标目录没有此文件，`RunFileInfoProbe`
   因启动失败直接返回，属性页只剩播放列表已有的字段，`cover` 始终为空。
   将辅助程序补进同一测试副本后，原 EXE 已能读取 FLAC 封面，说明不是安装目录名、
   当前工作目录或该目录 `ttpres.dll` 的封面资源兼容问题。
2. 文件属性使用 `PSH_MODELESS` 创建系统属性页。旧代码只处理 `WM_CLOSE`/
   `IDCANCEL`，没有截获 `WM_SYSCOMMAND/SC_CLOSE`，也没有检测属性页已经结束。
   实测标题栏/Alt+F4 对应的关闭命令执行后，`PSM_GETCURRENTPAGEHWND == 0`，
   但 `IsWindow(sheet) == TRUE`，宿主播放列表仍处于禁用状态。
   Windows 明确要求应用在无当前属性页时自行销毁无模式属性窗口。
   参见 [PropertySheetW 文档](https://learn.microsoft.com/en-us/windows/win32/api/prsht/nf-prsht-propertysheetw)。

这两个问题独立：补上旧辅助 EXE 只能恢复封面，不能修好关闭。

## 修复

- 原辅助程序中的 reader、metadata、封面和标签写入实现迁到
  `src/app/file_info_worker.cpp`，由主 EXE 和原独立辅助 EXE 共享。
- 主程序最早入口识别私有 `--ttplayer-file-info-worker` 模式；它不进入播放器的
  单实例转发、窗口创建、设置加载/保存流程。无效请求直接退出。
- 正常播放器的探测客户端启动自身 EXE 的独立工作进程，不依赖旁侧辅助 EXE，
  也不优先执行可能残留的旧版本辅助程序。EXE 改名后仍由 `GetModuleFileNameW`
  找到自身；插件路径和 `ttpcomm.dll` 路径仍由当前播放器目录提供。
- 保留原来的 Job、超时、取消及包格式校验；没有把可能卡死的旧插件读取移到 UI 线程。
- `SC_CLOSE` 投递统一关闭消息；关闭按钮/键盘命令和异常销毁都会清除接收目标并取消任务。
  隐藏的 OK 按钮不再保持启用，关闭按钮成为默认按钮。
- 模态宿主循环补齐 `PSM_GETCURRENTPAGEHWND` 检查，完成后销毁外层窗口、取消/回收任务，
  恢复原来被禁用的播放列表。

仍需原版运行文件（`ttpcomm.dll`、`ttpres.dll`、`AddIn`、需要的皮肤等）；
本节记录文件属性及共用 metadata 探测的修复；后续同日已将 DSP/输出设备
工作模式也合入主 EXE，详见 [EMBEDDED_WORKERS.md](EMBEDDED_WORKERS.md)。

## 验证与交付

- Release 构建成功，完整 CTest **25/25 通过**（15.78 秒）。
- `builtin_file_info_tests` 同时测试独立和内置工作进程，覆盖 MP3 标签策略、
  UTF-8 标签、PNG 封面写入/回读，以及无效私有请求不会进入 UI。
- 宿主机 `tools/probe_portable_file_properties.ps1` 使用目标目录 DLL 的临时副本，
  EXE 改名为 `Portable Player.exe`，不复制辅助 EXE，工作目录另设为 `%TEMP%`。
  使用用户提供的 FLAC 的副本，不写原歌曲或用户配置。
- 实際切换到封面页，确认图像控件可见且持有解码后的位图；随后检查标题栏关闭命令、
  关闭按钮、Esc、Alt+F4 对应的关闭命令、焦点在关闭按钮时的 Enter，以及
  `PSM_PRESSBUTTON(PSBTN_CANCEL)`。六项均关闭成功，播放列表重新启用（约 58–77 ms）。
  Enter 位于“替换”按钮上时应打开选择器，不将其误判为关闭失败。
- 旧 EXE 无辅助程序的报告：
  `%TEMP%\TTPlayer-portable-properties-369e8addcef64d65a1cce02bf6b0bbb0\report.json`。
- 旧 EXE 补辅助程序的报告：
  `%TEMP%\TTPlayer-portable-properties-4313eac81f684473be50d2b2f5690ebd\report.json`。
- 修复版最终封面页/关闭报告：
  `%TEMP%\TTPlayer-portable-properties-3aada70da1d346baab2366db398b91ff\report.json`。
- 已将修复版同步到 `D:\Programs\TTPlayer\ttplayer_rebuild.exe`，校验与 Release SHA-256 相同，
  并直接验证目标目录 EXE 的私有工作模式能退出且不创建播放器 UI。
  旧 EXE 保留为 `ttplayer_rebuild.exe.before-file-properties-20260911-054516.bak`。
  未替换目标目录 DLL、皮肤、配置或播放列表。Release 构建前备份的 27 个配置/列表文件已恢复并校验。

测试代码和脚本沿用仓库现有的 `tests`/`tools` 忽略规则，未修改 `.gitignore`。
