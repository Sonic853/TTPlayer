# 音频插件后续修复与原版多声道输出定位

本记录接续 `AUDIO_PLUGIN_COMPLETION_20261024.md`，区分插件修复、原版宿主缺陷和未覆盖边界。测试代码和样本仅存放在 `rebuild/tests/plugins_followup_20261024`，不上传，不在 Actions 执行。

## 1. 原版 Win7／Win11 的 5／7 声道失败原因

结论：本轮确认的失败来自原 EXE 的输出格式构造，不是 DTS 解码失败，也不是新版插件特有的回归。用原始 `AddIn/ttp_flac.dll` 播放相同的 5／7 声道 FLAC，在 Win11 同样失败。

### DirectSound 路径

原版 `004C3C86` 先提交普通 WAVEFORMATEX；失败后调用 `004C3000` 构造 WAVEFORMATEXTENSIBLE 重试。`004C2FD5` 只为 1、2、4、6、8 声道返回掩码，3、5、7 声道返回 0。即使插件正确提供了原始声道布局，该输出路径也不会保留它。

因此，偶数声道“能够开始播放”也不能证明原版保留了所有扬声器位置：例如 6 声道的 side/back 差别、8 声道的不同布局可能被原版固定表覆盖。接口 PCM 正确、宿主播放进度正常和实体多声道扬声器路由正确，是三个不同的验证层次。

隔离原版进程的实测：5 声道 FLAC 进入 `004C3C86` 时为 PCM、48 kHz、16-bit、blockAlign=10，最终返回 `0x88780064`（DSERR_BADFORMAT），随后未开始读取播放数据。

独立系统 API 探针在 XP、Win7、Win11 分别检查 1–8 声道：

| 格式 | XP 本次设备 | Win7／Win11 本次设备 |
| --- | --- | --- |
| 多声道普通 PCM 头 | 拒绝 | 拒绝 |
| 扩展 PCM、掩码为 0 | 接受 | DirectSound 拒绝 |
| 扩展 PCM、正确掩码 | 接受 | 接受 |

这解释了上轮同一个插件在 XP 可以播放、Win7／Win11 失败的系统差异；不是对所有声卡和驱动的普遍保证。

### WaveOut 路径

仅切换到 WaveOut 也不能解决该原版问题。`004E24B6` 使用同一个 `004C3000`，而后者把 `wValidBitsPerSample` 写成 0。Win7／Win11 的实测 `waveOutOpen` 拒绝该格式（WAVERR_BADFORMAT）；把有效位数改为 16 后，同一探针可以创建。XP 本次设备接受有效位数为 0 的扩展头。

原版实际切换 WaveOut 后，5／7 声道 FLAC 和 DTS-HD 测试仍无播放进度；调试记录显示 `004E24B6` 返回 `E_FAIL`。不能把“切换 WaveOut”写成已经验证有效的解决办法。

### 实施边界与兼容策略

重建版的输出链已保留扩展头、有效位数和真实声道掩码，不需要复制上述原版缺陷。解码插件继续输出真实声道与精度。

原版 EXE 的彻底修复需要修改宿主输出层：保留 Reader 的布局信息，在位深转换后同步更新有效位数，并把完整扩展头传入设备。它不能仅靠插件 `Format()` 修复，因为原 EXE 随后又重建了输出头。没有修改用户原版 EXE、注入运行补丁，或自动填充虚构声道／降为立体声。

可选的 5→6、7→8 静音声道补齐需要显式兼容选项、按扬声器位置重排，并校验原版固定掩码；仅补尾部零值会把 side/back 声道放错。当前不采用这种默认行为。需要这些格式时，已验证可用的路径是重建版；原版相关输出限制继续明确保留。

### 证据

- 原 EXE SHA-256：`c7999ea5823469c1684148cac1bd196009824348ccac5cdd45978f8fb574e28c`。
- REA 6.5.0／Ghidra 12.1.3，`004C3C86`：`ev_b6d7b0ae2b5ae7f4543b9f5b0aa7ac7bdc7b569ce4012bdd2e0260a6832121ad`。原始记录保存在私有测试 `host/original-dsound.json`；静态伪代码不等于执行证据。
- 其余函数对照既有 `reverse/decompiled/TTPlayer.exe.pseudo.c`。动态记录：`host/original-trace.log`、`wave-trace.log`、`old-plugin-Win11.json`、`wave-Win11.json`。
- 三系统 API 探针：`host/output-XP.log`、`output-Win7.log`、`output-Win11.log`。分别记录查询结果和真正打开结果，避免把仅查询成功当成可以播放。

