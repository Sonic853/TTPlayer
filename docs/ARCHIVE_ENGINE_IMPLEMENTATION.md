# 归档引擎迁移及 RAR4／RAR5 修复

日期：2026-10-11。

## 1. 修复内容

| 原问题 | 当前实现 |
| --- | --- |
| RAR 依赖系统 Shell，XP／Win7 无法保证支持 | UnRAR 7.3.1 静态并入重建 `ttpcomm.dll`，不依赖 WinRAR 安装或现代系统 Shell |
| Shell 解压整包，再等待缓存完成标志 | 枚举只读成员头；打开歌曲时只取得该成员的流，成功解压并校验后才交给 Reader |
| RAR 成员被按名称排序 | 保留归档成员顺序，与 `00473338`／`00474051` 的导入顺序对应 |
| 缓存可能残留、成员重复占用内存 | 小成员保存在 DLL 内存中，大成员使用关闭时删除的临时文件；Reader 直接读流 |
| RAR5 无统一支持或错误信息 | 支持 Stored、普通压缩、固实、Unicode、同目录完整分卷；区分密码、缺卷、校验、字典限制等错误 |
| XP 中 RAR 能解压，但包内 WAV 无法播放 | 原生 PCM 读取器增加文件／归档流适配，WAV、AIFF/AIFC、AU/SND 不再必须依赖 Windows Media 的 OpenStream |
| 新 EXE 可能搭配旧核心 DLL | 启动、Actions 组件校验及打包要求新的 `ttpcomm_query_archive`（502）接口 |

归档逻辑仍使用原有 `archive.rar|member` 标识，已保存的播放列表不依赖临时解压目录。包内 CUE 继续以逻辑成员路径解析相对 FILE，歌曲仍按原有解码器选择流程打开。归档成员保持只读，不向压缩包回写标签或封面。

原版 5.7.9 的依据：`004738B5` 分别选择 ZIP／RAR 工厂，`00474051` 枚举并分流普通歌曲和 CUE，`004F9332` 的旧签名识别拒绝 RAR5。详细对照见 [前期分析](ARCHIVE_RAR4_RAR5_ANALYSIS.md)。新 ABI 不改变旧 67 个导出，也不宣称替换 DLL 后原版 EXE 自身就支持 RAR5。

## 2. 代码分工

- `ttpcomm/src/archive.cpp`：归档枚举、成员解压、校验、只读 IStream、内存／临时文件存储及错误映射。
- `ttpcomm/cmake/unrar.cmake`、`adapt_unrar.py`：固定版本源码下载与固实前序取消回调适配。
- `third_party/ttpcomm-sdk`：经清单校验的 ABI 1 头文件及查询封装。
- `src/audio/archive_member.cpp`：RAR 调用 DLL；ZIP 保持已有实现；CUE／文本提供有上限的缓冲适配。
- `src/audio/audio_engine.cpp`：插件与 Media Foundation 直接接收成员流，PCM 文件读取器也接收成员流。
- `src/audio/legacy_windows_source.cpp`：旧 Windows Media 分支复用成员流，保留归档错误信息。
- `src/app/file_info_worker.cpp`：元数据 Reader 使用同一归档服务，COM 流以 RAII 释放。
- `src/ui/player_window_file_intake.cpp`：按归档顺序导入并显示归档枚举错误。
- `cmake/download_ttpcomm_component.py`、`package_player.ps1`：验证新导出和组件记录。

ABI、上游固定 SHA256、许可证及完整资源限制见 `ttpcomm/docs/ARCHIVE_SERVICE.md`。UnRAR 源码只下载至构建目录，许可证保留在仓库，不嵌入 DLL。

## 3. 实测结果

| 检查 | Windows 11 | XP SP3 虚拟机 | Win7 SP1 虚拟机 |
| --- | --- | --- | --- |
| 归档 ABI 及异常路径 | 163 项通过 | 163 项通过 | 163 项通过 |
| 实际播放器解码与 Seek、逐字节 PCM 比较 | 17 组通过 | 17 组通过 | 17 组通过 |

