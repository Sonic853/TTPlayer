# 音频插件修正、库更新与八个独立重建项目

最新进展见 [2026.10.29 后续修复与验收](AUDIO_PLUGIN_FOLLOWUP_20261029.md)：真实 UMX、Cook／DNET 补噪、SIPR 16k 与缓存回放错误恢复，已完成 XP／Win7／Win11 验收。前一轮结果见 [2026.10.25](AUDIO_PLUGIN_FOLLOWUP_20261025.md)。以下保留当时版本的记录与摘要。

后续补全与新增边界见 [2026-10-24 报告](AUDIO_PLUGIN_COMPLETION_20261024.md)。以下保留当时的审计／验收记录。

日期：2026-10-19。依据 [原插件审计](AUDIO_PLUGIN_RECONSTRUCTION_AUDIT_20261019.md)，以及用户明确选择“同时推进全部未重建插件”。

## 1. 范围与构建原则

- 现有 AAC、OGG、FLAC、APE 修正保存、流接口和旧系统边界。
- 新建八个独立工程：MPC、AC3/DTS、MOD、CLI Encoder、ASF、TAK、RM、VQF。
- 依据原 DLL 的工厂、IID、虚表、调用约定、读写行为和伪代码恢复宿主适配层；解码算法采用明确版本的源码库，不加载原 wrapper 冒充重建。
- 继续使用现代 MSVC x86、VC-LTL、YY-Thunks；测试代码、样本及逆向证据只放在 `rebuild/tests/audio_plugins_fix_20261019`，不加入各插件 Action 或发行包。
- 所有依赖固定版本与下载 SHA-256，上游源码构建时下载，仓库保留完整许可证与修改记录。运行 ZIP 仅包含对应 AddIn DLL 和 SHA256SUMS；AAC、AC3/DTS 及静态 FFmpeg 插件另附对应源码包。
- 未操作 GitHub 发布；没有修改工作区原 `AddIn` 或用户歌曲。各项目的未提交源码供本地审查。

## 2. 已有插件的实质修正

| 插件 | 修正与版本 | 验证记录 |
| --- | --- | --- |
| AAC | FAAD2 2.11.3 → 2.11.4；恢复最后释放时保存；可选 Commit 结果接口；旧宿主不实现 Commit 的兼容；避免失败留下双 moov；短读、参考时长、SBR/PS 实际输出声道 | [AAC](../../ttp_aac/docs/VALIDATION_20261019.md) |
| OGG | 可选 Commit 及旧文件流兼容；Vorbisfile tell 回调扩为私有 64 位 ABI，修复 2/4 GiB 打开截断；保留现有 Vorbis/Opus/封面 | [OGG](../../ttp_ogg/docs/VALIDATION_20261019.md) |
| FLAC/TTA | 可选底层 Commit；TTA flags=0 仅读 22 字节，首次播放才准备索引；ID3v2.2/2.3/2.4 unsync、扩展头/CRC/footer 与未知帧保留，同一事务迁移 | [FLAC](../../ttp_flac/docs/GAP_FIXES_20261019.md) |
| APE | Monkey's Audio SDK 13.30；保留 StartEx 错误修正；未实现 Commit 不误报失败，真实错误仍传播，重复 Finish/析构不重试 | [APE](../../ttp_ape/docs/VALIDATION_20261019.md) |

对 libogg 1.3.6、Vorbis 1.3.7、Opus 1.6.1、opusfile 0.12、FLAC 1.5.0 重新核对后，保持其当前稳定版本。没有为改变版本号改用未发布主分支。

## 3. 新项目及已确认的兼容差异