## 2. 本轮实际修复

### AAC 多段编辑列表

非碎片 MP4 的 `elst` v0/v1 现在支持顺序片段、跳跃、倒序、重复，以及首／中／尾空编辑静音。按累计 movie 时间换算采样位置，避免每段单独取整造成时长漂移。SMPB 只能细化相同的连续范围，不再覆盖明确的剪辑。标签保存保留编辑列表。

变速、驻留和不支持的 composition 时间线返回明确错误。反向编辑可能从头恢复 AAC 重叠状态，不使用近似定位。见 [AAC 详细说明](../../ttp_aac/docs/EDIT_LIST_COMPLETION_20261024.md)。

### RM／VQF 定位取消与缓存

修复定位过程中取消或底层 Seek 失败后丢失未播放 PCM：失败后恢复到调用前发布的位置，恢复期间再次取消也保留该目标。定位时已解出的 PCM 纳入既有 4 MiB 缓存，跳到远处后附近回退可以零源读取。

没有把容器包索引等同于解码器状态。Cook／TwinVQ 的重叠、增益、LSP／Bark 预测等状态仍需恢复；缓存外继续准确重放。见 [RM](../../ttp_rm/docs/SEEK_RECOVERY_20261024.md)、[VQF](../../ttp_vqf/docs/SEEK_RECOVERY_20261024.md)。

### DTS Express 时长

取得 AndroidX Media 和 dcadec 公开真实 LBR 样本，确认 stereo／5.1 Express 进入实际 LBR 解码器。修复非 Core raw DTS 使用非零码率估算时长时跳过精确扫描的问题：样本从错误的 949 ms 修正为 938 ms，45,056 帧／48 kHz 的 PCM 未变。沿用四百万包上限，远程／不可定位流不全文件扫描。

完整 stereo 样本 8,810,496 帧／183,552 ms 解码通过；三个短输入完成精确定位和取消恢复，并回归 MA、HRA、Core。见 [DTS Express 验证](../../ttp_ac3dts/docs/DTS_EXPRESS_VALIDATION_20261024.md)。

### TTA 与其他历史格式

新增 float32 特殊值和随机位模式、1／2／6／8 声道及跨帧定位对照，原 DLL 与重建 DLL 的 PCM 相同，无需修改该解码代码。

官方整数 TTA 范围为 8／16／24 位；FFmpeg 的 S32 输入实际编码为 24 位，不能作为原生 int32 支持证据。真实历史 float32 存档仍与本地生成的边界样本分开标注。见 [TTA 边界验证](../../ttp_flac/docs/TTA_FLOAT_BOUNDARIES_20261024.md)。

本机补测真实 UMX、fine effects XM、double tone-portamento XM：原／新 DLL 的时长和 EOF 长度一致，均能定位；但 PCM 仍有明显差异，最大整数差分别约 18,270、4,315、4,317。重复运行已排除“全部只是原 DLL 随机抖动”的解释，尚需拆分到音序效果、声像、重采样及混音状态。未改 MOD 内核，未宣称完全一致。见 [MOD／UMX 对照](../../ttp_mod/docs/UMX_EFFECT_FOLLOWUP_20261024.md)。

ASF 的 WMA2、24-bit WMA Pro、24-bit Lossless 5.1、MP3-in-ASF 四类本机 DMO，原／新 PCM 相同；小容量、部分输出、拒绝不足一帧容量后的重试和 Reset 均通过。同语言重复标签完整枚举、修改无关字段时全部保留；旧 `Set(name,value)` 仍替换所选语言整个同名组，没有逐项多值编辑参数。见 [ASF DMO／多值对照](../../ttp_asf/docs/DMO_MULTIVALUE_FOLLOWUP_20261024.md)。这部分为本机 Windows 11 系统 DMO 检查，不推定 XP 安装了相同 DMO。

### OGG 顺序流、失败恢复和声道排列

修复“Seek 能调用但长度未知”时错误启用随机访问的判断：Opus 不再提前截短，Vorbis 不再在开流时挂起。无可靠长度的流采用顺序读取，时长暂为 0，Seek 返回 E_NOTIMPL；不把未知长度误报为完整时长。

