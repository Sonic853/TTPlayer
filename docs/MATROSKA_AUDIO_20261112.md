# MKV/MKA 音频第一、二期实现

日期：2026-11-12。用户最终选择**独立 `ttp_mkv` 插件**，不是宿主内置后端。

## 1. 架构与部署

- 本地独立 Git 工程：`../ttp_mkv`，输出 `AddIn/ttp_mkv.dll`。
- 一个原版 ReaderCreator，保留原 AddIn/Creator/Reader/Metadata/Thumbnail 虚表；没有新宿主导出、COM 注册或额外运行库安装要求。
- 插件内部：可定位有限长度 IStream → Matroska demux → 按真实 codec id 路由到裁剪静态解码器 → 原生 PCM → 宿主既有播放/转换链。
- 不把包伪装成 WAV/MP4/Ogg 文件，不生成提取音频临时文件，不运行 ffmpeg.exe，不解码视频，不依赖 MF/DirectShow 或其它音频 DLL。
- 本轮试写的宿主内置注册及静态后端接入均已撤除。播放器运行时改动仅为 Matroska 头部提示与更名识别，正常入口、文件筛选和关联候选来自插件实际登记的 `*.mkv;*.mka`。
- 尚无远端、稳定 Release 或合集登记，不猜测 `BUNDLES` 仓库名；不提交/推送，不替换工作区根目录已部署的播放器。

## 2. 实现范围

| 阶段 | 音频 | 后端和边界 |
|---|---|---|
| 一 | PCM | 整数 8/16/24/32-bit；16/24/32-bit BE/LE；IEEE float32/64 LE |
| 一 | MPEG Layer I/II/III、AAC、AC3、DTS Core | 插件内裁剪的原生音频解码；不是对旧插件文件 Reader 的再次封装 |
| 二 | Opus / Vorbis | 原生包级解码；包含 Opus SILK 需要的内部 swresample，不进行用户可见的无条件重采样 |
| 二 | FLAC / ALAC | 原生整数结果；ALAC 20/32-bit 与 FLAC 32-bit 均保留有效位及低位 |
| 二 | E-AC3、DTS-HD MA/HRA、DTS Express | 在首个有效解码帧后协商真实采样率、声道、格式；不走旧 DTS Decoder 的 Core-only 模式 |

只接受 `DocType=matroska`。WebM 继续由 `ttp_webm` 处理，不能把 WebM 改名 `.mkv` 就声称通用接受。

容器支持 SimpleBlock、BlockGroup、Xiph/固定/EBML lacing、CodecPrivate、CodecDelay、正/负 DiscardPadding、header stripping、无 Cues、有限输入中的未知长度 Segment；视频/字幕只跳过。

音轨优先级：第一个受支持、启用的默认音轨，否则第一个受支持且启用的音轨。FFmpeg 原版忽略 FlagEnabled，本后端在私有源码中作最小补丁，解析该标志并以私有 metadata 交给选择器。禁用和不支持音轨不抢占后续可播放音轨；损坏的已选音轨不会静默换轨掩盖错误。

## 3. PCM、时间轴和错误

- FLAC/ALAC 等整数不经 float32；S16/S32 中左对齐的有效位按容器宽度复制，非整容器有效位由 EXTENSIBLE valid bits 描述。
- PCM IEEE32/64 按原字节复制，测试覆盖负零、正负次正规数。float32 核心保持 float32，不宣称双精度解码。
- 原生 PCM BitDepth=20 等非整字节声明明确拒绝。上游 Matroska 对此会回退成 PCM16，不能接受该回退；ALAC 的 20-bit 是另一条合法路径，不受影响。
- 已知声道布局使用后端提供的 Windows 声道掩码；只有声道数量的多声道 PCM 保留交织顺序、发布未指定 mask，不猜测扬声器位置。
- 首个可听样本作为音轨零点；时间戳两 tick 内的舍入以真实解码样本数为准，较大内部间隙按格式生成静音。
- 定位采用重新创建 demux/decoder、从头预解码并丢弃至目标帧，确保预测/overlap/reservoir、CodecDelay 和 EOF 完整重置。完整历史自然覆盖 SeekPreRoll。**不宣称快速 Cues 定位；长文件远距离定位开销随目标增长。**
- duration 优先采用容器元数据；未提供时可为未知（0），不为获得 duration 强制解码整个视频文件。
- AVIO 累积短读，保留取消/传输 HRESULT；坏帧和显式不支持返回错误。已产生的有效 PCM 可先交付，下次读取返回 sticky error；失败定位后不允许旧缓冲继续播放。
- 检查有限 Segment 的声明长度，避免已知长度截断被当正常 EOF；不宣称能识别所有未知长度尾部截断或修复任意损坏文件。

