# ReplayGain 多格式扫描修复与实测（2026-10-02）

## 先前覆盖范围

上一轮多曲目并行专项验证了 FLAC、Ogg Vorbis 和相应 CUE 路径；算法回归还包含 PCM／WAV 的 CUE 分段。**没有验证 MP3、AAC、WMA 的完整“扫描→保存→重新读取”流程。** 未验证格式保持串行，并不代表已经能扫描或保存增益。

## MP3 出错的原因和原版边界

原版 `TTPlayer.exe.pseudo.c`：

- `004A4E8C → 004B0D1A` 使用统一读取／解码入口；主程序内置读取器与 AddIn 都可以进入扫描。
- `004A50B5` 使用元数据接口写入 `replaygain_track_gain`、`replaygain_track_peak`。
- `004D9AC8 / 004D9B56` 是主程序内置 MPEG 标签路径；并不要求存在 `AddIn/ttp_mp3.dll`。后者在需要保存 ReplayGain 等字段时提供 APE 标签策略。

重建版此前存在三个遗漏：

1. 手动扫描只调用 `PluginManager::OpenReader`。MP3 没有对应 AddIn，返回 `0x80070032`，尚未读到 PCM 就失败。
2. 保存只打开 AddIn 元数据写入器，没有接入内置 MPEG 标签写入。
3. 播放自动扫描用 `HasReaderForPath` 排除没有 AddIn 的文件，也排除了内置 MP3。

这些问题与同时扫描 1／2／4 首无关。

## 本次修复

- AddIn 无法打开时，使用与播放相同的 `CreateDecodedAudioSource`，包括内置 PCM、现代系统 Media Foundation、XP 的旧系统读取路径和既有格式探测。访问／共享等终止错误仍直接返回。
- 在扫描线程正确管理可选 Media Foundation 的启动／关闭；先销毁音源再关闭平台服务。XP 不引入静态 MF 依赖。
- AAC／ASF 的压缩解码器已由 `LegacyReaderSession` 接入；问题在于扫描把单次空 PCM 块误判成失败。原读取状态机 `004E3CF3` 允许输入后暂时没有输出。现在继续读取，并在连续 1024 次无数据时返回错误；每次仍检查暂停与取消，并短暂让出 CPU。
- 新增只修改两个 ReplayGain 字段的 MPEG 写入路径。既有 ID3v2.3／2.4 和 APEv2 中的增益同步更新；无这两类标签时添加 APEv2。音频内容、其它 ID3 帧／APE 项、封面、歌词、ID3v1 与 Lyrics3 保留。不会套用文件属性窗口的整套标签格式转换选项。
- 清除 MP3 增益复用同一字段写入路径；播放自动扫描也允许内容验证通过的内置 MPEG 文件。
- 标签发布仍使用原有同目录临时文件和替换流程。损坏、无法安全重写的 ID3 或目前未支持重写的 ID3v2.2 返回失败，保留文件；不通过丢弃原标签来强行完成扫描。
- AddIn 设置字段成功并释放后，重新打开文件核对两个值。旧 ASF 插件对测试 WMA 返回 `S_OK` 却未保存字段，现在报告写入失败，避免界面误报“已完成”。

## 多格式实际结果

本地主机、VirtualBox XP、VirtualBox Win7 运行相同的合成文件与正式音频接口。下面“保存通过”包含关闭／重新打开核对，而非仅检查 setter 的返回值。