读取取消前已经消耗的字节保留到 Start 恢复；定位失败会重新打开解码器并回到失败前已交付的 PCM 位置。若回滚也失败，暂停读取直到成功的显式 Seek，避免从未知位置继续出声。错误码不被 EOF 覆盖。

补齐 Vorbis 3／5／7／8 声道到 Windows 顺序的重排，修复 PCM 顺序与声明掩码不一致。1／2／4／6 声道的 PCM 保持不变；9 声道继续保留应用定义的顺序和未知掩码。独立 FFmpeg 全帧参考误差不超过 `1.1920929e-7`。见 [OGG 完整记录](../../ttp_ogg/docs/STREAM_BOUNDARIES_20261024.md)。

### RM 原版历史后端对照

补测八类 RealAudio 样本，按原版要求设置回调、流扩展和旧后端运行库，并分别处理压缩 Reader 后接 AAC／AC3／MPEG Decoder 的路径。六类能够形成原版 PCM 的样本，声明时长相同；新版本七类正常输入的定位后尾部均精确对应自身顺序解码，原版通常落到更早的容器索引点。

共同区间统一量化后，AAC 和 MP3 ADU 的差异不超过 1 LSB；Cook、ATRAC3、DNET 仍存在实际 PCM 差异，SIPR 存在相对时移。Cook 多出 1,024 帧为全零，但共同音频也不同；AAC 多出的尾部包含非静音，不能为模仿原版提前 E_FAIL 而直接截除。已用原版推荐缓冲容量重读 Cook／ATRAC3，排除大缓冲探针造成差异。RA28.8 缺少原后端，RA14.4 原版 Open 失败／样本截断，均未记作一致性通过。

这部分仅为 Win11 的接口对照，MP3 ADU 调度使用复原宿主 wrapper，不冒充原 EXE 整机验收。见 [RM 详细对照与后续顺序](../../ttp_rm/docs/ORIGINAL_DECODER_COMPARISON_20261024.md)。

## 3. 本轮验收与发行

### 三系统验收

使用 XP SP3 x86、Win7 SP1 虚拟机与 Windows 11 本机。依照用户要求，以 Win11 代为现代 Windows 验证；没有独立 Win10 实测。以下均针对本轮最终 DLL，不借用旧版通过结果。

| 验证组 | 每系统顶层用例 | XP | Win7 | Win11 |
| --- | ---: | --- | --- | --- |
| AAC 编辑列表、旧 Profile／裁剪、标签保存 | 48 | 通过 | 通过 | 通过 |
| OGG 流边界、取消、定位回滚、声道 | 89 | 通过 | 通过 | 通过 |
| RM／VQF 定位与故障恢复 | 45 | 通过 | 通过 | 通过 |
| DTS Express 解码、定位与取消 | 9 | 通过 | 通过 | 通过 |
| TTA 历史 float32 位模式、跨帧定位 | 56 | 通过 | 通过 | 通过 |
| 原版／重建版实际宿主播放 | 12 | 通过 | 通过 | 通过 |

RM／VQF 的 45 次运行内执行 243 个子场景，另有 44 个因未到达 I/O 而明确跳过的注入分支，不能算作已注入故障。AAC 的预期拒绝项核对退出码及 HRESULT，其他合法项与期望 PCM 用 `fc /b` 比较。TTA 的 56 次解码／定位另各做一次字节比较。OGG 的超大长度边界使用虚拟 Stat／Seek，没有实际下载或创建超大音频。

宿主测试覆盖 AAC 多段编辑、真实 DTS Express 5.1、Cook RM、VQF、Vorbis 和 Opus，各使用隔离原版与重建版。核对加载的 DLL 路径、无错误对话框、播放时间前进和自然退出；AAC 连续运行超过 10 秒，越过回跳与中间静音。测试以静音输出运行，不等于实体扬声器的听音及路由验收。原版 5／7 声道限制仍按第 1 节单列，未计为播放通过。

MOD／ASF／RM 的扩展原版 PCM 对照仅在 Win11 完成，未扩大为三系统结果。五个修改插件均通过 XP／Win7 静态导入检查。所有本地测试仍只放在 `rebuild/tests`；没有加入 Actions。