163 项检查包含 RAR4 Stored 和实际压缩、RAR5 Stored／普通／固实、中文外层路径及包内路径、数据密码与头密码、错误密码、完整／缺失分卷、CRC 损坏、截断、伪 RAR、枚举顺序、EOF、Seek、Clone、只读流、磁盘后备、取消、字典／成员大小限制和四线程调用。

17 组比较使用生产 `ttplayer_core` 的 `CreateDecodedAudioSource`，覆盖 RAR4／RAR5／ZIP 中 WAV、RAR5 中 AIFF／AU、包内 CUE、FLAC 插件。读取结果与未压缩样本相同，回到起点后读取结果也一致。测试关闭声音输出，不依赖听感判断。XP 初次测试发现 Windows Media 拒绝包内 WAV；上述 PCM 流适配修复后复测通过。

此外，生产 `archive_member.cpp`／`skin_package.cpp` 的 5 组后端样本逐字节验证通过。DLL 的 67 个旧导出和 3 个扩展导出审计通过；DLL、主 EXE、更新器均通过 XP／Win7 静态导入审计。

另执行了一次现有完整 `audio_recovery_tests`：原生 WAV／AU／AIFF 转换检查通过，随后停在既有 CDA 测试对错误文字 `CreateFile(CD drive)` 的断言。该 CDA 路由及设备打开实现本次未改动；此轮不将完整音频测试计为通过。归档专项检查独立执行并全部通过，记录为 `version_display/archive-audio-recovery.log`。

测试代码、样本和原始记录只在本地 `rebuild/tests`：

- `archive_support/probe_api.cpp`、`player_decode.inc`、`vm_results.json`。
- `archive_support/backend_results.json`。
- `version_display/archive-api-final.log`、`archive-player-decode-final.log`、`archive-component-stage.log`。

测试没有加入 Actions，也不包含在发行压缩包中。没有真实 Windows 10 环境；本次用 Windows 11 完成现代系统验证。没有重新执行完整的主窗／歌曲列表／列表空白处人工拖放、重启恢复和全格式矩阵；这些导入入口仍走原有公共逻辑，不将接口与解码测试冒充完整 UI 测试。

## 4. 资源与交互边界

- 默认字典上限 256 MiB，最高允许 512 MiB；超出限制返回明确错误。32 位进程并非支持任意现代 RAR5 大字典。
- 单成员默认上限 8 GiB，默认内存阈值 16 MiB；超过阈值使用临时文件。CUE／文本缓冲适配限制 64 MiB。
- 固实归档需要处理前序成员，暂未实现跨歌曲缓存、并行预取。为隔离上游全局错误状态，引擎调用串行化，已打开的流独立读取。
- 引擎已有密码参数、进度和取消回调；播放器暂未新增密码输入或专门的归档进度对话框。没有密码时报告需要密码。
- 普通成员枚举不解压内容；现有导入线程上的包内 CUE 读取仍可能等待。
- 本次未增加 7z／TAR，也未改造 ZIP64、ZIP Unicode 扩展及 ZIP 加密功能。

## 5. 输出与发布顺序

本地输出：

- `build/Release/TTPlayerRebuild.exe`
- `build/Release/ttpcomm.dll`
- `build/Release/TTPlayerRebuild-2026.10.11.zip`
- 独立核心包：`ttpcomm/build/Release/ttpcomm-2026.10.11.zip`

核心组件记录标注 `local-unpublished-build`，不将本地验证包当作已发布版本。播放器包仅包含既有发行文件及新 DLL、SHA256 清单。

本轮生成包与输出文件逐项校验一致：

| 文件 | SHA256 |
| --- | --- |
| `TTPlayerRebuild.exe` | `7d8ce2b34afccc559d604dec01df565a73609dfce6e2c024dc3f8ed0a8a107da` |
| `ttpcomm.dll` | `45f54bfb8b3df515ed11620cd15da76e7796414ab4ef6e3b7a623051b287e86a` |
| `TTPlayerRebuild-2026.10.11.zip` | `9d72cf4959e66cbe6f89f9cf6804857a75c10e7f14945b628fb4152a14553efa` |

**先发布含 502 接口的 ttpcomm，再运行播放器发行 Action。** 在此之前，Action 若下载到旧核心，应按新增校验拒绝构建。播放器更新继续采用手动完整替换包的方式；仅替换 EXE 无法满足新接口要求。