## 4. 信息与格式协议

- 标签、所选音轨标签覆盖全局标签、PNG/JPEG 附件封面只读；Capabilities 不声明写入，不提供 Commit。
- `Matroska=1u<<25`、`All=0x3ffffff`；ABI v1、结构大小、IID 和 `Builtin` 不变。
- Probe 必须核对完整有界 EBML DocType。SourceInfo 只在真正 Open 成功后报告 `container=matroska` 与实际编码；建议 `.mkv`，合法别名 `.mka`，不建议改成内部音频后缀。
- 正式 SDK、插件副本和播放器 SDK snapshot/manifest 已同步。播放、信息读取、后台 manager snapshot 使用同一个 Reader 工厂。

## 5. 构建、许可证和验证

固定 FFmpeg 9.0.2、GNU make 4.4.1、VC-LTL 5.3.1、YY-Thunks 1.2.2，版本和 SHA-256 在独立工程内。构建脚本逐个确认所需 decoder 确实启用，防止配置依赖缺失时静默漏掉 Opus。只编译所需 audio decoder/parser、Matroska demux 和 Opus 内部重采样；没有视频解码器、编码器、协议或程序。

插件适配层为 MIT；链接后端为 LGPL-2.1-or-later。运行 ZIP 保持 DLL + SHA256，另附完整对应源码、后端源码、FlagEnabled 补丁脚本、GNU make 归档、许可证与构建身份，支持重新链接修改的 LGPL 后端。

私有测试：`tests/matroska_20261112/`；输出：`out/matroska-20261112/`。Action 不编译、运行或上传私有测试和样本。

已完成 Reader 矩阵 **59 项，0 失败**：52 组实际解码及其短读复跑、7 组预期拒绝；每个有效样本均作小缓冲、EOF、六次定位/回放对照、检查只读、Open/Seek 取消、非 seek 拒绝和 inspection 同轨验证。包含真实 MA/HRA/96 kHz/Express、整数低位、大小端、三种 lacing、头移除、间隙、首尾裁剪、默认/禁用/不支持音轨选择、封面、Unicode 和错误后缀。无损与原始 PCM逐字节比较，有损浮点用独立 FFmpeg 可执行文件作数值对照；MPEG I/II 的参考明确使用 float decoder，避免与工具默认整数 decoder 的量化差异混淆。

播放器格式 Probe、更名和错误后缀 self tests 三项通过；DLL 和播放器两个 EXE 的 XP/Win7 静态导入审计通过。

宿主集成 **52 项，0 失败**，覆盖筛选/关联候选（只读查询，不改注册表）、按内容接收错误后缀、inspection、DecodedAudioSource 全曲 PCM、重复定位、更名策略及后台 manager 保活。原有 WV/OFR/ALAC/Speex 等 **49 项**在加入 MKV 插件前后做完整 PCM/count/hash 对照，全部一致。Reader 矩阵中有 **29 组**实际触发读取中途取消，其余小文件可能已全部预取；所有有效样本均验证 Open/Seek 取消。

运行 ZIP 的文件布局与内外 SHA256 已校验；源码包含 **10,426** 个文件，已在独立目录按源包构建，再以 ALAC20、Opus 6 声道、96 kHz DTS-HD MA 三组验证 PCM 与主产物一致。冷重建不要求相邻插件或播放器源码，仅使用固定的兼容工具链缓存和源包自带 FFmpeg/GNU make。