| 格式／用例 | 计算增益 | 保存并重新读取 | 说明 |
| --- | --- | --- | --- |
| MP3 CBR、VBR | 通过 | 通过 | 包含 22.05／44.1／48 kHz、单／双声道 |
| MP3 内容但后缀 `.flac` | 通过 | 通过 | 按真实内容接入 MPEG 路径 |
| MPEG Layer II `.mp2` | 通过 | 通过 | 验证共享的 MPEG 标签路径 |
| MP3 无标签、仅 ID3v1、仅 APEv2 | 通过 | 通过 | 按需要添加或更新 APEv2 |
| MP3 ID3v2.3／2.4 + APEv2 + Lyrics3 + ID3v1 | 通过 | 通过 | 更新两处增益并保留非目标原始数据 |
| M4A 内 AAC | 通过 | 通过 | 包含解码准备阶段空块 |
| FLAC | 通过 | 通过 | 已有并行专项继续通过 |
| Ogg Vorbis | 通过 | 通过 | 已有并行专项继续通过 |
| 裸 AAC／ADTS | 通过 | 不支持 | 当前插件没有可写增益标签接口；返回错误 |
| TTA | 通过 | 不支持 | 当前 FLAC/TTA 插件的 TTA 元数据接口拒绝设置字段 |
| WAV、AIFF | 通过 | 不支持 | 现有内置读取路径只读，未新增其标签写入器 |
| WMA／ASF | 通过 | 未保存，明确报错 | 本次测试的旧 ASF 插件忽略增益设置；重新读取可检出 |

没有宣称 APE、MPC、TAK、AC3/DTS、MOD 等其余格式已通过本次测试，也没有扩充并行安全白名单。MP3、AAC、WMA 等仍按上一轮策略使用保守串行扫描。

系统 MPEG 解码器的尾帧处理有差异：本机／Win7 部分无前置 ID3 文件首次添加 APE 后，解码帧数少一个 MPEG 帧；XP 的返回帧数也与新系统不同。测试确认这些样本的增益／峰值不变、原压缩音频字节保留，不把不同系统的解码结果描述成逐样本一致。

## 回归验证

本地测试都保留在 `rebuild/tests`，未提交测试代码，未将测试加入 Actions 或发行包。

- `audio_recovery_tests --replay-gain-formats`：19 个格式／标签组合、真实扫描窗口混合队列、错误后继续、跳过已有增益、清除增益、只读策略、取消不落盘。
- ID3v2.3／2.4 的非目标帧与 APE 原始项、嵌入 PNG、歌词、私有数据、Lyrics3、ID3v1 和 MPEG 音频数据逐字节保留检查；两种标签读取优先级均检查新增益。
- 实际 MP3 静音播放到 EOF，验证自动扫描落盘与手动扫描增益一致。
- `--replay-gain`、`--replay-gain-parallel`：本机原有增益与 FLAC／Ogg 并行回归通过。
- `builtin_file_info_tests`：本机内置 MP3 标签编辑回归通过。

本地日志：`tests/replaygain-formats-{host,XP,Win7}.log`；样本：`tests/artifacts/replaygain-formats-20261002`。虚拟机在独立目录运行 Guest Control 测试，不等同于人工逐项操作。XP 沿用前次 Ogg 测试目录的应用本地 `MSVCR110.dll`，这是旧插件自身依赖，没有新增系统级运行库安装。

## 发行

统一 x86 Release；`BUILD_TESTING=OFF`。主程序 20 个 DLL／714 个导入、更新器 13 个 DLL／291 个导入通过 XP／Win7 静态检查。发行文件输出到 `build/Release`，包名 `TTPlayerRebuild-2026.10.02.zip`。


本次固定输入插件：

| DLL | SHA-256 |
| --- | --- |
| `ttp_aac.dll` | `80e30c42f647de03dc99272c7d9f76350891da2f843fcc1c2af45458d29762d7` |
| `ttp_asf.dll` | `f36dcc9104e7bd11100ec983c03b0183da5a9d77592b806fe8a909d87d18fcf5` |
| `ttp_flac.dll` | `e2833c023377787fa9705d52215d3a02b7f5a742bdda4012f9d639c2e1f64dbd` |
| `ttp_ogg.dll` | `ee58d0603a5c4c7596219590d5e92e12748187b395e14dc1275256e0904dcdc5` |

这些结果针对上表二进制及所测样本，不能推及其它版本插件。ZIP SHA-256：`9164c464bc065e5f1db62dd636db017f78495d0b28d463b929f1b48404b82e4d`；已核对四个包内条目、校验清单与正式构建文件一致。