| 工程 | 内核/机制 | 本轮实现重点 |
| --- | --- | --- |
| `ttp_mpc` | Musepack r475 | SV7/SV8、float32、定位、标签、ReplayGain、原宿主释放保存、可选 Commit |
| `ttp_ac3dts` | liba52 0.8.0 / libdca 0.0.7 | 原四工厂、压缩 Reader＋float64 Decoder、原声道顺序、短读、小缓冲、DTS 原版归一化 |
| `ttp_mod` | 默认 libmodplug 0.8.9.0；可选 libopenmpt 0.8.9 | MOD/XM/IT/S3M/FAR/MTM/STM/UMX、恢复毫秒时长及音序；全局混音状态隔离与并发锁；Unicode/虚拟 IStream |
| `ttp_clienc` | 外部命令编码器适配 | XML 预设、管道/临时 WAV、原 PCM 转换、Unicode、独立子进程标准句柄、退出码、失败保留目标文件 |
| `ttp_tak` | FFmpeg 9.0.2 最小 TAK 核心 | 原 Reader ABI、整数 8/16/24 位保持精度、定位、短读、无专有外部解码 DLL |
| `ttp_asf` | Windows WM/DMO＋原容器解析兼容路径 | 原压缩 Reader＋独立 Decoder 两工厂、24 位与多声道、MP3-in-ASF；解码能力受系统 codec 限制 |
| `ttp_rm` | FFmpeg 9.0.2 最小 RM 核心 | Cook/ATRAC3/SIPR/RA/MP3 ADU、原标签块与可靠保存、原始 RA 排除尾部标签 |
| `ttp_vqf` | FFmpeg 9.0.2 TwinVQ | 原 Reader/Metadata、首帧预热与回调时机、定位、修复 Win7 首帧未初始化缓存 |

**不能将“已构建”解释为所有原行为完全相同：**

- DTS 新库默认 QMF 除数改为 `sqrt(2)*32768`，旧版为 `49152`，导致主声道约 6% 幅度差。已找到原函数和常量并恢复；LFE 独立常量不动。
- MOD 现代渲染内核曾使部分 S3M/MTM/STM 的节奏、跳转和结束长度变化，因此没有作为默认内核发布。默认改用更接近原版的 libmodplug，八格式样本的时长和 EOF 字节数已对齐；插值后的 PCM 仍不保证与原 DLL 位精确。现代内核须显式设置 `TTP_MOD_MODERN_ENGINE=ON`。
- RM 原外部 codec 依赖 MSVCR71，现有本机和 XP/Win7 未找到该运行库；重建版无需它。无法执行的旧 codec 动态分支必须标记缺失，不能当作新 DLL 的原版对照已通过。
- AAC 复杂 Profile、非标准超长文件、格式私有标签、完整转换 GUI 等仍按各项目文档列明边界。

## 4. 既有四插件的实际验证

Windows 11 作为用户指定的 Windows 10 代理环境；没有独立 Windows 10 实测。

| 验证 | Windows 11 | XP SP3 x86 | Win7 SP1 |
| --- | --- | --- | --- |
| AAC/OGG/APE 联合接口、解码、保存、封面矩阵 | 103 项通过 | 103 项通过 | 103 项通过 |
| 上述结果文件、PCM、封面哈希 | 82 项基线 | 82 项全部匹配 | 82 项全部匹配 |
| FLAC 新提交/标签边界结果 | 17 个文件基线，另有 208+79 本地记录 | run.cmd 无失败，17 个哈希匹配 | run.cmd 无失败，17 个哈希匹配 |
| 原版 5.7.9 实际加载 AAC/OGG/FLAC/APE | 四项通过 | 四项通过 | 四项通过 |
| 重建播放器实际加载 AAC/OGG/FLAC/APE | 四项通过 | 四项通过 | 四项通过 |

宿主测试使用私有安装副本，核对实际 DLL 路径、播放时钟、错误对话框和正常退出。为排除短样本刚好结束导致的 0 ms，OGG/FLAC/APE 另用 12 秒样本重测；XP 原版四项保存进度约 3670～3680 ms，Win7 约 3760～3840 ms。文件属性保存 UI 的全部鼠标路径不在此加载测试结论中。

## 5. 八个新插件的验证

| 范围 | 实际结果 |
| --- | --- |
| MPC / AC3DTS / MOD | 本地 76 项回归，另有 40 个截断 MOD 输入检查；最终 R4 在 XP、Win7 均 `FAILURES=0` |
| MPC 保存 | XP 相对路径替换问题修复；故障注入模拟 ReplaceFile 已移动原文件再失败，恢复后原 SHA 不变，重复 Commit 不重写 |
| MOD 并发 | 两条播放和后台反复创建 Reader 的三线程测试，与单 Reader PCM 相同；八格式原/新时长、EOF 长度相同，XP/Win7 默认新核 PCM 哈希相同 |
| CLI Encoder | 原/新十种 PCM 转换结果相同；普通管道与 Job 拒绝、子进程继承输入管道的异常路径均在 XP/Win7 通过；取消不再无限等待 |
| TAK | 原/新 12 种 8/16/24 位、单/双声道及压缩等级样本 PCM 相同；XP/Win7 短读、定位、容量测试通过 |
| RM / VQF | 最终 50 项在 Win11、XP、Win7 全通过；41 个输出哈希匹配。VQF 用堆污染稳定复现并修复首帧未初始化；RM 原始 RA 保存标签前后 PCM 相同 |
| RM 写入原版回读 | XP/Win7 原 DLL 能读取新版 PNG 封面、空标题删除状态和原始 RA 的新标题。原 DLL 的探针目录临时配备其 VC2012 运行库；重建 DLL 无此依赖 |
| ASF | Win11 常规 WMA 48 次交叉解码 PCM 一致；高精度 48 次中 32 次成功且 PCM 一致，16 次受格式容量限制的请求旧、新均拒绝；六种原生 ASF 包布局 24 次读包和 72 次定位对照；两台 VM 的 WMA、高精度、标签、封面及失败恢复通过 |
| 实际宿主 | MPC / AC3DTS / MOD / TAK / RM / VQF / ASF 均在三系统的原版和重建版播放器中加载、推进播放时间并正常退出；ASF 分别验证 WMA 和 MP3 两种编码 |