最终 DLL 为 **1,303,552 字节**，SHA256 `2cbdc0ad8489e38697e2774a5e0d0c503d12a29e3a8fb94d25bf3ef3bb94f780`；无外置 FFmpeg DLL。报告分别为 `matrix.json`、`integration.json`、`packages.json`、`legacy-imports.json`、`virtual-machine-tests.json` 和 `no-mkv-tests.json`，运行/源码包汇总至 `out/matroska-20261112/delivery/`。

### 5.1 XP / Win7 虚拟机实测

通过已接入 OpenCode 的 `virtualbox`（XP）和 `virtualbox_win7` MCP，在原本已运行的虚拟机中执行；未启动/停止/重置虚拟机或恢复快照。每台使用独立 `C:\TTPlayerMkv_20261112_1627` 目录，不覆盖已有安装。使用的 DLL 与上述交付产物 SHA256 相同。

| 环境 | Reader 矩阵 | 短读复跑 | 宿主集成 | ABI / Probe self | 合计 / 失败 |
|---|---:|---:|---:|---:|---:|
| Windows XP SP3，5.1.2600，x86 | 59（52 解码、7 拒绝） | 52 | 52 | 2 | 165 / 0 |
| Windows 7 SP1，6.1.7601，AMD64，运行 x86 产物 | 59（52 解码、7 拒绝） | 52 | 52 | 2 | 165 / 0 |

每台的实际日志与 PCM 已复制回 `vm-results/xp/`、`vm-results/win7/`，不是仅凭 MCP 的 `(ok)` 判定成功。`compare_vm.py` 检查日志、格式字段、所有输出长度和短读一致性，并与 Windows 11 基线对照；**每台全部 52 组 PCM（包括浮点）均逐字节相同，浮点最大误差为 0**。Reader 的小缓冲、EOF、六次定位、取消、只读和 inspection 检查亦在来宾执行。来宾使用 ASCII `case-NNN` 文件名，不能据此声明来宾 Unicode 文件名已验证。

### 5.2 无 MKV 插件检查

另建 `C:\TTPlayerMkv_NoPlugin_20261112`，只放测试 EXE、播放器 EXE、`ttpcomm.dll`、`ttpres.dll` 和生成的 PCM WAV，输入插件目录为空。未复制或临时移动任何既有安装中的 DLL。

XP SP3、Win7 SP1 **各 5 项检查，0 失败**：内置 WAV 完整解码、信息读取、格式 self test，以及 `TTPlayer.exe` / `TTPlayerRebuild.exe` 两种名称的 GUI 启动/正常退出 smoke test。两种 EXE 分别使用新目录，退出码均为 0 且成功保存独立配置；测试使用专有 IPC 并跳过文件关联检查，不干扰已运行的播放器。WAV 为 48 kHz / 2 声道 / 16-bit，96,000 字节 PCM，完整输出哈希与原始样本一致。EXE 导入表不含 `ttp_mkv.dll` 或外置 FFmpeg DLL。

因此 `ttp_mkv.dll` 是**可选输入插件，不是播放器启动依赖**；不安装它不影响内置格式运行。其它插件的有/无 MKV 回归另见上述 Windows 11 的 49 项结果。缺少该插件时不保证 MKV/MKA 可用，也不以系统组件的偶然接管作为支持承诺。

**环境边界：** 已完成 Windows 11、XP/Win7 的上述 ABI/宿主实测，以及 XP/Win7 的无插件启动/退出检查；尚未验证实际音频设备输出、原版播放器 GUI 或 Win10，不据此宣称全系统/全界面验收。

## 6. 不在本次范围

手动音轨切换 UI、标签/封面写回、不可定位或持续增长直播、解密、任意压缩 ContentEncoding、TrueHD/MLP、WMA、Matroska WavPack/TTA/Speex 扩展以及快速 Cues 定位均不在第一、二期交付范围。
