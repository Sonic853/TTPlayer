# 5.7.9 与当前重建版：音频功能逐项核查

主报告：[全功能对照与修复顺序](PLAYER_FULL_FEATURE_COMPARISON_20261020.md)。本专题与主报告共同组成本轮审计。

日期：2026-10-20。范围：输入与解码、PCM 处理、输出设备、播放时序、转换与抓轨，以及与这些流程直接相关的网络流。仅分析；没有修改生产源码、编译播放器、加载实体音频设备或播放用户音频。

## 1. 基线、证据和结论边界

本轮基线是 `rebuild` 的 **`c63237e77a36e091852526ad9a450435bd0116f9`**，已包含 2026-10-19 的音频主链修复，以及随后更新的插件、格式探测、内置 MPEG Decoder。不能把旧报告中“没有内置 MPEG Decoder”“自动位深总是 16 位”“DSP 在重采样前”“XP MMS 只走 MF”等旧结论继续当作当前缺陷。

原版样本：

- 根目录 `TTPlayer.exe` SHA-256：`c7999ea5823469c1684148cac1bd196009824348ccac5cdd45978f8fb574e28c`。
- 根目录 `ttpcomm.dll` SHA-256：`e349ef73e8a1d35b2c5debd2ddaeb5cc2513a90647766b7339d2605b34483b33`。
- 原版函数地址指 `reverse/decompiled/TTPlayer.exe.pseudo.c` 的映像地址；`evidence.json` 保存了本轮读取的入口行号和文件 SHA-256。旧反编译漏掉的内部标签不能假装有完整独立函数。
- 当前文件路径在下文相对于 `D:\Projects\Backup\TTPlayer\rebuild`；插件项目和 `ttpcomm` 则相对于工程根目录注明。

证据分级：

| 代码 | 意义 |
|---|---|
| S | 本轮重新阅读当前源码，并核对原版伪代码／已保存二进制证据。确认控制流，未宣称动态等价 |
| M | 本轮运行的有界数学／状态模型。它验证缓冲约束，**不是**真实 DirectSound/WASAPI 端点试验 |
| H | 过去实际执行的测试，有私有日志或实施记录；本轮没有重跑 |
| U | 仍需真实设备、特定文件或完整运行时对照 |

状态：

- **已接通**：当前有完整可达入口和对应核心职责；不等于所有输入逐样本相同。
- **部分**：存在实际实现，但有明确未恢复职责或组合边界。
- **扩展**：原版没有对应功能，或按用户要求有意改变。
- **原版边界**：原版本来如此，不记为重建缺失。

### 结论

主要播放链已经建立。当前最值得优先修复的是：

1. **手动 ReplayGain 扫描复制了不完整的 `WAVEFORMATEXTENSIBLE`，随后越界读取扩展字段。**
2. **DirectSound 和 WASAPI 的固定四大块缓冲与 80% 恢复水位组合存在不可达状态，断流后可能永久停在补缓冲。**
3. **普通播放定位漏掉了原版“解码器返回较早时间后丢弃前缀 PCM”的处理；CUE 路径已单独实现这部分。**

此外，通用 ACM Decoder、普通本地 MPEG Reader 的原路径、KS 拓扑音量仍有恢复差异。真实 ASIO/KS、物理 CD、真正 mp3PRO、失效的第三方 DirectShow provider 等不能用虚拟机或模拟驱动测试代替。

## 2. 输入与插件宿主矩阵

| 功能 | 原版证据 | 当前入口／源码 | 状态与本轮结论 | 证据 |
|---|---|---|---|---|
| AddIn 枚举与分类、Reader/Decoder/Encoder 工厂 | `004CA48E`、`004CA86C`，随后 reader `004CBC1F`、decoder `004CD045` | `src/plugins/plugin_manager.cpp:2262`、`:2273`、`:2494`、`:2815` | 已接通。普通候选打开失败后继续，不把“扩展名匹配”当作成功打开 | S/H |
| Decoder 工厂返回 `S_FALSE` 仍可使用 | `004CD045` 内 `004CD0ED` 只用 FAILED 判定 subtype support | `plugin_manager.cpp:2796`、`:2843` | 已接通。未再次引入“只有 S_OK 才是候选”的旧错误 | S/H |
| 保留压缩 Reader + 独立 Decoder | `004E3CF3`、`004B1375` | `plugin_manager.cpp:1228`、`:1310`、`:1687` | 已接通。持久输入 CBuffer，部分输出缓存、EOF 和 decoder reset；外部异常受到边界检查 | S/H |
| 内置 MPEG Decoder，包含 MP3/MPEG/ADU subtype | `004C9AC1`、`004C96D5`、`004C9748`、`004C98D0` | `src/plugins/builtin_mpeg_decoder.cpp:111`、`:146`、`plugin_manager.cpp:2820` | 已接通，优先于外部 Decoder；使用 ttpcomm ordinal 10–14。新提交确实在当前树中 | S/H |
| 通用 ACM Decoder | `004C9C1F` 的 acmStreamOpen 与格式降级；`004CA48E` 中 `004CA0A0` 注册 | `plugin_manager.cpp:2815` | **部分／缺少专门实现，见 A04**。现在只有内置 MPEG + 外部 Decoder 候选 | S |
| 普通本地 MPEG Reader | `004E4B71`、`004CA86C`，Reader 后接公共 Decoder | `src/audio/audio_engine.cpp:1253`、`:1368`–`:1387` | **部分，见 A05**。一般本地 MP3 的后备依旧是 MF／Windows Media，不因增加 MPEG Decoder 就自动成为原版 Reader 路线 | S/H/U |
| RAW PCM WAV、AIFF/AIFC、AU/SND | 原内置 Wave Reader `004EA0EA`，其他系统 reader 候选 | `audio_engine.cpp:901`、`:998`、`:1062`、`:1142`、`:1197` | 已接通。大小端、符号 8 位、整数／浮点、帧对齐与范围验证；新增解析形式不能倒推原版逐格式全同 | S/H |
| 系统最终 Reader / DirectShow 图 | `004CBC1F` 最终无扩展名限制候选、`004E8BD6` | `src/audio/directshow_source.cpp:523`、`:590`、`:608`；`audio_engine.cpp:1382` | 已接通。自己接收 PCM，不让默认 renderer 绕过播放器输出链 | S/H/U |
| XP 的 Windows Media PCM fallback | 原 ASF/WM 及公共 Reader/Decoder 结构；不等同于原私有 MPEG Reader | `src/audio/legacy_windows_source.cpp:20`、`:91`、`:118` | 已接通的替代实现。输出由系统 codec 决定，不能宣称与旧内核逐样本一致 | S/H/U |
| 错后缀、强格式探测、打开后报告实际格式 | 原 `004CBC1F` 候选尝试及系统识别；新 Probe IID 原版没有 | `src/audio/format_probe.cpp:91`、`:129`；`plugin_manager.cpp:2389`、`:2519`、`:2735` | 原机制已接通并有扩展。探测不直接授权写入或改名；Opened 结果来自成功打开 | S/H |
| mp3PRO 增强 | `004E6CBD`、`004E6FB6`、`004E738F`、内部 `004E73D9` | `src/audio/mp3pro_source.cpp:291`、`:321`、`:386`、`:418` | 已接通可选 Winamp 输入桥，保留单占用、基础 MPEG fallback、PCM 尾部和异步 seek 确认。真正 SBR 音频仍 U | S/H/U |
| Reader 输出容量与小请求 | `004E3CF3`、CBuffer，原 CUE 限制剩余字节数 | `plugin_manager.cpp:1363`、`:1762` | 已接通。Reader 容量不放大；Decoder 整包最小容量通过本地 pending PCM 消化 | S/H |
| Reader 返回非零成功码 EOF | 原 ASF/WMA 等 Reader 的成功非零结束码 | `plugin_manager.cpp:1401` | 已接通；不能只把 S_FALSE 看作 EOF | S/H |
| 可并行读、交叉淡出的源能力 | `0045BB61`–`0045BB89` 的 `(capabilities & 9)==0` | `audio_engine.cpp:664`、`:1413`、`:1577`、`:1624`、`:1647`、`:2468` | 已接通。网络、CD、原有特殊来源不会误进普通双对象交叉淡出 | S/H |