接口测试没有替代全部转换配置窗口的鼠标操作。CLI 的验证覆盖真实外部进程、标准句柄、WAV/PCM 输出和异常退出，未声称完成原播放器转换 GUI 的所有流程。

## 6. 同步修复播放器宿主

修改 `src/plugins/plugin_manager.cpp`，新增 `src/plugins/builtin_mpeg_decoder.cpp`：

1. **向 Decoder 声明 22 字节扩展容量。** 原 EXE `004E3A3D` 的字节 `6a 28 8d 77 54 58 e8 c0 ce fc ff` 将 40 传给 `004B0908`；该函数把 `容量−18` 写入 WAVEFORMATEX `+0x10`。重建版原先只分配了 40 字节却保留 `cbSize=0`，可能使 24 位 WMA 降为 16 位。现传入 22，并拒绝超出分配容量的返回格式。
2. **允许压缩输入没有平均码率。** VBR MP3-in-ASF 在原 Reader 中也可返回 `nAvgBytesPerSec=0`；现在压缩格式允许零，PCM 和解码后的输出仍校验有效字节率。
3. **恢复宿主内置 MPEG Decoder。** 原 EXE `004CA86C → 004C9AC1 → 004C9748` 在通用 DMO 前注册 MPEG 解码器，经 `ttpcomm` ordinal 10–14 输出 float64。XP 即使没有 MP3 DMO，也能通过此分支播放 ASF 内 MP3。现恢复该顺序、分片缓存、定位重置和仅限内置 MPEG 的 EOF 排空，避免末帧丢失。详见 [原版证据与实现](BUILTIN_MPEG_DECODER_20261019.md)。
4. **真实解码验证。** 四种 24 位 WMA Pro/Lossless 双声道/5.1 经 PluginManager 输出完整 288000/864000 字节；12 秒 MP3-in-ASF 经内置 MPEG 输出 8,497,152 字节，等于 461 帧完整 PCM。容量协商、异常返回格式、整块/7 字节/4096 字节输入与 EOF 后重置检查通过。
5. **最终三系统集成。** Windows 11、XP SP3、Win7 SP1 的 19 项实际 PluginManager 全流解码均 `FAILURES=0`。原版/重建播放器的 ASF/MP3 窗口测试播放进度分别为 Win11 3831/3810 ms、XP 3330/2840 ms、Win7 3820/3790 ms；均无错误对话框或强制结束。

完整 `audio_recovery_tests` 已通过。其旧归档用例误加载样本根目录的原版 `ttpcomm.dll`，该库不提供重建归档 API，曾返回 `2147943706`；测试现从测试 EXE 同目录加载构建产物。另补全 ReplayGain 只读样本所需的文件版本信息。没有为通过测试修改生产归档实现。测试及样本仍仅保留在 `rebuild/tests`。

最终播放器 EXE SHA-256：`442d66f7219f780171ab5b9a329f003e94d927ed17d6e58295b0c34ab18574e2`。

## 7. 保留的差异与覆盖边界