### 发行内容

本轮更新 `ttp_aac`、`ttp_ac3dts`、`ttp_ogg`、`ttp_rm`、`ttp_vqf`，版本统一为 **2026.10.24p1**，输出到 `rebuild/build/Release`。每个运行 ZIP 只包含 `AddIn/ttp_名称.dll` 和 `SHA256SUMS.txt`；AAC／AC3DTS／RM／VQF 的对应源码沿用独立 `-source.zip`。许可证保留在仓库／对应源码包，不嵌入 DLL。

同时更新该目录 `AddIn` 中的这五个 DLL。没有改动的插件、播放器 EXE 和原 `2026.10.24` ZIP 保持各自版本。`AUDIO_PLUGINS_20261024p1_SHA256SUMS.txt` 记录本轮五个 DLL、五个运行包及四个源码包，安装时应以所选 ZIP 内的清单核验。

## 4. 仍保留的差异

后续更新：[2026-10-29 修复与验收](AUDIO_PLUGIN_FOLLOWUP_20261029.md) 已进一步处理真实 UMX、Cook／DNET 补噪、SIPR 16k 和缓存错误状态。[2026-10-25 修复与验收](AUDIO_PLUGIN_FOLLOWUP_20261025.md) 已处理 AAC 呈现时间、ATRAC3 起始延迟与极性、MOD 混音兼容，并完成 OGG 真实 HTTP 测试。以下是本页原轮次的历史边界，不能作为新版本仍未修复的完整清单。

1. **原版输出层：** Win7／Win11 下的 3／5／7 声道掩码，以及 WaveOut 有效位数问题，需要原宿主输出代码改动；解码插件无法保证宿主保留布局。没有把降精度／虚构声道作为默认修复。
2. **MOD／RM 完全一致性：** 真实 UMX 与部分 XM 效果存在 PCM 差异，需逐行／逐 tick 分离音序命令、声像、重采样和混音状态。RM 的 ATRAC3、DNET、Cook 历史后端及 AAC 提前结束也需继续定位；不能为追求帧数一致而盲目裁掉有效声音。
3. **精确但可能较慢的远距离定位：** RM／VQF 缓存外、AAC 反向编辑仍需要恢复解码历史；容器索引不能替代预测／重叠状态快照。
4. **尚未穷尽的格式分支：** AAC 碎片／变速／复杂 composition 编辑及历史 Profile、DTS Express 其他布局／选项、真实历史 float32 TTA 存档仍待覆盖。官方 TTA 整数范围为 8／16／24 位，不把 S32 输入转为 24 位的样本当成原生 32 位证明。
5. **网络与系统依赖：** OGG 确定性 IStream 故障测试不能替代 HTTP 重连、Range、异步 E_PENDING 和真实超大流；ASF 系统 DMO 可用性取决于实际系统安装，旧 ABI 也不提供同名多值标签的逐项编辑参数。

这些边界应继续留在差异清单中，不能据本轮通过项宣称所有插件与原版完全相同。

## 5. 最终 DLL 摘要

| 插件 | 字节 | SHA-256 |
| --- | ---: | --- |
| ttp_aac.dll | 395776 | `3d6d738ff0e5b23dded84615e304257a94fb8f5b4f67359331b8703f003b250c` |
| ttp_ac3dts.dll | 1006080 | `d563b6abdd2f9b87e4656d94b87a0aa45685293fe15ce1aea923048e8b3c5c31` |
| ttp_ogg.dll | 356352 | `719318bc8b9d03efedd00beba93c4e3cd0229d814696992f0610a14a99c4300d` |
| ttp_rm.dll | 952320 | `bccc7e826e52bd78e54c8a235487e2e7f64d3e90915eb116af016642e390beb4` |
| ttp_vqf.dll | 803840 | `3b061a12b07a41d5961ff220b7dbdb5412505ff74caa1b0421c2f2cd8ce5b741` |

运行 ZIP 内 DLL、构建目录 DLL、Release/AddIn、隔离宿主 DLL，以及从 XP／Win7 测试目录取回的 DLL 均逐一核对 SHA-256 相同。MSVC 打包时重链接产生的时间戳变化没有替换已验证产物；确认仅 COFF／Debug 时间戳有差别后，发行包使用测试通过的原始 DLL 字节。