### 2.1 当前格式覆盖不是仅靠 EXE 扩展名表

原版大多数格式由 AddIn 实现，EXE 负责 `004CBC1F/004CD045` 接口选择。以下表核对的是**当前宿主可接入的模块职责**，并非在本轮重新证明所有插件内部算法与旧二进制相同。

| 格式组 | 原版来源 | 当前来源 | 当前状态和边界 |
|---|---|---|---|
| MPEG-1/2 Layer I/II/III | EXE MPEG Reader + 内置 MPEG Decoder / ttpcomm | 本地系统 source；压缩 Reader 可用 `builtin_mpeg_decoder.cpp`；HTTP `http_mpeg_source.cpp:36` | 能力已存在，原路径仍有 A05 差异 |
| AAC、ADTS、MP4/M4A、ADU | `ttp_aac.dll`、原内置 MPEG ADU subtype | 根项目 `ttp_aac/src/reader.cpp`、公共 Decoder | 已接通；本轮明确核查其 `Seek` 回写帧时间，形成 A03 的可达输入 |
| Vorbis/Ogg | 原 `ttp_ogg.dll` | `ttp_ogg` | 已接通；跨编码链、损坏页、标签写入的完整插件审计超出本轮 EXE 检查 |
| Opus | 原 5.7.9 无同等原生路径 | `ttp_ogg` 的 Opus 扩展 | 扩展；不能列为“未与原版一致” |
| FLAC、TTA | 原 `ttp_flac.dll` | `ttp_flac` | 已接通，包括原宿主接口；历史浮点 TTA 的稀有语料仍不等于普遍全覆盖 |
| APE | 原 `ttp_ape.dll` | `ttp_ape` | 已接通；现代库替换的历史文件覆盖需按插件报告区分 |
| MPC | 原 `ttp_mpc.dll` | `ttp_mpc` | 已接通，最近插件更新已经落入打包目录 |
| AC3、DTS、DTS-CD 解码 | 原 `ttp_ac3.dll` / `ttp_dts.dll` 类别；CD 用 ordinal 106 探测 | `ttp_ac3dts` 的 Reader/Decoder + `cda_source.cpp:78` | 已接通；不等于实体 DTS-CD 已测试 |
| MOD/S3M/XM/IT 等 tracker | 原 `ttp_mod.dll` | `ttp_mod`，默认兼容 libmodplug，可选现代模块后端 | 已接通；现代可选引擎是扩展，不能期待两引擎逐样本一致 |
| ASF/WMA | 原 `ttp_asf.dll` / WM Decoder | `ttp_asf`；系统 source 后备；内部 MP3 可经内置 MPEG Decoder | 已接通；不再把 ASF-MP3 解码缺失列为当前问题 |
| RM、TAK、VQF | 对应旧 AddIn | `ttp_rm`、`ttp_tak`、`ttp_vqf` | 已接通；最新实现中使用的替代内核与旧 DLL 的逐文件等价应另做语料对照 |
| WebM/WebA | 原版无对应原生支持 | `ttp_webm` | 扩展，统一 Reader/标签/封面接口 |
| MIDI/MID/RMI | EXE 特殊播放对象；Reader 不提供合成 PCM | `midi_player.cpp` / `AudioEngine::MidiWorker` | 见 MIDI 矩阵；不把无法离线 PCM 转换算作漏实现 |
| CDA | EXE 内置 CD Reader | `cda_source.cpp`、`disc_media.cpp` | 普通数字音频、DTS 探测、缓存/标签等已接通；物理驱动 U |
| CUE、压缩包成员 | EXE CUE/stream 路线 | `CueSegmentSource`、archive IStream | 接入公共解码。归档只读遵从用户要求；归档格式全审计由其他分工负责 |
| OSZ 背景/多音频 | 原 5.7.9 无对应 | `osz_archive.cpp` + archive IStream | 扩展；不改变 ZIP 后缀属于既定行为 |

