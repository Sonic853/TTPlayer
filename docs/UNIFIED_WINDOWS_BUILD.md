# 通用 Windows 构建：调整清单与验证记录

日期：2026-09-25。

## 1. 完成目标

将普通版与 XP／Win7 兼容版合并为**一份 x86 Release EXE、一个发行 ZIP**。
所有构建统一使用 **VC-LTL 5.3.1 + YY-Thunks 1.2.2**，同一个程序根据实际运行系统
选择功能。新系统保留 SMTC、现代文件窗口和三轮随机播放等能力，旧系统使用对应后备实现。

| 项目 | 合并前 | 合并后 |
| --- | --- | --- |
| CMake 构建 | 普通版、`TTPLAYER_LEGACY_WINDOWS=ON` 兼容版 | 默认即通用版，固定 Release |
| 主程序运行库 | 普通版依赖现代 MSVC/UCRT；兼容版使用系统 CRT | 全部 `/MT` + VC-LTL，使用系统 `msvcrt.dll` |
| XP API 适配 | 仅兼容版链接 YY-Thunks | 全部优先链接 XP thunk 对象 |
| SMTC | 普通版编入，兼容版排除 | 全部编入，Win10+ 初始化，旧系统入口直接返回 |
| 随机播放与缓存 | 编译宏决定，即使换系统运行也不变 | `RtlGetVersion` 检测实际运行系统后选择 |
| Actions | 编译两遍、生成两个 ZIP | 编译一次、生成一个通用 ZIP |
| Release 附件 | 两个 ZIP 加外层校验文件 | 通用 ZIP 加外层校验文件 |

不再新增 `TTPlayerRebuild-XP-Win7-版本号.zip`。历史 XP-Win7 包保留，当前日期的通用包和 EXE 更新为本次构建产物。

## 2. 各系统功能

| 功能 | XP SP3 | Windows 7 | Windows 10／11 |
| --- | --- | --- | --- |
| 同一 EXE 启动、插件 ABI | x86 | x86 | x86，运行于 32 位兼容环境 |
| 打开／保存文件／选择文件夹 | 经典 WTL/Win32 对话框 | 优先现代 `IFileDialog` | 优先现代 `IFileDialog` |
| SMTC、系统媒体面板 | 不初始化 | 不初始化 | 实际 SMTC 工厂可用时启用 |
| AlbumArtist、Genres、封面与媒体按键 | 无系统 SMTC 会话 | 无系统 SMTC 会话 | 保留已有发布及控制实现 |
| 随机播放内部索引 | 始终单份洗牌索引循环 | 始终单份洗牌索引循环 | ≤5000 首三轮滚动，>5000 首单份洗牌索引循环 |
| 歌曲信息缓存上限 | 128 项／4 MiB | 128 项／4 MiB | 512 项／16 MiB |
| WASAPI 共享／独占 | 系统不支持 | 按音频端点和接口能力启用 | 按音频端点和接口能力启用 |
| WaveOut／DirectSound | 保留 | 保留 | 保留 |
| 任务栏按钮、封面预览 | 不提供 Win7 任务栏 API／DWM 功能 | 按接口及 Aero 合成状态启用 | 按接口及合成状态启用 |
| 专辑封面解码 | WIC 可用时使用，否则 GDI+ 后备 | WIC，失败时 GDI+ 后备 | WIC，失败时 GDI+ 后备 |
| 系统文件关联 | 当前用户关联路径 | 当前用户关联路径 | 系统默认程序确认流程 |
| Media Foundation | 不可用时走内置 PCM、Windows Media Format、AddIn 等后备路径 | 按组件是否存在选择 | 按组件是否存在选择 |

说明：

- **单份随机索引循环**是一次洗牌后沿该随机顺序反复循环，不是按界面歌曲行号顺序循环；
  保留此前用户指定规则。歌曲数、来源或行映射变化仍会触发索引重建。
- Win8／8.1 按新系统的随机索引及缓存策略运行，但不启用本项目的 Win10 桌面 SMTC 实现。
  Vista 按旧系统内存策略运行。上述两类系统本次没有虚拟机验证。
- 系统版本仅决定产品策略。文件窗口、输出设备、DWM、图像及媒体组件继续检查真实接口是否存在，
  不以版本号替代接口检查；精简系统或缺少驱动时不会凭版本号保证功能可用。
- `IFileOpenDialog`、`IFileSaveDialog` 创建失败时保留原有经典窗口后备。
  用户取消现代窗口不会再弹出经典窗口。
- 不改动音频淡入淡出、歌词、文件属性、播放模式规则等既有实现。

## 3. 源码调整

### 3.1 系统版本与统一策略

新增：

- `include/ttplayer/platform/windows_features.h`
- `src/platform/windows_features.cpp`

`CurrentWindowsFeatures()` 缓存一次实际系统版本。通过 `ntdll.dll!RtlGetVersion` 动态查询，
避免 `GetVersionEx` 受兼容性清单影响。版本查询失败时使用保守策略：不进入 WinRT，
选择单份随机索引和 128 项／4 MiB 缓存。