- 现代解码库不承诺所有历史样本与旧 DLL PCM 位精确；AC3、MOD、RM、VQF 的具体差异见各项目文档。
- RM 的旧 RealCodec 全量动态对照缺 MSVCR71；已完成新 DLL 的三系统播放、原标签 ABI 回读和可用分支对照，没有把缺少的对照写成通过。
- RM/VQF 当前定位采用解码并丢弃到目标位置，长文件远距离跳转还可优化；没有为了速度改变播放结果。
- OGG 的 64 位回调已验证超过 2/4 GiB 的打开与定位值；巨大稀疏空洞的完整搜索未通过时间限制，不宣称所有超大文件的 seek 已验证。
- ASF 的 WMA 等系统编码依赖 WM/DMO；MP3 已由宿主内置 MPEG 承接。XP 独立调用 ASF 通用 Decoder 仍因系统没有 MP3 DMO 而失败，原独立 Decoder 亦失败，这与两种播放器实际可播放不矛盾。DRM、全部第三方 DMO、多语言/多值属性及所有网络时序不在本轮覆盖内。
- 宿主原通用 ACM Decoder 尚未完整恢复；本轮恢复的是有原版证据的内置 MPEG 路径。更多损坏 MPEG、ADU、播放中声道变化语料仍需单独覆盖。
- 新工程各自具备独立 Action。2026-10-19 验证时尚未接入播放器的上游下载列表；2026-10-20 已按各工程 Git origin 补齐八项。远程仓库已存在，但仍需发布首个正式 Release 后才能完成在线合集构建，详见 [插件包接入与验证](ADDIN_RELEASE_BUNDLES.md#2026-10-20-新插件接入与验证)。

## 8. 文档与发行文件

- [MPC](../../ttp_mpc/docs/RECONSTRUCTION.md)、[AC3/DTS](../../ttp_ac3dts/docs/RECONSTRUCTION.md)、[MOD](../../ttp_mod/docs/RECONSTRUCTION.md)
- [CLI](../../ttp_clienc/docs/VALIDATION_20261019.md)、[TAK](../../ttp_tak/docs/VALIDATION_20261019.md)、[ASF](../../ttp_asf/docs/VALIDATION_20261019.md)
- [RM](../../ttp_rm/docs/IMPLEMENTATION_AND_VALIDATION.md)、[VQF](../../ttp_vqf/docs/IMPLEMENTATION_AND_VALIDATION.md)

最终发行文件汇总到 `rebuild/build/Release`；独立运行 ZIP 保持 `ttp_<插件>-2026.10.19.zip`，对应源码为单独 `-source.zip`。`AUDIO_PLUGINS_SHA256SUMS.txt` 记录本轮插件与包的校验，不覆盖播放器自己的校验清单。

## 9. 最终 DLL 清单

文件版本统一为 `2026.10.19`。运行包内 DLL、独立构建输出和 Release/AddIn 三处已核对一致。共 12 个 DLL、12 个运行 ZIP 和 5 个对应源码 ZIP。

| DLL | 字节 | SHA-256 |
| --- | ---: | --- |
| ttp_aac.dll | 373248 | `041999507e260d2c8e828b0223adbbb973e4ea287080bf9ba832de9d280e09d9` |
| ttp_ogg.dll | 340480 | `7b0242371f265bdac78d094495c3190c13c939033b649d9bc2f37220686dca69` |
| ttp_flac.dll | 158208 | `4f2cde8cac807e0e928c7b0d2283b1f49d7adbf2b2257bb8bb22c4151df6aede` |
| ttp_ape.dll | 239616 | `674e8bc08e256156a307c8c01bb516279116b6112a66b67a852e163dd064ef7d` |
| ttp_mpc.dll | 142336 | `dce63812200a3cff42ed9533d0d6bc9373436899045eb2b9f4be009bbc733b20` |
| ttp_ac3dts.dll | 235008 | `9a856bc4edb2cf2f13ff0ebf7b42dde2ef95543545527ee87d3ee7285a9dbe3f` |
| ttp_mod.dll | 135168 | `87a5d2816981928aebfc743713c5260adfe03d9f4379d0b7f0c67396c6fdfb0e` |
| ttp_clienc.dll | 146944 | `80911b4e11c334ecc5762192c0f1f62300aa023e87dd67735116b06a34e1a8ba` |
| ttp_asf.dll | 92672 | `9a962df7d7d6c1f8197f881178c3ec7ca9240c9047044265bd94242b5b267218` |
| ttp_tak.dll | 435712 | `afee0141fd27b7fd335e594972b801385ec31c9edf0257c4acf3ec1dc1120821` |
| ttp_rm.dll | 935936 | `f5690cbc7e9124e89dbbbbcea94ad887ce39f02d957b7fc8bd2e40c84255a2dc` |
| ttp_vqf.dll | 787968 | `890f3c3709297ed87999c8626f734971aa714cc46f0d11478ccdea330228b1a3` |