最近历史验证：`docs/AUDIO_FORMAT_PROBE_IMPLEMENTATION_20261020.md` 的三系统 99/99 包括 28 格式×3 后缀和 12 组 ABI 检查；`docs/AUDIO_PLUGIN_RECONSTRUCTION_IMPLEMENTATION_20261019.md` 记载插件更新。**这些不是本轮新跑**，也不覆盖每种编码的所有历史版本和恶意输入。

## 3. PCM、音效、扫描与 Winamp 矩阵

| 功能 | 原版证据 | 当前入口／源码 | 状态与剩余边界 | 证据 |
|---|---|---|---|---|
| 自动位深 | `004AB1C4` / 伪代码 162471–162483：读 Reader 原位深，合法 8/16/24/32，否则 16 | `pcm_effects.cpp:11`；`audio_engine.cpp:2728`；`playlist_transforms.cpp:1607` | 已接通。Reader 用 float64 作为解码容器不意味着 Auto 输出 64 位 | S/H |
| PCM 输入格式、有效位、半幅 double 域 | `004B1375`、`004B1950`、`004AA360` | `pcm_output_transform.cpp:94`、`:161`；`ttpcomm/sdk/include/ttpcomm/pcm.h:21` | 已接通。保留 format 有效位，正常播放并非 A01 那样截断格式 | S/H |
| 播放 ReplayGain | `004B107E` 两标签、`004B1950` gain/软限幅 | `pcm_effects.cpp:16`、`:54`；`audio_engine.cpp:2018` | 已接通；有限值检查是安全加固。zero peak 标签对与非有限处理不能误报旧回归 | S/H |
| EQ 十频段＋前级／预设 | ordinal 103，`004B1B81`、`004B182C` | `pcm_effects.cpp:33`；`include/ttplayer/audio/legacy_equalizer.h` | 已接通。源采样率、声道初始化；更新与 Reset 分开。ttpcomm OOM 恢复已修 | S/H |
| 比例环绕 | ordinal 104，`004B1B81`、`004B16FE` | `pcm_effects.cpp:45`、`:67`、`:73` | 已接通；参数变化重建环绕对象，定位清状态 | S/H |
| 源效果→SSRC 的顺序 | `004B1375` / 168626–168636 | `audio_engine.cpp:2897`、`:3341`；`playlist_transforms.cpp:1586`、`:1649` | 已接通。播放与离线共用 PcmSourceEffects；不能再报“EQ 在重采样后” | S/H |
| SSRC 模式、尾部和定位 reset | ordinal 102，`004B1BF4`，`004B16FE` | `pcm_output_transform.cpp:138`、`:196`、`:227`、`:289` | 已接通。EOF 排空已产队列，不额外向 EQ 灌零、不把 Reset 当 Finish | S/H |
| Dither/noise shaping 与无抖动舍入 | `004AAFAF`、`004AA360` | `pcm_output_transform.cpp:71`、`:79`；ttpcomm runtime quantizer | 已接通、有状态。IA32 基线和 SSE2 gain 分派是已完成兼容修复，本轮不重复记缺陷 | S/H |
| Winamp 16 位桥与处理顺序 | `004AC166` → 16 位量化 → `0042898D` → `004AA8B0` | `pcm_output_transform.cpp:250`–`:278`；`winamp_dsp.cpp:238` | 已接通。16 位以上源会先量化为 16 位；与原版当前可达调用一致 | S/H |
| Winamp ABI 的返回长度／容量 | `004280CD` 忽略返回长度并预留两倍容量 | `winamp_dsp.cpp:444`–`:519` | 保留原宿主固定长度语义、两倍 scratch、超大块分段；不能用现代 Winamp 的其它宿主行为认定这里缺少高位深协议 | S/H |
| DSP 加载、配置、关闭、错误隔离 | 原 Winamp wrapper 生命周期 `004280CD/0042898D` 周边 | `winamp_dsp.cpp:371`、`:530`、`:572`、`:729` | 已接通。配置重入/排队、异常禁用，未知插件仍在进程内，不是沙箱 | S/H/U |
| 手动 ReplayGain 分析 | `004A4E8C`、`004A50B5` | `replay_gain_scanner.cpp:321`、`:375`、`:446` | **部分：A01 扩展 PCM 结构问题**；普通 PCM 的多曲目扫描、取消、写盘校验存在 | S/H |
| 多曲目并行分析及旧插件串行门 | 原版不是该现代并行方案 | `replay_gain_scanner.cpp:199`、`:254`、`:336`；`playlist_transforms.cpp:296` | 扩展。只放行已知可并行来源，写盘有共享门，不能粗暴并行所有旧 DLL | S/H |
| 播放中自动扫描增益 | `004B1950` 分析原 PCM、`004B16FE` 定位撤销分析、`004B1A5C` 结束写盘 | `audio_engine.cpp:1964`、`:2018`；`replay_gain_scanner.cpp:650`、`:721` | 已接通。天然结束、reader 释放之后提交；源版本保护已接入。提交后 UI 缓存发布由数据分工另审 | S/H |
| 可视化 PCM 快照 | 原 sound 对象两路 512 样本接口 | `audio_engine.cpp:2065`、`:2121`、`:3445`、`:3143` | 已接通，按真实播放游标提取，mono 复制右声道；画面算法由 UI 分工审计 | S/H |

### 对 Winamp 高位深的明确结论

`ModifySamples` ABI 中有 `nbits` 参数，并不等于所有插件都有“支持 24/32 位”的可查询能力。当前 `winamp_dsp.cpp:244` 真实传值为 16；原 `004AC166` 同样先量化到 16。输出设备选 24/32 位仅改变桥接之后的整数容器，不能恢复已经丢掉的精度。

当前保持未知插件 16 位是兼容边界，不应在这轮原版恢复中混入高位深猜测。若以后引入明确白名单或版本化扩展 ABI，应单独验证输入布局、双倍容量、返回帧数、插件内部 casts 以及最终 dither。不能用崩溃试探作为能力协商。

## 4. 输出、控制、淡化和时钟矩阵