`WindowsFeatures` 集中提供 SMTC 启用条件、随机播放三轮数量阈值、缓存条目和字节上限。
不读取用户配置，不修改系统设置。

### 3.2 SMTC 保留与旧系统保护

- `CMakeLists.txt`：通用核心库始终编译 `system_media_controls.cpp` 并链接 SDK 接口库。
- `player_window.h/.cpp`、`player_window_taskbar.cpp`：去除按发行版本排除 SMTC 的条件编译。
- `system_media_controls.cpp`：`SetSource` 先判断实际系统；Win10 以前不创建 WinRT 字符串、工厂或对象。
- Win10+ 仍捕获初始化／更新失败，不能因此中断普通播放。
- 保留元数据、AlbumArtist、Genres、内存封面、播放状态、时间轴、媒体命令及过期回调保护。
- C++/WinRT 是 SDK 头文件依赖，不新增需要用户安装的 SMTC 运行库。

### 3.3 随机播放及信息缓存

- `playback_order.h/.cpp`：原编译期常量替换为运行时 `ThreeRoundLimit()`。
  XP／Win7 阈值 0，Win8+ 阈值 5000；后台生成、跨轮回退、失效和取消机制保持原有实现。
- `file_info_probe_client.cpp`：每个信息读取会话按系统初始化缓存容量；保留工作进程复用、LRU 淘汰、
  文件指纹失效、取消与超时处理。

### 3.4 构建与兼容性约束

- 所有播放器构建包含 `cmake/legacy_windows.cmake`，内部复用 `xp_runtime.cmake`。
  历史文件名保留，含义已改为通用构建。
- 强制 Release：VC-LTL 的 XP 运行库使用发布配置，Actions 移除 Debug／RelWithDebInfo 选项。
- 保留现代 MSVC、C++20、WTL 10.01 与 ATL；不安装或依赖 v141_xp。
- `_ATL_XP_TARGETING` 对所有播放器目标启用。
- 链接参数保留 `/MT`、`/OSVERSION:5.1`、`/SUBSYSTEM:WINDOWS,5.01`；
  YY-Thunks 对象在系统导入库之前链接，确保缺失的新 API 由兼容入口处理。
- 全部构建保留 `ttpcomm.dll!#3` 启动导入，让 XP 为原版通信 DLL 正确初始化 TLS。
- 每次链接后强制 XP／Win7 导入表审计，不能只修改 PE 版本头掩盖新 API 依赖。
- `TTPLAYER_LEGACY_WINDOWS=ON/OFF` 作为旧命令迁移输入，仅输出提示，均构建通用版。
- `ttp_aac`、其它独立插件仍由各自项目与 Actions 构建，不并入播放器工作流。

## 4. Actions 与包内容

`.github/workflows/manual-build.yml`：

1. 保留北京时间日期与同日 `pN` 版本分配逻辑，编译前固定最终版本。
2. 只配置和构建一次 x86 Release，`BUILD_TESTING=OFF`、`TTPLAYER_STAGE_RUNTIME=OFF`。
3. 构建成功须同时通过 XP／Win7 导入审计；打包前验证 EXE 作者、说明和版本资源。
4. `cmake/package_player.ps1` 核对架构、子系统、两个系统导出清单、`ttpcomm.dll!#3` 及 EXE 哈希。
5. 生成 `TTPlayerRebuild-版本号.zip`，包内**只有**：

   ```text
   TTPlayerRebuild.exe
   SHA256SUMS.txt
   ```

6. Artifact 外层 `SHA256SUMS.txt` 只记录这个 ZIP；`build-info.json` 标记 `unified=true`。
7. GitHub、Gitee 发布各上传通用 ZIP 与外层校验文件，共两个附件。
   两个平台的独立发布、并发锁、版本防冲突和禁止覆盖已有版本逻辑保留。
8. `package_legacy.ps1` 转发到通用打包脚本，保留旧脚本调用入口。

许可文件保留在仓库 `docs/licenses/` 及依赖构建目录中。测试只在本地 `rebuild/tests`，
本次不提交测试、不更新测试子模块指针，Actions 不下载测试子模块或运行测试。

## 5. 用户仍需准备的文件

主 EXE 静态导入使用系统 `msvcrt.dll`，不再需要普通版原来的 `MSVCP140.dll`、
`MSVCP140_ATOMIC_WAIT.dll`、`VCRUNTIME140.dll` 和 UCRT API 集。
**VC-LTL、YY-Thunks 是构建依赖，不是要用户另行安装的运行库。**

仍应把程序放入已有千千静听运行目录，保留 `ttpcomm.dll`、`ttpres.dll`、皮肤和插件。
原版 AC3/DTS、ASF、MOD、MPC、OGG、RM 等插件仍可能依赖 VC++ 2012 x86，
合并主程序运行库不会重新链接这些二进制插件。已重建的 AAC 插件按其独立构建说明使用。

## 6. 本次验证

### 已完成