| 功能 | 原版证据 | 当前入口／源码 | 状态与剩余边界 | 证据 |
|---|---|---|---|---|
| 输出类型／设备键 | `004918B1/004918FE`、`004AB1C4` | `native_output_contract.cpp:11`；`audio_engine.cpp:2182` | 已接通。旧 GUID 路由保留，ASIO/KS 稳定键为扩展 | S/H |
| Native 设备枚举与可用性 | 原 ASIO/KS 枚举与 `004E0502/004E1450` | `src/ui/output_devices.cpp:518`、`:636`、`:694`、`:727` | 已接通。deadline=0、无 topology、通配范围等已修；枚举成功不等于硬件成功播放 | S/H/U |
| WaveOut 开关与 WOM_DONE 回收 | `004E24B6`、`004E2585`、`004E27AD`、`004E2471` | `audio_engine.cpp:3274`、`:3304`、`:3431`、`:3680` | 已接通；四块有界队列。seek 前 reset/pause、尾部等已有历史测试 | S/H |
| WaveOut 音量与外部状态恢复 | `004E2585`、`004E263A`、`004E2471` | `audio_engine.cpp:3284`、`:3698`、`:4335` | 已接通。不再把“切其它输出异常小声”的历史缺陷当当前未修 | S/H |
| DirectSound 创建、默认设备后备 | `004C3170`，内部 `004C31AA` 等 | `audio_engine.cpp:2736`–`:2822` | 已接通，主缓冲／硬件缓冲选项和 PCM ring 存在 | S/H/U |
| DirectSound 低水停止／补缓冲 | `004AC605`，原 buffer 数量 `004AB1C4` | `audio_engine.cpp:3013`–`:3045`、`:3128` | **A02：四大块＋80% 水位不可达** | S/M/U |
| ASIO 激活、COM 回退 | `004E207B` | `asio_sink.cpp:159`、`:438` | 已接通；特殊工厂失败回退、普通 COM 等已修；x64-only 驱动不能装入 x86 进程 | S/H/U |
| ASIO 格式、通道、preferred 双缓冲 | `004E14BD`、`004E1610` | `asio_sink.cpp:446`–`:514`；`native_output_contract.cpp:99`、`:124` | 已接通。CanSampleRate 不硬拒绝，总输出声道不是限制成 32；实际只开所需首通道 | S/H/U |
| ASIO 首周期、真实消费计数、EOF／暂停恢复 | `004E19E5`、`004E1AF2` | `asio_sink.cpp:330`–`:429`、`:581`、`:643` | 已接通，历史真实双缓冲模型验证；未证明每个实体驱动回调次序 | S/H/U |
| ASIO reset/rate 通知与 outputReady | 原回调表／ASIO SDK 契约 | `asio_sink.cpp:675`–`:701` | 已接通的受限能力：reset/rate 改变通知导致工作线程报错重建流程，不声称无缝自愈；不虚报 resync/timecode | S/H/U |
| ASIO 音量、balance、PCM 淡化 | 原输出相关槽是未实现 | `audio_engine.cpp:3410` | **原版边界**。unity PCM，不能为“所有输出统一淡化”悄悄破坏原行为 | S |
| KS pin/格式/提交与取消 | `004E029E`、`004F066E`、`004E0430`、`004E088A` | `kernel_streaming_sink.cpp:170`、`:328`、`:368`；`native_output_contract.cpp:84` | 已接通。mono→stereo、unsigned8、16 位后备、异步数据所有权已修 | S/H/U |
| KS 音量／拓扑与淡化 | `004E061B` 查拓扑音量节点 | `audio_engine.cpp:3395`–`:3409`；KS transport 无对应 node 设置 | **部分，A06**：软件逐包音量，不能等同原硬件控制；显式过渡淡化仍禁用 | S/U |
| WASAPI 共享／独占、软件音量 | 原版 5.7.9 无 WASAPI | `wasapi_sink.cpp:167`、`:241`、`:250`、`:357` | 扩展。独立渲染线程、格式协商/对齐重试/设备错误明确；有 A02 | S/H/U |
| 播放、暂停、恢复、停止、定位命令 | `004ABA14/004ABA2F`、`004AC605`、`004B16FE` | `audio_engine.cpp:3888`、`:3920`、`:3974`、`:4061` | 主链已接通，状态请求与设备所有权分离。普通 seek 精确裁剪仍 A03 | S/H |
| 普通定位后的源 PCM 修剪 | `004B16FE` 写 `+0x84`，`004B1375` 消费前缀 | `audio_engine.cpp:804`、`:3084`、`:3560` | **部分，A03**。保存返回时间但普通输出路径不使用 | S |
| 播放／暂停／定位／停止淡化 | `004AC605`、`004ACD95/004ACE17`，五个独立位 | `audio_engine.cpp:3854`、`:3888`、`:4008`、`:4136`、`:4208`、`:4297` | 已接通，可中断且不会跳回满音量；KS/ASIO/MIDI 边界见各行 | S/H |
| 手动切歌 DirectSound 双对象淡化 | `0045BB61`–`0045BB9D`，旧曲退出 Winamp | `audio_engine.cpp:2438`、`:2468`；主窗口 `PrepareTrackChange` 调用 | 已接通；旧来源能力限制与共享 DSP 生命周期保留 | S/H |
| WaveOut 切歌串行淡化 | 原版 WaveOut 不进类型1交叉淡出 | `audio_engine.cpp:2484`、`:4126` | 用户要求的扩展；不是原版漏恢复 | S/H |
| 结束/失败后清理与通知 | `004AAC0F`、`004AC605` | `audio_engine.cpp:2629`–`:2643`、`:3665`、`:3679` | 已接通。等待输出尾部，通知在清理后；具体 native 驱动 U | S/H/U |

## 5. MIDI、CD、CUE 与转换矩阵

| 功能 | 原版证据 | 当前入口／源码 | 状态与剩余边界 | 证据 |
|---|---|---|---|---|
| MIDI 按后缀分流 | `004AA982` 明确检查 .mid/.rmi/.midi；`004AB1C4` 特殊对象 | `midi_player.cpp:9`；`audio_engine.cpp:2617` | 已接通。错后缀 MIDI 分流属于原版也有的边界，不误报为新缺陷 | S |
| MIDI 图建立和控制 | `004E2A40`、`004E2B9C/004E2BDE/004E2C20` | `midi_player.cpp:41`–`:87`；`audio_engine.cpp:3712` | 已接通，在同一 COM 工作线程创建/使用/释放；系统合成器能力 U | S/H/U |
| MIDI seek、音量、balance、50ms EOF | `004E2DAF`、`004E2C72/004E2CCE`、内部 `004E2EA2` | `midi_player.cpp:20`、`:27`、`:89`、`:117`；`audio_engine.cpp:3784` | 已接通。不得把合成器固定格式占位看作真实采样输出 | S/H/U |
| MIDI EQ/SSRC/RG/转换 | 原 Reader vtable `005230E8` 的 `+0x38/+0x3c` 为 E_NOTIMPL | `audio_engine.cpp:1233`、`:1632` | **原版边界**：没有向 PCM 管线提供 MIDI 合成输出 | S |
| CUE 子曲目、帧边界和精确定位 | `004E323B`、公共 reader/seek 路线 | `audio_engine.cpp:1645`、`:1814`、`:1875` | 已接通。解析序号、跨 FILE 边界、较早 reader seek 后补读与裁剪；这是 A03 可抽取的既有实现 | S/H |
| CD 设备/TOC/区间/短读重试 | `004E778B`、`004DEE53`、`004EF94C`、`004E7E0F` | `cda_source.cpp:14`、`:48`、`:143`；`disc_media.cpp` | 核心功能已接通，底层使用 RAW_READ 而非原 SCSI READ CD，见 A07 | S/H/U |
| CD 48 扇区预读、DTS 探测/解码 | `004E778B` + ttpcomm ordinal106 | `cda_source.cpp:74`、`:78`、`:95` | 已接通；DTS 后接公共 Decoder。实体 DTS 光盘、坏盘/热插拔仍 U | S/H/U |
| CD 属性描述后备、本地 CDDB | `004E743C`、`004DF142/004DF1EC`、`004E7675` | `disc_media.cpp:148`、`:169`、`:219`；`builtin_file_info.cpp` 的 CDA 分支 | 已接通。本地标签写 XML，不能说写入光盘。属性 UI 由数据分工核对 | S/H |
| CD 网络查盘 | `0043840A`、`004381B2/00438241` FreeDB | `disc_lookup.cpp:127`、`:176`、`:186` | MusicBrainz 是用户授权替代/扩展，不是逐请求复制过期 FreeDB 服务 | S/H |
| 转换配置/进度/取消 | 对话框220/221；`004122CD`、`00412A8D` | `playlist_transforms.cpp:905`、`:978`、`:1129`、`:1504`、`:1706` | 已接通，单个转换任务窗口、暂停/取消检查、逐曲错误 | S/H |
| 离线 RG/EQ/环绕/SSRC | `004122CD → 004B1375` | `playlist_transforms.cpp:767`、`:1586`、`:1649` | 已接通，先源效果后 SSRC、保留 AddIn float64 输入；不自动加播放用全局 Winamp 插件 | S/H |
| Wave 与 LAME 编码 | 原内置 Wave `004CA3BE`，LAME Blade 接口 | `file_encoder.cpp:84`、`:133`、`:176`、`:200` | 已接通；RIFF 32位大小上限被明确拒绝，Unicode 无损路径检查。RF64 是未来扩展，不是已恢复 | S/H |
| AddIn 编码器配置、Start/Write/Finalize | 原 encoder 公共 factory、`004122CD` | `plugin_manager.cpp:1878`、`:1912`、`:1920`、`:1938`；`playlist_transforms.cpp:1540`、`:1627` | 已接通。Nero 原 DLL、CLI encoder 等不表示宿主可保证其内部线程/取消 | S/H/U |
| 转换元数据与失败保护 | `004121C4`、内部 `00412575/004125C1` | `playlist_transforms.cpp:1437`、`:1483`、`:1675` | 已接通并加强：先私有临时目录，完整成功后替换，保护本批源文件；不传播旧 RG 到新音频 | S/H |
| CDA 抓轨到编码器 | CD Reader + `004122CD` 通用转换 | `CreateDecodedAudioSource` → `cda_source.cpp` → `ConvertPlaylistTrack` | 已接通共用 PCM 管线；无物理 CD 不能声称真实抓轨成功 | S/H/U |
| 网络源转换设置与取消 | 原通用 Reader/转换管线 | `playlist_transforms.cpp:1567`–`:1580`；`src/ui/audio_options.h:9` | 已接通。代理设置传入，stop_token 合并取消，错误保留目标原文件 | S/H |

## 6. 网络/缓存与音频交接矩阵

| 功能 | 原版证据 | 当前入口／源码 | 状态与边界 | 证据 |
|---|---|---|---|---|
| HTTP(S) IStream 解码 | `004E8AAD` / `CreateStreamOnInet` 公共边界 | `http_stream.cpp:584`；`audio_engine.cpp:1509` | HTTP 路线已接通；HTTPS 是 ttp_https 扩展，XP 不依赖 MF 才能取字节 | S/H |
| 旧非 HTTP provider、MMS | `004E8AC3`、`004E8BD6` 的 NetShow/FileSourceFilter | `directshow_source.cpp:523`；`audio_engine.cpp:1598` | 已恢复选择和配置/取消；没有现成有效 MMS 服务的端到端验证 | S/H/U |
| 代理、认证与取消边界 | `004E8BD6` 原系统 provider 配置 | `directshow_source.cpp:21`、`:42`、`:590` | 已接通受支持配置；旧 NetShow 无 credential setter时明确 E_NOTIMPL。第三方忽略 Abort 可继续阻塞，见 A07 | S/H/U |
| ICY/live MPEG、长度未知流 | 原 MPEG/network reader 功能，网络子链实现不同 | `http_mpeg_source.cpp:36`、`:71`、`:142`；`http_stream.cpp:409` | 已接通，live seek明确失败，动态标题和有界缓存；未证明任意电台内容等价 | S/H/U |
| 独立预取、Range、有限续传 | 原缓存/下载协调函数 `004CFB25`、`004CF648`、`004D80DE` | `http_stream.cpp:54`、`:128`、`:158`、`:390`；`http_cache.inl:79`、`:144` | 已接通。部分记录有哈希和来源验证；不能把旧文档“没有部分续传”再列为现状 | S/H |
| 前台/下载共享缓存任务 | 同一网络/缓存体系 | `http_stream.cpp:525`–`:540`、`http_download_queue.cpp:255` | 已接通，consumer各自取消，profile参与任务身份；本轮未重跑完整断网矩阵 | S/H |
| 旧 cachelist/完成文件复用 | `004CFB25`、`004CF648`、`004D80DE` | `http_download_queue.cpp:112`、`:118`、`:343` | 已接通；包括已完成 `.bin` 让 Reader识别，不再按扩展名硬拒绝 | S/H |
| 音乐窗专属 netacc/P2P/网页下载集成 | 原音乐窗流程及旧服务 | 本轮不恢复 | **用户排除项**，不能作为本轮需要补做的 UI/协议缺陷 | S |