| 环境／检查 | 结果 |
| --- | --- |
| Windows 11（10.0.26200） | 通用 EXE 隔离启动及正常退出通过 |
| XP SP3（5.1.2600） | 同一 EXE 隔离启动及正常退出通过，无入口缺失错误 |
| Win7 SP1（6.1.7601） | 同一 EXE 隔离启动及正常退出通过 |
| 运行时策略矩阵 | 未知版本、XP、Vista、Win7、Win8、Win8.1、Win10、Win11 规则通过 |
| XP 实际策略／文件窗口 | 单轮、128 项／4 MiB、SMTC 安全跳过；打开／保存／文件夹为经典窗口，取消通过 |
| Win7 实际策略／文件窗口 | 单轮、128 项／4 MiB、SMTC 安全跳过；三个现代文件窗口显示及取消通过 |
| Windows 11 实际策略／文件窗口 | 5000 首阈值、512 项／16 MiB；三个现代文件窗口显示及取消通过 |
| Windows 11 原生 SMTC | 实际系统会话元数据、封面、AlbumArtist／Genres、播放控制、进度跳转、清理和生命周期通过 |
| 三系统播放导航 | 全播放模式、0／1／2／3／13／5000／5001 首、随机轮次、跨轮回退、空列表和切换通过 |
| 三系统信息加载 | 工作进程复用、缓存命中、错误／取消、CUE、队列及编辑竞态通过 |
| 三系统音量回归 | 主窗口滚轮、限幅、音量、静音、提示像素、超时重置、滑块及手动输入通过 |
| 主机旧系统组件后备 | 强制无 Media Foundation，WAV 解码／定位／转换、文件复制、内存流、播放工作线程通过 |
| 导入审计 | x86，子系统 5.01；19 个 DLL、659 个静态导入，XP／Win7 清单通过 |
| 工作流语法 | actionlint 1.7.12 通过 |
| 发布脚本离线回归 | 12 项日期／配置、1 项实际 ZIP 打包、35 项模拟发布及校验保护通过，无远程写入 |

原生 SMTC 测试在桌面会话运行；受限沙箱中系统媒体服务返回 `0x80070424`，
相同测试在正常桌面权限下完整通过。没有为此更改系统服务或生产代码。

本地旧组件测试有一个过期断言：此前恢复的格式探测链允许原生 PCM 拒绝截断 WAV 后
再尝试 DirectShow，不能要求整个解码链的 `Open` 必然失败。测试改为验证截断数据
不产生完整 PCM 帧且能结束读取；未改动这次任务之外的生产解码逻辑。

### 验证边界

- XP／Win7 的策略、对话框、导航、信息读取及音量均在真实虚拟机来宾中运行；
  没有仅以主机模拟系统版本代替来宾验证。
- Win10、Vista、Win8／8.1 未在本次运行系统上验证；版本策略单元检查不等于完整系统回归。
- WASAPI／ASIO 等实际驱动、所有插件与音频格式、在线服务没有在本轮穷举。
  本次未改变其实现，继续依赖既有按能力回退路径。
- GitHub Actions 通过本地语法／脚本和不含测试文件的干净源码构建验证；未触发远程工作流，
  未创建标签或发布线上 Release。

## 7. 相关文档

- [构建与发布](BUILDING.md)
- [XP／Win7 使用与兼容性](LEGACY_WINDOWS.md)
- [SMTC](SMTC.md)
- [随机播放索引](RANDOM_PLAYBACK_ROUNDS.md)
- [歌曲信息加载优化](PLAYLIST_INFO_LOADING_OPTIMIZATION.md)

## 8. 最终发行文件

输出目录：`D:\Projects\Backup\TTPlayer\rebuild\build\Release`。

| 文件 | 字节数 | 说明 |
| --- | ---: | --- |
| `TTPlayerRebuild.exe` | 3,116,544 | 通用 x86 Release，文件／产品版本 `2026.09.25` |
| `TTPlayerRebuild-2026.09.25.zip` | 1,558,133 | 仅含 EXE 和内层 `SHA256SUMS.txt` |
| `SHA256SUMS.txt` | — | ZIP 的外层 SHA-256 |
| `legacy-imports.json` | — | 最终 EXE 的 XP／Win7 导入审计报告，仅保留在本地构建目录 |

最终 EXE SHA-256：

```text
4b52210a7c15645a7aae2a8144a8e92465cd131ad0b0bab288e38bd3a6f6c077
```

通用 ZIP SHA-256：

```text
34fb80f2c3a62e16ab75aae9c490bf79cea89f86dbbe534575f8a679b9b8f313
```

文件版本资源校验、ZIP 内容检查、包内 EXE 哈希与审计报告及外层校验文件的一致性检查全部通过。
最终发行版本来自 `BUILD_TESTING=OFF`、不含测试目录的干净源码构建；保留 x86、Release、
VC-LTL／YY-Thunks 及体积优化，与 Actions 参数一致。原版运行文件不打入 ZIP。

最终发行 EXE 再次在 Windows 11、XP SP3、Win7 SP1 启动并正常退出（返回 0）。
从两个来宾回传 EXE 后计算 SHA-256，均与上述最终发行哈希完全一致。