## 7. 本轮确认的缺口与建议顺序

### A01：手动 ReplayGain 扫描截断扩展格式后越界读取（P1，S）

**当前路径**

1. `replay_gain_scanner.cpp:375`：
   `const WAVEFORMATEX format = segment ? segment->OutputFormat() : reader->Format();`
2. 该赋值只复制基础 `WAVEFORMATEX`（Windows 布局 18 字节）。原对象若是 `WAVEFORMATEXTENSIBLE`，复制结果仍保留 `wFormatTag=0xfffe`、`cbSize=22`。
3. 第403行把基础对象交给 `ConvertSamples`；第125行进入 ttpcomm PCM Decode。
4. `ttpcomm/sdk/include/ttpcomm/pcm.h:29`–`:37` 根据 tag/cbSize 转成 `WAVEFORMATEXTENSIBLE`，读取 SubFormat GUID、valid bits、channel mask；这些字段已不在被复制的局部对象内。

**结果和可达性**

合法 EXTENSIBLE PCM/float source 会出现对象边界外读取，通常可能被判为不支持，也可能受邻接栈内容影响。这里不需要损坏文件。`LegacyReaderSession::Format()` 返回的原始完整格式存储在 `plugin_manager.cpp:1415`；系统/DMO 输出、多声道合法扩展格式都可以到达此路径。

原 `004A4E8C` 经公共 reader/format 管线扫描，并没有这个 C++ 基础结构值切片。已经通过的普通 PCM 扫描不覆盖这条扩展结构路径。

**建议**

保留生命周期覆盖扫描循环的 `const WAVEFORMATEX&`，或使用真正拥有整个 `sizeof(WAVEFORMATEX)+cbSize` 的格式对象。不要只清空 cbSize，那会丢掉子类型和有效位。扫描转换入口应共同采用完整格式约定。

**验证**

用私有 fake Reader 返回完整的 EXTENSIBLE PCM24-in32、6声道 PCM、IEEE float；分别经过 reader 和系统 source 分支。记录正确 RG/peak，与同 PCM 的基本格式版本比较。增加紧邻不可访问页的格式结构或边界检查探针；此轮没有运行该本机异常探针。

### A02：DirectSound／WASAPI 补缓冲恢复水位不可达（P1，S+M，设备 U）

原 `004AC605` 确实有低水暂停和约80%恢复，见伪代码163735附近；但原 `004AB1C4`（162500–162514）以 Reader 小包为粒度计算缓冲数量，至少4个并按4对齐。重建版把总时长切成固定4个大块，这改变了可达状态。

当前：

- DirectSound：`audio_engine.cpp:2795`–`:2805` 固定4段；`:3013`–`:3045` 低水暂停、`resume_water=ring_bytes*8/10`；`:3128`–`:3132` 只能在剩余空间足够一个整段时填充。
- WASAPI：`wasapi_sink.cpp:174`–`:178` 设置低水/恢复水位；`:269`–`:276` Stop后等恢复。播放器 `audio_engine.cpp:3185`–`:3192` 同样固定4块，`:3607`–`:3629` 必须等整块真实播放完才允许复用。

**具体可达组合**

48kHz、输出缓冲2000ms，4块各500ms；慢解码/归档/网络导致设备停止时剩余80ms。来源恢复后可填3块，总就绪量：

`80 + 3 × 500 = 1580ms < 1600ms 恢复水位`

DirectSound剩余空间420ms，无法填500ms整段；WASAPI播放器的未完成80ms块仍占一个槽位，另3槽已补满。设备已经停止，最后80ms不能完成，于是没有可继续填充或恢复播放的动作。除非 EOF、seek/reset或重新播放打破状态，否则一直等待。

本轮 `capture_evidence.py` 的4个模型见 `evidence.json`：2000/10000ms配置和1000ms配置残余30ms存在此状态；1000ms残余80ms则可以恢复，作为“不夸大成每次断流都挂”的反例。它只执行算术和状态约束，不调用 DirectSound/WASAPI。

**建议**

使恢复水位与可提交粒度一致。可以恢复原版“较小处理包＋多个包”的协调模式，或者支持填充不足一整段的剩余空间，或者将恢复条件限制在不需要未完成块继续播放的可达上限。不要单独把等待超时后重开设备当作正常恢复机制。

**验证**

注入解码暂停后恢复，扫描100–10000ms缓冲、块边界±1帧、残余0–100ms、EOF短尾、暂停/恢复与取消。先模拟端点时钟断言最终能继续，再用真实 WASAPI/DirectSound 回归。本轮没有播放音频。

### A03：普通播放的粗粒度 Seek 缺少前缀丢弃（P2，S）

原 `004B16FE` / 168816–168835 将 Reader 回写的时间与请求比较，较早时把差值换成帧对齐的 PCM 字节数存于 `+0x84`；`004B1375` / 168552–168582 在后续读包中逐步丢弃这些前缀，然后才做 RG/EQ/SSRC。

当前 `LegacyPluginSource::Seek`（`audio_engine.cpp:804`–`:815`）正确保存 Reader 回写值；但 DirectSound（`:3075`–`:3111`）和 WaveOut/ASIO/KS/WASAPI公共路径（`:3554`–`:3582`）只调用 Seek，再从请求时间发布时钟，既不使用 LastSeekPosition也不丢前缀。`LegacyReaderSession::Seek`（`plugin_manager.cpp:1407`）仅调用Reader并reset Decoder，没有隐藏的补偿。

这是实际可达合同：

- `ttp_aac/src/reader.cpp:420`–`:429` 的 MP4 上界搜索退到目标前一 sample，再回写该 sample时间。
- 同文件 `:440`–`:446` 的 ADTS 也回写较早 AAC frame时间。
- 例如请求落在AAC帧中部，实际输出从帧头开始，而当前播放器时钟已按请求时间计，造成声音/歌词/后续seek基准偏差。

`CueSegmentSource::PositionAtByte`（`audio_engine.cpp:1875`–`:1937`）已实现“读回实际位置、必要时回到更早点、丢到精确字节位置”。不能再报“CUE完全不支持精确定位”，正确问题是普通路径没有该共用层。

**建议**

把“定位实际时间与前缀丢弃”放到公共 decoded-source/processor边界，裁剪发生在效果之前；保留Reader自身seek返回语义。复用CUE经验但避免双重丢弃CUE已经裁好的音频。若Reader返回更晚位置，要明确策略而不是伪造精确成功。

**验证**

用可预测PCM的fake reader固定回退100ms和逐帧实际AAC样本，检查seek后的首个输出样本与未seek顺序解码在目标处相同；覆盖暂停seek、EQ/SSRC、多个小读包、EOF、无 LastSeekPosition、CUE内层。此轮未新跑解码探针。

### A04：通用 ACM Decoder 尚未恢复（P2，S）

`004C9C1F` 在原版中独立于 MPEG decoder，尝试 EXTENSIBLE/basic格式、float→integer、必要时16位回退，并调用 `acmStreamOpen/acmStreamSize`。`004CA48E` 中 `004CA0A0` 注册它。

当前 `PluginManager::OpenDecoder`（`:2815`）只增加一个内置 MPEG candidate，后面都是外部decoder工厂；没有对应通用ACM对象。外部 reader产生只能由已安装ACM codec解码的压缩WAVEFORMATEX时，宿主没有原版那条最后路线。

**限定**：不能据此说“全部ADPCM/WAV都不能播放”。本地文件仍可能被 DirectShow/Windows Media 解码；这与公共AddIn Decoder ABI缺失不是同一结论。

建议先恢复独立creator/decoder及buffer/flush/reset合同；用不需要实体设备的系统PCM/ADPCM小样本和fake compressed Reader验证回退次序、format容量、尾部、失败后释放。不要用系统最终Reader代替所有插件的Decoder角色。

### A05：普通本地 MPEG Reader 仍是系统替代路径（P2／一致性，S+H，差异幅度 U）

内置 MPEG Decoder 的恢复已经完成，但 Reader 与 Decoder职责不能混淆。

当前 `RecoveredFileSource::Open` 对普通本地 MPEG 的后备调用 `WrapMp3ProSource(system_source())`（`audio_engine.cpp:1368`–`:1379`）；`system_source` 在有MF系统用MF，否则用Windows Media。新 `builtin_mpeg_decoder.cpp` 只有在一个Reader已经交出压缩MPEG格式并请求公共Decoder时才发挥作用，例如ASF内MP3。

原 `004E4B71` 等负责MPEG帧/定位信息，后接内置MPEG Decoder。当前系统后备可以正常播放，但头尾裁剪、错误帧容忍、编码器delay/padding、定位和格式报告不能据名称宣称原样等价。HTTP专用MPEG source是另外一条重建路径，不能作为本地路径已恢复的证据。

建议在A03之后决定是否补内置本地MPEG Reader，逐项对照原版读取/索引/ID3/Xing/VBRI/尾部和gapless政策，再复用现有Decoder。先用私有语料测差，不要为了“走同一DLL”直接删除已有系统后备。真正mp3PRO SBR样本仍单独待验证。

### A06：KS 拓扑音量和过渡淡化仍为部分实现（P2，S，硬件 U）

原 `004E061B` 实际探索设备拓扑中的音量节点。当前 KS transport只负责pin/状态/packet/position；`audio_engine.cpp:3395`–`:3409` 将用户音量、balance和末尾gain烘焙到新包，不执行显式play/pause/seek fades。

这不是“KS从未实现”：mono、unsigned8、位宽候选、通配range、可选topology、异步取消和稳定设备身份都已实现并有历史模拟测试。真正差异是音量生效时机和可用节点控制。

建议有真实支持节点的声卡时再恢复node控制和能力展示；无节点保持清楚的软件回退合同，不伪称具有实时硬件淡化。包已提交后再改变音量无法追改已发送数据，需要基于输出时钟设计，不能只删除当前fade限制。

### A07：明确的替代实现与未验证边界（P3，S/H/U）

1. **CD transport**：`cda_source.cpp:19`、`:31` 使用TOC/RAW_READ；原 `004EF94C/004DEE53` 是NT SCSI READ CD路线。普通功能已恢复，某些驱动/坏盘/读取模式的兼容差异仍需硬件；不因没有物理CD而杜撰失败。
2. **NetShow/MMS**：`directshow_source.cpp:42` 正确marshal接口并Abort；第三方provider若忽略Abort，operation及join仍可阻塞。MF默认fallback也不提供同样的严格取消时限。明确profile认证不支持时返回错误是安全差异。
3. **ASIO驱动重置**：当前把reset/rate变化转为可见工作线程错误；没有证明能与全部驱动无缝恢复。历史模拟的353项不是实体声卡发声/外部时钟测试。
4. **mp3PRO**：历史真实DLL试验只有非增强MPEG-2静音回退，增强PCM来自模拟DLL；真正SBR音质不能声称验证。
5. **转换外部encoder**：取消检查位于宿主调用之间；单个第三方Write/Finalize永久阻塞不被stop_token强行终止。现有事务保证目标不提前覆盖，不保证第三方代码可取消。

## 8. 可简化与复用的地方

| 建议 | 当前重复点／具体依据 | 实施边界 |
|---|---|---|
| 统一源包处理、元数据刷新和EOF排空协调 | `audio_engine.cpp:2897` 与 `:3317` 两套decode/fill分别跑 source→processors→transform | 公共生产PCM层保留；DirectSound ring与WAVEHDR/native队列仍应各自管理，不强塞成同一种设备 |
| 共用水位/粒度计算 | A02两个后端都复制80%策略，但缓冲粒度是播放器决定 | 先建立可达水位合同和模型，再替换重复常量；不仅重命名函数 |
| 共用精确decoded seek | 普通source只保存LastSeekPosition，CUE `PositionAtByte`独自补齐 | 抽取帧对齐、前缀丢弃、EOF/失败处理；避免CUE双裁剪 |
| 完整音频格式的拥有者/视图 | A01基础结构切片；`PcmSourceEffects`已经完整复制，`PcmOutputTransform`已经显式保存valid_bits | 全链明确“借用完整格式”与“拥有规范化PCM描述”，别继续到处按值复制WAVEFORMATEX |
| MPEG帧头校验复用 | `format_probe.cpp:11`、`http_mpeg_source.cpp:14`、`mp3pro_source.cpp:27`都有不同用途的MPEG位域逻辑 | 可共用合法头解析；保留strong探测需相邻帧、live变格式、mp3PRO准入各自政策，不把三者限制混同 |
| Decoder标准壳容纳MPEG/ACM/外部工厂 | `plugin_manager.cpp:1687`已经承担pending、容量和reset，新增ACM可复用同一session | 内核ABI不同仍各自隔离；不要在每个音频格式项目再复制宿主状态机 |
| 保留已完成的共用效果与设置快照 | `pcm_effects.cpp`、`src/ui/audio_options.h:9` 已共用 | 这些是已实现简化，不需要“重新统一”而再造一个平行实现 |

## 9. 验证记录与后续测试矩阵

### 本轮实际执行

```powershell
python rebuild/tests/all_feature_audit_20261020/audio/capture_evidence.py
```

输出：当前HEAD匹配；86个原版入口定位；4个补缓冲状态模型，3个不可达恢复状态、1个可恢复反例。脚本只读取源码、哈希和Git HEAD；写本私有目录的 `evidence.json`。没有编译、注册DLL、修改系统音量或播放音频。

### 仅引用的历史验证

- `tests/player_gap_fix_20261019/audio/AUDIO_FIX_RESULT.md` 与同目录 `unit.log`、`original-effects.log`、`http.log`、`xp.log`、`win7.log`：自动位深、64组Winamp量化桥、源效果、SSRC尾部、KS mono/8bit、旧provider取消和`.bin`复用；网络转换loopback主要在Win11。**本轮未重跑。**
- `docs/ASIO_KS_COMPATIBILITY_20261019.md`：Win11/XP/Win7模拟ASIO 353检查、KS包生命周期、native身份，以及55项选项稳定键。**模拟设备不等于实体专业声卡。**
- `docs/BUILTIN_MPEG_DECODER_20261019.md`：帧分块/EOF/reset、ASF内MP3及三系统集成。**本轮静态确认源码存在，未新做PCM对照。**
- `docs/AUDIO_FORMAT_PROBE_IMPLEMENTATION_20261020.md`：三系统99检查和Win11更名交互。**不是本轮重新执行。**
- `docs/CDA_MUSICBRAINZ_IMPLEMENTATION.md`：虚拟TOC/sector、短读、错误和查询数据；没有实体CD。
- `docs/MP3PRO_RECOVERY.md`：模拟增强及实际DLL普通MPEG回退，SBR真实样本缺失。

### 建议新增、尚未执行

| 测试组 | 必须检查的输入/状态 | 主要判据 |
|---|---|---|
| A01格式生命周期 | EXTENSIBLE24-in32/float、2/6/8声道、Reader与source、结构靠近不可访问页 | 无越界；与相同PCM基本格式同RG/peak；异常不污染已完成结果 |
| A02补缓冲 | 100/500/1000/2000/10000ms、残余0至100ms、每个边界±1帧、来源恢复/EOF/取消 | 任何合法有限恢复输入均有进展；停止时钟不会让恢复水位永久不可达 |
| A03定位 | AAC/ADTS帧内目标、假reader回退、多个小读包、暂停/恢复、EQ/SSRC、CUE | 首输出样本对应请求位置；时钟/歌词一致；CUE不双裁剪 |
| A04 ACM | 系统已有的PCM/ADPCM转换及fake compressed Reader、拒绝格式、float→integer后备、reset/EOF | 保留工厂尝试顺序；失败不留下对象/stream；输出容量正确 |
| A05本地MPEG | CBR/VBR/Xing/VBRI、ID3前缀、坏帧、delay/padding、ADU、格式变化 | 原版与现系统路径差异先量化，再决定精确恢复；不以“可播放”替代样本证据 |
| Native实体硬件 | 同一x86驱动、ASIO/KS、44.1/48/96k、8/16/24/32、mono/2/6/8、设备占用/拔除/外部时钟 | 可复现诊断、首尾无重复/丢帧、定位及关闭完成、音量节点行为可解释 |
| CD实体硬件 | 普通CD、DTS-CD、短读/坏盘/换盘、抓轨取消 | 不拼接不同盘数据、边界/重试/错误正确；验证RAW_READ替代路径兼容性 |
| 第三方阻塞 | 不配合Abort的provider/encoder，超时及用户退出 | 不夸大宿主可控取消；如增加隔离需独立设计与授权，不使用TerminateThread |

所有新增源码、样本和证据继续留在 `rebuild/tests`，不上传测试、不让Action执行。用户已用Windows11替代Windows10测试；这仍不能写成“真实Win10已验证”。本报告不声称全EXE所有分支动态等价。
