# AddIn 音频插件重建审计与后续方案

最新进展见 [2026.10.29 后续修复与验收](AUDIO_PLUGIN_FOLLOWUP_20261029.md)：真实 UMX、Cook／DNET 补噪、SIPR 16k 与缓存回放错误恢复，已完成 XP／Win7／Win11 验收。前一轮结果见 [2026.10.25](AUDIO_PLUGIN_FOLLOWUP_20261025.md)。以下保留当时版本的记录与摘要。

后续补全与新增边界见 [2026-10-24 报告](AUDIO_PLUGIN_COMPLETION_20261024.md)。以下保留当时的审计／验收记录。

日期：2026-10-19。范围：本工作区 AddIn 的音频 Reader、Decoder、Encoder 及其配套组件；同时审查已建的 `ttp_aac / ttp_ogg / ttp_flac / ttp_ape / ttp_enc`。`ttp_webm / ttp_ncm` 作为新增格式单列。本文为分析报告，不代表已执行下述修复或库升级。

后续实施与验证见 [音频插件修正、库更新与八个独立重建项目](AUDIO_PLUGIN_RECONSTRUCTION_IMPLEMENTATION_20261019.md)。下文保留修正前的审计基准，当前项目状态以实施报告为准。

`ttp_lrcsh` 是歌词服务，`ttpres` 是资源组件，均不计作音频编解码插件。主程序／ttpcomm 内的 MP3、WAV、CD、MIDI 和输出后端也不因 AddIn 中没有同名文件而被判为未实现；DSP 音效插件属于另一个接口范围。

## 1. 结论与判定标准

目前仍有 **8 个原音频插件没有独立重建工程**：`ttp_ac3dts`、`ttp_asf`、`ttp_clienc`、`ttp_mod`、`ttp_mpc`、`ttp_rm`、`ttp_tak`、`ttp_vqf`。

已有的五个重建项目可以覆盖大量实际播放和转换流程，但不能统一标为“原插件全部功能完全恢复”。应分开验收：

1. **宿主兼容**：导出、工厂顺序、GUID、虚表、引用计数、分配器和错误返回。
2. **功能覆盖**：全部原格式、定位、网络流、标签、图片、编码及配置。
3. **输出一致性**：无损解码应逐样本一致；更换有损编解码库后通常不能要求压缩码流或浮点 PCM 与旧核逐字节相同。
4. **异常与旧系统**：短读、取消、只读输入、保存失败、损坏文件，以及 XP／Win7 实际运行。

伪代码和二进制可以作为行为重建依据；不能仅凭反编译成功率、导出数量或几首样本通过，证明已获得全部原源码或全部行为等价。对于 Nero、TAK SDK、TwinVQ、mp3PRO 等外部组件，恢复插件适配层也不等于重建了其算法内核。

## 2. 基准身份：不能把当前 AddIn 全部当成原版

本轮重新计算了二进制 SHA-256、PE 导出／导入及源码仓库 HEAD。完整清单在本地 `rebuild/tests/audio_plugins_audit_20261019/inventory.json`。

### 2.1 一个已确认的混合版本问题

根目录 `AddIn/ttp_aac.dll` 的文件说明是 **TTPlayer AAC/MP4 reconstructed add-in**，版本 `0.1.0`，369,664 字节，SHA-256 前缀 `80e30c42f647de03`。它是早期重建产物，不是原版。

本轮 AAC 原版比较使用已存的隔离副本：

```text
rebuild/out/test-artifacts/aac_rebuild/original-only/ttp_aac.dll
314720 bytes
SHA-256 49195708ba9b942edd8215ffcb4e5db73a1548b0c02a5e2b11960ea12af69135
```

`TTPlayer5719/AddIn` 还包含另一批较旧插件，不能把其地址／功能直接混用到上述基准。例如该目录 AAC 是 270,768 字节，OGG 是 142,256 字节；当前作为原版比较基准的 OGG 是 192,352 字节。文中地址均绑定对应 DLL，而非仅绑定文件名。

### 2.2 当前文件和源码的关系

| 项目 | 原版或旧部署文件 | 当前工程 Release 产物 | 状态 |
| --- | --- | --- | --- |
| AAC | 原版 `49195708…`；根 AddIn 已是早期重建 | 370,176 bytes，`c9917672d0733d0d…` | 有独立工程，存在新发现的保存兼容问题 |
| OGG | `ee58d0603a5c4c75…` | 340,480 bytes，`38e77bbba7bde365…` | 有独立工程，另增 Opus |
| FLAC | `e2833c023377787f…` | 156,160 bytes，`00154706bb112924…` | 有独立工程，同时包含 TTA |
| APE | `e89483dd7ec03b2b…` | 239,616 bytes，`508de60e0b9d8640…` | 有独立工程，含编码器 |
| ENC | `c0f116ea6c27f4a5…` | 309,248 bytes，`293402ebf28662fc…` | 有独立工程，MP3／WMA／mp3PRO |
| WebM | 本批原 AddIn 无对应插件 | 378,880 bytes，`9d65b32d7153b775…` | 新增格式工程，不属于原版缺失工厂 |
| NCM | 本批原 AddIn 无对应插件 | 141,824 bytes，`08d814fae9c7c1e8…` | 新增格式工程，不属于原版缺失工厂 |

本机 `rebuild/build/Release/AddIn` 的 APE、OGG、ENC 仍与根 AddIn 的原文件相同。这是**当前部署身份事实**，不能据此推断各工程没实现，也不能把用这些文件运行的结果算成新版验收。

Action 已在 `cmake/package_addin_bundles.py` 中为 WebM、APE、FLAC、ENC、OGG、AAC 下载各独立仓库的稳定发行版，并另生成 `CodecAddIn-Rebuild-版本.zip`；`manual-build.yml` 的发行说明要求用户合并该包的 AddIn。普通本地构建、主程序包和独立插件包需要分别核对，不能仅依据目录名判断正在加载的版本。NCM 不在当前六项 codec bundle 列表内。

## 3. 尚未重建的八个插件

| 插件 | 实际职责 | 重建建议 | 核心边界 |
| --- | --- | --- | --- |
| `ttp_mpc` | Musepack SV7／SV8、定位、标签、ReplayGain | 最适合先做独立重建 | 原版已有 SV8，不能当新增成果 |
| `ttp_ac3dts` | AC3／DTS 的 Reader 与 Decoder | 先恢复 liba52／libdts 路径，再扩展 | 新增 E-AC3／DTS 扩展需单独后端与测试 |
| `ttp_mod` | Modules Reader、合成渲染 | 旧渲染基准＋可选现代 libopenmpt | 换库会改变插值、循环和听感 |
| `ttp_clienc` | 调用外部编码 EXE | 可以完整恢复适配层 | 不包含每个 EXE 的压缩算法 |
| `ttp_asf` | ASF Reader＋DMO Decoder | 先保留系统 DMO 以兼容旧系统 | WMA 核心由系统提供 |
| `ttp_tak` | TAK Reader＋外部 SDK 调用 | 恢复接口后选官方库或开源核心 | 换官方 DLL 不等于源码独立重建 |
| `ttp_rm` | Real 容器＋外部 RealCodec | 容器／旧核基准，再评估 FFmpeg | 需按实际音频编码逐项覆盖 |
| `ttp_vqf` | TwinVQ Reader＋tvqdec | 优先级较后，重点状态和幅度 | 旧核心全局状态／浮点数值需验证 |

### 3.1 Musepack：原版已经包含 SV7 和 SV8

原文件 `c8e832ebed2424a8…`。本轮下探到 `60506D68`，同时处理 `MP+` 与 `MPCK`，SV8 分支还识别 `AP / SH / RG / EI / SO` 块。因此不能依据“文件年代久”或旧 `libmpcdec 1.2.6` 的能力推断本插件只有 SV7。

Reader 输出 float32，标签委托宿主 `CreateStdContent`，存在最后释放时写盘的路径；ReplayGain 还有直接修改 SV7 头的分支。重建必须同时考虑头内增益和尾部标签，不能只有通用 APEv2 标签读写。

`60502731` 会先复制上一次剩余的整块 PCM，再处理本次容量；这是小缓冲的静态风险，尚未动态复现越界。`60503466` 的头部增益写入只处理 `MP+`／SV7，因此 SV8 能播放不等于已经证明其 ReplayGain 修改可正确保存。定位则已有稀疏 seek table，不应误写成与 AC3 一样仅按文件长度比例跳转。

建议按当前 Musepack SDK 的 demux/decoder 对象模式重建：保留 SV7／SV8、header gain、sample count、seek 后预热、浮点输出与宿主 ABI。改进重点是有界读写、小缓冲分帧、64 位位置、取消和明确的 EOF／损坏错误。升级库并非主要收益：官方仍提供历史 r475，首先应把已存在的两代格式和旧宿主交互恢复齐全。

### 3.2 AC3/DTS：四个工厂都需要恢复

原文件 `5196ff7bb9cd3d29…`。已确认工厂顺序为 **AC3 Decoder、DTS Decoder、AC3 Reader、DTS Reader**，二进制及调用链对应 liba52／libdts 家族。仅实现 `.ac3/.dts` 文件读取，会遗漏由其他容器交接过来的 Decoder 接口。

原路径输出 64 位浮点；定位按数据长度与时长的比例换算字节偏移，再交给帧处理。重建时应先对齐原输出幅度、声道重排、下混、DRC、帧同步与返回值，再增加按帧索引的准确定位。数据不完整时保留同步缓冲，避免把短读视为结束。

推荐先评估现行 liba52／libdca；新增 E-AC3、DTS-HD 或其他 DTS 模式不能靠扩展后缀或升级名字自动实现，应单列能力与解码后端。可采用精简 FFmpeg decoder，但需检查多声道布局、采样数、预热和输出精度，不能直接把 float32 当原 float64 输出交给宿主。

### 3.3 MOD：恢复接口容易，恢复听感和控制语义更费工作

原文件 `e295817c9532d1a1…`。原插件只注册一个 Modules Reader，嵌入 ModPlug 家族渲染器，输出固定 **44.1 kHz／16 bit／双声道**。旧打开路径有 Unicode→ACP→CreateFileA，并映射整文件的行为；与能直接消费包内 IStream 的 Reader 不同。

建议以现有模块样本建立原输出基准，再决定使用旧兼容渲染参数还是现代 libopenmpt。关键不是只让 MOD／XM／IT／S3M 发出声音，而是：tempo/speed、效果命令、循环与终止、插值、升采样、音量、重采样滤波、seek 后状态和子曲目是否一致。

现代化收益包括 Unicode、包内流、逐步读取／有界输入、准确时长、取消、更多模块格式。libopenmpt 已有专门的旧系统构建路线，应按该路线验证，不能直接部署其普通现代版。库升级造成的渲染变化应明示为改进模式，不伪称原渲染逐样本重现。

### 3.4 CLI 编码器：应重建进程和管道管理

原文件 `685db2973b9dd56c…`。关键函数 `6020142E`：模板含 `%s` 时通过宿主流创建 `.ttp_tmp.wav`；否则建立管道；`%d` 替换目标路径，隐藏启动外部编码器。旧代码同时存在 CreateProcessW/A，不能简单断言它完全不支持 Unicode。

本轮确认它会临时用 `SetStdHandle(STD_INPUT_HANDLE)` 修改播放器进程的全局 stdin。这是并发转换和失败清理需要避免的状态。当前播放器已补宿主导出、XML 也已有 FFmpeg 预设，但旧 DLL 本身没有重建。

方案：恢复 XML 配置、变量替换、临时 WAV／管道两种方式；用子进程自身的标准句柄启动，补 stderr 排空、退出码、超时／取消和子进程回收。明确 WAV 超过 4 GiB、浮点／高位深、临时空间耗尽和 Unicode 路径；编码完成并成功写标签后再提交输出。更新外部 FFmpeg EXE 与重建 CLI DLL 是两项独立工作。

### 3.5 ASF/WMA：保留系统后端可提高原版一致性

原文件 `f36dcc9104e7bd11…`。对象包括 `CASFReader / CDMODecoder` 及各自 Creator。`60155CAF` 加载 `wmadmod.dll`、检查版本、查询 `DllGetClassObject` 创建解码对象；还引用 `msdmo.dll` 与 `MoCreateMediaType`。

因此可完整重建的主体是 ASF header/packet、索引／时间、Reader 与 Decoder 接口、标签／图片、保存与系统解码器适配。WMA 数学解码核心本来就在系统组件中；重写 wrapper 不等于重新实现该核心。

建议首阶段保留 DMO：XP 支持 DMO，新系统可另外接 MFT，依据系统实际支持的 WMA Standard／Pro／Lossless 格式工作。[微软解码器文档](https://learn.microsoft.com/en-us/windows/win32/medfound/windowsmediaaudiodecoder) 明确区分了这些系统与接口。完全脱离系统 codec 可评估 FFmpeg，但声道下混、DRC、延迟和舍入要重新对照；受保护文件不是普通容器兼容测试的替代品。

该 wrapper 静态依赖 MSVCR110；采用现有现代兼容构建可消除 wrapper 的 VC++ 2012 运行库要求，但不会消除 WMA 系统组件需求。历史封面测试包括 JPEG／PNG，重建不能只恢复播放而遗漏内容保存。

### 3.6 TAK：容量处理和错误分类优先于更换库

原 wrapper `eed31287e60cb641…`，配套 `tak_deco_lib.dll` 为 **2.2.0.0**。动态调用六项主要接口：`tak_SSD_Create_FromStream / Valid / GetStreamInfo / ReadAudio / Seek / Destroy`，标签还使用宿主标准内容服务。

这里的 2.2.0.0 指 **AddIn 内**的库；播放器根目录另有 **2.3.3.0**，SHA-256 `345bd0d94aee5967b9b6c21cb1ee2a3c3946778fd84e0495b5fccbb084b78936`。本轮未启动宿主确认实际加载哪一份，因此不能笼统报告“当前 TAK 运行的就是 2.2”。先记录运行时模块绝对路径和哈希，再统一分发。

`100015D7` 获取了输出 capacity，但请求样本数取对象字段，当前函数内没有依 capacity 限帧；库返回状态非 0／3 时转成 `S_FALSE`。这两点需要小缓冲、短读、损坏文件测试：前者是静态边界风险，本轮未声称复现溢出；后者不能在新实现中把所有错误都吞成 EOF。

建议补样本对齐、64 位内部计数、seek 状态复位和取消；APEv2／ReplayGain／多值／图片复用已核对的公共标签层。后端有两条路线：

- 保留官方 SDK，先验证升级候选 **2.3.3**。本轮可从 [TAK 组件作者的更新记录](https://foobar.hyv.fi/?view=foo_input_tak) 确认该官方库版本曾被采用；TAK 作者网站访问超时，不能把 2.3.3 写成已核验的“绝对最新版”。
- 采用 [FFmpeg TAK decoder](https://github.com/FFmpeg/FFmpeg/blob/n9.0.2/libavcodec/takdec.c) 实现完全源码构建。TAK 是无损格式，应严格对比整数 PCM、样本数和 seek 区间；只有测试覆盖的版本／声道／位深可以声明兼容。

### 3.7 RealAudio：容器和多种外部 codec 不能混为一个功能

原 wrapper `3b2a0f2a44f795e7…`。`60601652` 先尝试加载 codec，再查 RealPlayer／Helix 注册表、COM 路径、EXE 旁 `RealCodec` 和公共目录；解析 `RAOpenCodec[2] / RAInitDecoder / RADecode / RAFlush / RASetFlavor` 等接口。实际解码不在这个 62 KB wrapper 内。

当前 `RealCodec` 包含 atrc、cook、sipr，三个旧后端均依赖 MSVCR71；wrapper 自身依赖 MSVCR110。仅重建 wrapper 不能自动消除后端的 VC++ 2003 运行库依赖。

建议先恢复 `.RMF/.ra`、流属性、packet/interleave、时间戳／索引和定位后 flush，再按真实编码分支接入旧后端或 FFmpeg 的 Cook／ATRAC3／SIPR 等实现。缺少 codec 应明确报告 FourCC，不能变成空文件／EOF。历史 `RM/AC3` 的解码失败与封面成功是不同事实，需要独立列为兼容测试。未核实到可直接取代全部这些旧库的现代同 ABI SDK。

### 3.8 VQF/TwinVQ：幅度和帧状态也属于兼容契约

原 wrapper `0c5f655766521a67…`，`tvqdec.dll` 为 573,440 字节，导出初始化、解码、帧长、声道／采样率和销毁接口。

`801D1B4B` 将解码后的分离声道转成交错数据，并把值夹到 **[-32700,32700]** 后乘 **1/32768** 输出 float。只说新解码器“也输出浮点”不足以保证声音一致。该局部同样读取 capacity 但未用来限帧，需补缓冲不足用例。

可恢复 VQF chunk、文本字段、帧索引、seek 后预热和状态，并保留 tvqdec 作为参考。长期可采用 [FFmpeg TwinVQ 实现](https://ffmpeg.org/doxygen/trunk/twinvqdec_8c_source.html)，但应对照预热、尾部、mono/stereo、码率模式和原削波行为。未核实到官方现代同 ABI tvqdec 发行渠道。精确旧浮点输出与独立维护的成本较高，建议放在 MPC／AC3DTS／TAK 等之后。

## 4. 外部音频组件：适配与算法重建的边界

### 4.1 Nero 编码组件

当前文件为：

| 文件 | 版本 | 职责 |
| --- | --- | --- |
| `Aac.dll` | 2.5.9.991 | Nero 对象工厂、配置窗口、编码管理、MP4 封装、标签和收尾 |
| `aacenc32.dll` | 3.2.0.30 | AAC 编码核心 |
| `NeroIPP.dll` | 3.2.0.30 | 配套计算组件 |

已存伪代码确认 `Aac.dll!10011A30` 返回主对象，`100119A0` 枚举 Reader／Encoder 工厂；`10003D3C / 10005BA0 / 10004A48` 分别启动、写入和收尾。真正的压缩帧由 `aacStreamOpen32 / aacStreamEncodeFrame32 / aacStreamClose32 / aacStreamDestroy32` 等外部调用生成。详见 [已有 Nero 分析](../../ttp_aac/docs/NERO_AAC_DLL_PSEUDOCODE_ANALYSIS.md)。

当前 `ttp_aac/src/nero.cpp` 仍加载这三个 DLL，恢复的是调用约定、工厂、输入转换和标签传递，并没有重建 Nero 编码算法。普通 AAC 播放使用 FAAD2，不需要这三个编码组件。

建议保留经验证的 Nero 入口，新增独立命名的现代 AAC 编码器。可以评估 [FAAC 2.2](https://github.com/knik0/faac/releases/tag/faac-2.2) 或 FDK-AAC，但须按其实际 Profile、声道、延迟／尾部、码率和容器接口重新接入。FAAC 2.2 的 ABI 与 x86 调用约定已有变化；这些库不是 `aacenc32.dll` 的同名替换件，也不能标作“Nero 原算法完全复原”。新版编码器的配置界面应反映实际能力，而非把 Nero 预设数字直接映射过去。

TAK、TwinVQ 和 RealAudio 的对应边界见第 3 节；ENC 的 mp3PRO 同样仍依赖用户已有的 `fhtpro.flt`，其 SBR 编码核心未重建。

## 5. 已重建 AAC：确定的差异和新发现

### 5.1 已恢复与有意变化

原 `60006A97` 工厂枚举为 MP4 Reader、AAC Reader、AAC Decoder、Nero Encoder 四项，当前 `src/plugin.cpp` 保留该次序。本轮 REA／Ghidra 重新核对了此函数，证据 ID 为 `ev_8f3c870c43026230d9ca1da4b530a4b825be9bab7c024ecf167f375e1f6bf2d1`。

已恢复本地 ADTS／MP4 的主要播放、定位及标签接口，并新增分片 MP4 和有界的未知长度 ADTS 顺序读取。`HTTP_ADTS_STREAMING_20261011.md` 和当前 `reader.cpp::open_live` 已证明后者存在；早期 README／RECONSTRUCTION 中“不可定位网络流未恢复”的概括已过时，应改为具体描述仍缺的分支。

FAAD2 从原 2.7 家族升级到 2.11.3 后，有损 PCM 数值发生过已记录的变化。这属于库升级差异，不应为追求旧哈希而撤销新版修正。

### 5.2 P0：保存失败后可能留下当前 Reader 也打不开的 MP4

代码依据：`ttp_aac/src/mp4.cpp:622` 的 `Mp4File::save` 先追加新 `moov`，调用 `IStream::Commit`，再把旧 `moov` 改成 `free`。它直接把 `E_NOTIMPL` 当作失败；失败分支对回滚 `SetSize` 的返回值没有检查。

对比已修复的 `ttp_enc/src/common.h::commit_stream`：原版播放器的直接文件流不实现 `Commit`，ENC 已明确接受 `E_NOTIMPL / STG_E_INVALIDFUNCTION`，同时仍传播真实 I/O 错误。AAC 没有同步这一兼容规则。

本轮 Windows 11 x86 独立 ABI 实测，使用测试目录中的 MP4 副本和包装的 `IStream`：

| Stream 的 Commit 行为 | Set Title | 释放后重开 | MP4 顶层结构 |
| --- | --- | --- | --- |
| 正常提交 | `S_OK` | `S_OK` | 旧 moov 已改 free，仅一个有效 moov |
| 返回 `E_NOTIMPL` | `80004001` | `8007000D` | 偏移 28 与 17,313 各保留一个 moov |

原样本 17,313 字节，失败副本 18,495 字节；当前 Reader 在遇到第二个 moov 时拒绝打开。证据：本地 `aac-reopen-results.json`，DLL SHA-256 `c9917672d0733d0dedfc7b8f67c857dfe0389526b4c0986dc64d7f5ed3ca3e85`。

这是**受控流边界复现**，不是本轮实际点击原版属性窗口的录像。原 DLL 在同一简化宿主中 Set 返回成功，但没有持久化标题，故没有把该结果写成“原 DLL 保存成功”。真正原版宿主的属性保存还需要用其原生流补做验证。

建议：统一直接文件流的 Commit 兼容规则；显式检查截断／回滚；对不能可靠回滚的流在写之前拒绝写入或使用可恢复的事务方案。原版宿主保留最终 Release 提交语义，重建宿主增加可选显式提交接口返回真实结果。必须覆盖失败后重新打开和音频数据不变，不能只断言 Set 返回值。

### 5.3 其余待补／待验证

| 项目 | 当前事实 | 建议 |
| --- | --- | --- |
| 打开标志 | 原 `60005644` 使用 `~flags & 1` 选择打开方式；当前 `AudioReader::Open(IStream*, DWORD)` 忽略 flags，依据 Stat 的写权限设置可写 | 核对检查／播放／编辑三类打开，恢复 flags 与实际流权限的共同约束 |
| 保存时机 | 每次 Set／封面操作立即写新 moov；原版存在关闭对象时写回 | 暂存修改并一次提交，避免每个字段追加一份完整 moov；保留失败反馈 |
| IReferenceDuration | 接口存在，`reference_duration_` 只赋值，无实际使用 | 恢复原未知总长／参考时长估计，区分不可 seek 的直播与可续读文件 |
| 复杂 AAC | ADIF、Main／SSR／LD、960 帧长、MP3-in-MP4 有实现或识别，但缺完整样本对照 | 按 Profile 和状态转换补测试；未验证不能算缺实现，也不能算已完成 |
| 动态格式 | 当前 `AacCore::decode` 拒绝中途采样率／声道变化 | 先核对原支持范围，再定义宿主重配流程；不要静默以旧格式输出 |
| 多 sample description | 仅使用 stsd 第一项；classic／fragment 对引用非 1 的索引明确拒绝 | 如扩展，保存每包描述编号和 ASC 变化；不能只放宽检查 |
| 后缀枚举 | 原资源列出 `*.aac;*.aa`，当前 AAC 工厂只列 `*.aac`；MP4 工厂另增 3gp／3g2 | 对照真实样本确认 `.aa` 的历史用途，再决定恢复过滤器；枚举某后缀不等于能解码其所有内容 |
| 无缝裁剪 | 维持原 ctts 规则，未接入完整 elst／iTunSMPB 首尾裁剪 | 作为独立改进设计，与原版时长／seek 兼容测试分开 |
| 封面 | 一张、写入上限 60,000 bytes；分片 MP4 只读 | 旧接口保持行为，新接口可扩大；不把原有限制误列为重建遗漏 |
| Nero | 仍依赖原三个外部 DLL；配置／核心未自行重建 | 保留已有兼容入口，现代 AAC 编码独立实现 |

## 6. 已重建 OGG／FLAC／APE 与新增格式

### 6.1 共同的流提交兼容问题：本轮新增实测

私有 IStream 保持 Read／Write／Seek／SetSize 正常，仅将 Commit 返回值改为 `E_NOTIMPL`，以正常 Commit 为对照。当前三个工程产物得到：

| 插件／分支 | 观察结果 | 修正重点 |
| --- | --- | --- |
| OGG 标签保存 | Set 成功，释放也没有错误返回；落盘文件却回到修改前哈希，标题没保存 | 兼容直接写入流；增加可选显式 Commit；验收必须关闭后重读 |
| FLAC 保留宿主流的回写分支 | 可选 Commit 返回 `80004001`，文件被回滚 | 共享 Commit 语义；正常 ReplaceFileW 分支不能一概算失败 |
| APE Encoder | Finish 返回 `80004001`，但编码文件与正常对照哈希完全相同 | 避免把“可选刷新未实现”报告为编码失败；保留真正 I/O 错误 |

代码位置：`ttp_ogg/src/comments.cpp:211`、`ttp_flac/src/metadata.cpp:410`、`ttp_ape/src/encoder.cpp:231`。实测证据为本地 `parity/commit-notimpl-results.json`。这是明确的受控 ABI 兼容缺口，不是“所有原宿主界面路径每次都失败”的结论。

### 6.2 OGG／Vorbis／Opus

**已恢复：** 单 Reader 工厂、Reader／Metadata 虚表、float64 输出、原六声道 `0/2/1/5/3/4` 重排、毫秒 seek、流扩展／回调、UTF-8 comments、同名字段首项修改及最后 Release 写盘。原入口包括 `605542FD / 6055268D / 6055299A / 60552EFA / 60552FF4`。

**已经实现的扩展：** Opus、oga／opus 后缀、多张内嵌封面、备份回滚。Opus 的 48 kHz、pre-skip、尾部裁剪、头增益、R128 与 ReplayGain 适配，以及 family 0／1 的 1～8 声道重排均已存在，不能重复列作缺失。

剩余差异／建议：

- 当前 QI 没有可选保存结果接口；析构保存失败只写调试输出。应与 FLAC／WebM 一样提供扩展 Commit，旧宿主仍保留 Release 保存。
- `reader.cpp:58` 的 Vorbis tell callback 使用 Windows `long`，位置超过 `LONG_MAX` 返回 -1，超过 2 GiB 的定位是具体边界。需修改 64 位回调／偏移适配，而非删除检查后截断成 32 位。
- 变采样率／变声道的链式流返回 `E_NOTIMPL`。这与原单一 Format 契约相关；完整支持需要宿主重配或统一输出格式。
- Opus family 2／3／255 未覆盖；Vorbis 仅六声道重排，其他 3／5／7／8 声道保持原输出顺序。改通用 Windows 布局属于明确的行为改变。
- 整文件流式重写有写放大、传统 MAX_PATH 和极端回滚边界；不等于断电原子保存。

历史验证包括选定的 11 组 Vorbis 8～96 kHz／1～8 声道原 DLL PCM 对照，以及 Opus、标签／封面和 XP／Win7 双宿主验证。记录条数不是独立音乐文件数或全部参数组合；当前新发现的 Commit 用例此前没有覆盖。

### 6.3 FLAC／TTA

**已恢复：** 两个 Reader、原宿主标准内容 type 5／4、FLAC／TTA 的容量和定位、Unicode／多值标签、多封面、事务保存和可选 Commit。libFLAC 从 1.2.1 改为 1.5.0，32 位 FLAC 有效输出、TTA 毫秒时长不再截整秒，属于应保留的改进。

**TTA 历史 float32 分支已恢复。** `tta.cpp` 存在双倍内部通道的重组，既有 1／2／6 声道与原 DLL 逐字节测试；不能再写成“浮点 TTA 尚未实现”。format 1 的 32 位整数 TTA 缺独立样本，是另一个验证问题。

本轮确认 **TTA 信息打开模式没有恢复**：原 `60303E3E` 在 flags=0 时只调用 `60303C5A` 读 22 字节头，flags=1 才准备解码；新版 `reader.cpp:254` 丢弃 flags，提前读取完整定位表。实测如下：

| 输入／模式 | 原版 | 重建版 |
| --- | --- | --- |
| 正常 1.25 秒 TTA，flags=0 | S_OK，1 次 Read／22 bytes | S_OK，3 次 Read／38 bytes |
| 同一文件仅保留合法 22 字节头，flags=0 | S_OK，可报告头信息 | STG_E_READFAULT |
| 仅有头，flags=1 播放 | E_FAIL | STG_E_READFAULT |

建议恢复 header-only 状态，延迟位流和定位表初始化；保留真实播放时的完整验证。此处要求的是显示信息的行为和性能，不是让损坏文件继续播放。原 FLAC／OGG 本来也忽略该 flags，APE 已正确区分，不能把 TTA 的问题推广成所有 Reader 的统一缺陷。证据：`parity/tta-flags-results.json`。

其他剩余边界：

- `external_tags.cpp` 对全局 unsync、扩展头、压缩／加密等特殊外部 ID3，仍拒绝文字修改以避免旧后备标签复活；文件可读／播放不代表可完成所有标签修改。应复用已完善的 ttpcomm 标签基础，在**同一个事务**中写 native comments 并清理旧文字，保留未知／二进制帧。
- 签名识别需要 seek，尚非完整的不可定位 FLAC／TTA 流 Reader；Ogg FLAC、MP4 FLAC、TTA2／加密 TTA 属于新增范围。
- 当前接受 8／16／24／32 位，其他有效位深、所有历史 TTA 参数、大文件／超长路径和极端回滚仍需扩展或验证。

### 6.4 APE

原 Reader／Encoder 双工厂、两项辅助导出、五档压缩配置、APL、元数据／图片转发与释放次序已恢复。原地址包括 `601024D3 / 6010284A / 601028E4 / 601014C7 / 601015F4 / 60101C60`。

SDK 从 4.11 升到 13.27；整数编码保留 8／16／24／32 位精度，不能为重现旧版强制 16 位的行为而撤销。新压缩数据的大小和字节不同不等于无损 PCM 有差异。

剩余事项：

1. 修复第 6.1 节的 Finish 误报失败。
2. 旧流编码接口没有总帧数，新 SDK 会预留未知长度定位表，小文件可能明显变大。可以设计可选总长度提示／收尾整理，保持旧 Encoder ABI；以完整解码后的样本验证，不能只看 ZIP／APE 大小。
3. 当前接受的历史版本范围、定位表 16 MiB／帧 PCM 256 MiB 上限均应写明。所有旧版、32 声道、高压缩和超大文件尚无穷尽矩阵。
4. Reader 与 Encoder 固定单线程。播放保留低延迟单线程合理；批量转换／增益扫描可以用宿主线程预算，避免“多曲目×各自多线程”超额占用。
5. 标签／封面仍需宿主 `CreateStdContent`。独立最小宿主没有该服务时不提供同等标签能力；原宿主最后 Release 写盘没有结果返回，也是接口本身的限制。

### 6.5 WebM／NCM 是扩展，不是原插件未恢复项

- **WebM**：已有 Opus／Vorbis 音轨、1～8 声道、标签和 PNG／JPEG Attachments、回滚及可选 Commit。仍要求已知长度可定位输入，长文件初开需完整索引；直播、加密／压缩 track、通用 MKV/MKA 和切音轨 UI 不在当前范围。建议改进 SeekHead／Cues 与延迟索引。Attachments 图片是 Matroska 兼容扩展，不能保证每个 WebM 工具都显示。
- **NCM／UC**：解封装后限 MP3／native FLAC，标签封面只读，不额外联网补图或产生明文临时音频。UC! 完整性和恰好截在 MP3 帧边界的文件无法总能判断；这属于该扩展的输入边界。

## 7. 已重建 ENC：恢复范围与剩余边界

`ttp_enc` 已经有完整独立工程，不应再列入“尚未重建”。原 `60253278` 枚举 MP3、WMA、可用时的 mp3PRO；`60253781 / 602537A2` 为默认配置，`60254722` 匹配 WMA 系统格式。

当前恢复了原配置键、MP3 CBR／ABR／旧预设、WMA 参数匹配和标准标签映射；新增 MP3 V0–V9、WMA 两遍 VBR／16 位无损、mp3PRO 同目录免安装发现与 Unicode 路径。原版／重建宿主转换及 XP／Win7 的历史验证有明确记录，见 [VALIDATION](../../ttp_enc/docs/VALIDATION.md) 与 [PARITY_FIXES](../../ttp_enc/docs/PARITY_FIXES.md)。

仍需明确的差异：

- **LAME 4.0 与旧核不等价。** 原资源写 3.90.3，内核版本函数为 3.93；不能用界面字符串确定算法版本。现代编码结果、VBR 大小和尾部行为不要求旧哈希一致。
- **公共 PCM 路径仍先量化到 16 位。** `encoder.cpp::Write` 用 `sample16` 转换，再交给三个后端。WMA“无损”只保证对这些 16 位数据无损，并不保留 24／32 位输入全部精度。这是现有设计限制，高位深改进应单独实施。
- **WMA 由系统提供。** 不能把某台 Win11 能编码的格式自动当作 XP／Win7 可用；当前没有近似采样率静默替换，这是正确保留的限制。
- **mp3PRO 内核未重建。** `fhtpro.flt` 的全局状态需要单会话保护，配置与编码不能同时进入。独立入口恢复不意味着可以并行编码多首 mp3PRO。
- **配置界面有意扩大。** 新增模式占用新的控件空间，不能再称原窗口像素完全一致。
- **验证绑定构建。** 最新体积优化 DLL `293402eb…` 有本地编码回读、XP／Win7 加载和工厂检查；文档明确未为此哈希重跑全部旧虚拟机编码和窗口矩阵。历史全测结果不能无条件挪用。

优化方向：MP3 采用 LAME 的高精度输入接口，WMA 按系统枚举结果提供真正高位深模式；复用固定大小 PCM／编码缓冲，避免每次 Write 新建缓冲；测试短流、整帧尾部、磁盘满、析构收尾和重复 Finish。高精度或新模式不应悄悄改变既有 16 位兼容配置的行为。

## 8. 引用库现状与可升级项

核查日期为 2026-10-19。“最新版本可取得”不代表其默认二进制能在 XP 运行，也不代表旧适配补丁仍适用。

| 库／组件 | 工程现状 | 本轮核查结果 | 建议 |
| --- | --- | --- | --- |
| FAAD2 | 2.11.3 | [2.11.4，2026-10-01](https://github.com/knik0/faad2/releases/tag/2.11.4) | 可升级，独立验证解码变化与旧系统 |
| Monkey's Audio SDK | 13.27 | [13.30，2026-10-16](https://www.monkeysaudio.com/versionhistory.html) | 可升级；主要是内部／构建修正，不承诺音质或速度提升 |
| libogg | 1.3.6 | [1.3.6](https://xiph.org/downloads/) | 已是核查到的稳定版 |
| libvorbis／vorbisfile | 1.3.7 | [1.3.7](https://xiph.org/downloads/) | 已是稳定版；老版本号不等于存在新发行 |
| libFLAC | 1.5.0 | [1.5.0](https://xiph.org/downloads/) | 保持固定依赖，先补适配边界 |
| Opus | 1.6.1 | [1.6.1](https://opus-codec.org/downloads/) | 已是稳定版 |
| opusfile | 0.12 | [0.12](https://opus-codec.org/downloads/) | 网络继续用宿主 IStream，不必另加 opusurl／TLS 栈 |
| LAME | 4.0 | [4.0](https://lame.sourceforge.io/download.php) | 已是稳定版 |
| libwebm | 1.0.0.32 | [1.0.0.32 tag](https://chromium.googlesource.com/webm/libwebm/+refs) | 已是核查到的最新 tag |
| cJSON | 1.7.19 | [1.7.19](https://github.com/DaveGamble/cJSON/releases) | 已是稳定发行 |
| dr_mp3 | 0.7.3 固定提交 | [master 头文件](https://github.com/mackron/dr_libs/blob/master/dr_mp3.h) 标 0.7.4／TBD | 只能称开发快照，不能当稳定新版本直接更新 |
| Musepack | 未重建，原插件已有 SV7／SV8 | [官方历史 SDK r475](https://www.musepack.net/index.php?pg=src) | 采用适合双版本的 SDK，重点是恢复适配而非追新号 |
| libdca | 原 libdts 家族 | [0.0.7](https://www.videolan.org/developers/libdca.html) | 可作重建基线，不等于支持所有 DTS 扩展 |
| liba52／a52dec | 原 liba52 家族 | 本轮未成功核实维护站当前稳定版本 | 先锁定官方来源和归档；不把未经核实的 0.8.0 写成已确认最新版 |
| libopenmpt | 尚未接入 | [0.8.9](https://lib.openmpt.org/libopenmpt/download/)，有专门旧系统构建 | MOD 的现代化候选；另验原渲染参数 |
| TAK SDK | AddIn 2.2.0；根目录另有 2.3.3 | 可确认 2.3.3 候选，未确认作者站更晚版 | 先查实际加载路径；官方库与独立源码路线分开 |
| FFmpeg | 若用于新增／替换后端 | [9.0.2](https://ffmpeg.org/download.html) | 精简所需模块；XP／Win7 编译、导入、CPU、实际播放全部另验 |
| FAAC | 尚未接入 | [2.2](https://github.com/knik0/faac/releases/tag/faac-2.2) | 新 AAC 编码器候选；不是 Nero ABI 原位升级 |

Monkey's Audio 升级还需重验 `cmake/prepare_mac.py` 的 StartEx 错误传播适配、旧版本支持宏、导出裁剪和运行时 SIMD。上游官方存在 XP 安装器不能替代本工程新构建的验收。

FAAD2 的升级需特别注意：官方 2.11.4 增加的 `elst / iTunSMPB` 处理位于其 [frontend/mp4read.c](https://github.com/knik0/faad2/blob/2.11.4/frontend/mp4read.c)。当前项目只静态编译 `libfaad/*.c`，MP4 容器由自己的 `src/mp4.cpp` 处理。因此只把版本号改成 2.11.4，**不会自动获得插件的完整无缝播放**。应先升级核心、更新 SHA-256 和两处头文件适配检查，再单独迁移容器裁剪语义与回归测试。

新库仍采用固定版本／源码归档／SHA-256 的可复现获取方式；库的来源、修改和许可文件继续保留在仓库。运行包、对应源码包和私有测试各按已有工程策略处理。

## 9. 可以共用的实现与不能合并的行为

建议统一到现有 SDK／公共实现：

1. IStream 的 read-exact、短读、64 位 seek、取消、Commit 兼容和保存错误传播。
2. UTF-8／系统代码页、ID3／APEv2／Vorbis comments 的公共基础；封面结构、边界检查和多值标签规则。
3. 引用计数、GUID、CoTaskMem 所有权、异常到 HRESULT 的边界。
4. 文件事务、同文件修改互斥、图片读取上限、内存上限和临时文件清理。
5. 固定依赖、版本资源、体积配置、XP／Win7 导入审计以及插件身份记录。

需要保留在各插件内：编解码器的帧延迟／预热、声道布局、增益和裁剪、定位后状态重置、原工厂顺序、各格式的标签重写语义。不能因为公共方法名称相同，就统一其错误码或 EOF 行为。

兼容原版播放器时，优先复用静态 SDK／头文件代码，或通过可选接口查询使用新服务。若强制导入只有新 ttpcomm 才有的函数，就会使“单独替换插件、保留原 ttpcomm”失效；必须分别声明依赖边界。

## 10. 建议实施顺序

| 次序 | 工作 | 验收结果 |
| --- | --- | --- |
| 0 | 固定原版、当前源码、实际加载 DLL 三方身份；修复 AAC 保存失败留下双 moov | 同一输入的正常／未实现 Commit／真实写失败，失败后可重开且原音频未变 |
| 1 | 审计 APE／OGG／FLAC 的同类流提交；恢复 TTA 信息模式的延迟初始化 | 原宿主保存／失败反馈可信；列表读取信息不提前扫完整索引 |
| 2 | 补现有格式边界：OGG 大文件／保存结果、FLAC 外部标签、AAC 参考时长与复杂 Profile | 每项有原 DLL 和新版成对用例，能区分功能缺失与未验证 |
| 3 | 分开升级 FAAD2、Monkey's Audio SDK | 固定归档和补丁；各自有 PCM／seek／元数据／旧系统回归，不与接口大改混成一批 |
| 4 | 重建 MPC、AC3/DTS | 先恢复原有 SV7／SV8、AC3／DTS 和 ABI，再单列新的编码模式扩展；不扩展后缀却缺解码器 |
| 5 | 重建 CLI、ASF、TAK 适配 | Unicode／取消／进程生命周期；系统 codec 能力枚举；TAK SDK 与可选独立核分别验证 |
| 6 | 重建 MOD | 选择原听感兼容与现代渲染策略，逐项测试节拍、循环、插值、子曲目、定位和配置 |
| 7 | RM、VQF、独立现代 AAC 编码 | 建立真实旧格式样本矩阵；分别声明外部核心依赖、替代算法和未支持分支 |

每个插件独立构建、独立提交；公用 SDK 的更改单独提交后再逐个接入。原宿主兼容先验收，扩展功能随后验收，避免一次替换过多层后无法定位声音／保存差异。当前用户请求是审计，本轮未执行这些修复或提交。

## 11. 验收矩阵与本轮边界

后续每个插件至少按以下矩阵验收：

| 层次 | 必测内容 |
| --- | --- |
| ABI | 原 EXE 与重建 EXE；工厂枚举；IID／虚表；引用计数和失败后的输出指针；对象交错释放 |
| 解码 | 正常／截断／坏头；小到一帧的缓冲；奇数容量；0／中间／末尾 seek；重复 Stop／Start；非有限输出 |
| 流 | 本地、包内只读流、已知／未知长度 HTTP、短读、超时、取消；不可 seek 时不伪造 seek 成功 |
| 标签 | Unicode、多值、未知字段、全部删除、封面；只读拒写；保存失败重开；音频数据不变 |
| 编码 | 短于一帧、整帧、尾帧；实际输入精度；输出解码样本数；配置、析构、磁盘满、并发 |
| 大文件 | 超过 2 GiB／4 GiB 的位置及样本数，不能用小文件测试替代 |
| 系统 | 同一发行哈希在 XP SP3 x86、Win7、Win11；Win11 作为用户指定的 Win10 代理，仍注明无独立 Win10 实测 |
| CPU | SSE2 是多个现有工程的明确要求；仅有 XP VM 通过不能证明非 SSE2 老 CPU 可运行 |

本轮执行的是静态审计、原 DLL 重点反编译、官方版本核查，以及 AAC／OGG／FLAC／APE 提交边界和 TTA 信息模式的本机隔离复现；没有重新宣称全部插件通过 XP／Win7 全矩阵。已有历史测试以各工程文档中的具体哈希和范围为准。

测试、临时音频和 REA 输出全部放在 `rebuild/tests/audio_plugins_audit_20261019`，不进入插件源码仓库或 Actions。未修改用户音乐、原 DLL、播放器部署或生产源码；未执行库升级、发布或 Git 提交。

## 12. 证据索引与覆盖状态

### 12.1 本轮静态证据

REA 6.5.0 通过 Ghidra 12.1.3 核对关键函数；使用公开 CLI 的临时会话，调用完成后已退出。完整记录保存于本地 [REA 索引](../tests/audio_plugins_audit_20261019/rea-index.json)，包含 Evidence ID、目标哈希、操作与地址。下表的伪代码地址不表示已动态执行该分支。

| 目标 | 已追踪的主要路径 | REA 记录与补充来源 |
| --- | --- | --- |
| AC3/DTS | `60053BBC → 600529E3`；四工厂；`60051A78` Reader；`60052703 / 60052872` Decoder | `ac3dts_factory.json / ac3dts_init.json`；完整静态导出 335 函数 |
| MOD | `60453541 → 60453137`；`60452929` 打开；`60452DC0 → 60452B6D` ANSI 文件路径 | `mod_factory.json / mod_init.json`；完整静态导出 569 函数 |
| MPC | `60503D83 → 605039A5`；`60506D68` 双格式；`60502731` Read；`60503466` 增益写入 | `mpc_factory.json / mpc_init.json`；完整静态导出 328 函数 |
| ASF | `601569C7 → 60155B2E`；`60155CAF` 动态 DMO 创建 | `legacy/ttp_asf_0x601569c7.json / ttp_asf_0x60155cc5.json` |
| CLI | 实际入口 `6020142E` 的模板、管道与子进程启动 | `legacy/ttp_clienc_0x6020147f.json` |
| RM | 实际入口 `60601652` 的后端搜索、加载和 API 解析 | `legacy/ttp_rm_0x60601b8c.json` |
| TAK | `100015D7` Read、容量与状态映射 | `legacy/ttp_tak_0x100015d7.json`；本轮默认 Ghidra 未识别 Seek `10001664` |
| VQF | `801D1B4B` Read、交错与夹幅 | `legacy/ttp_vqf_0x801d1b4b.json`；本轮默认 Ghidra 未识别 Seek `801D1C55` |
| AAC 原件 | `60006A97` 四工厂次序；旧分析中的 `60005644` 打开分支 | `rea_aac_factory.json` 及该哈希对应的既有伪代码 |

AC3/DTS、MOD、MPC 的完整伪代码另由 Ghidra `ExportDecompiled.java` 导出，**不是 REA Evidence**；函数数量不代表每个函数已人工验证。PE 目录、导入、资源与 SHA-256 来自只读本地解析，也没有被当作运行证据。

详细本地笔记：[开放内核插件](../tests/audio_plugins_audit_20261019/legacy_open_codecs.md)、[系统／外部内核插件](../tests/audio_plugins_audit_20261019/legacy_system_codecs.md)、[已重建插件对照](../tests/audio_plugins_audit_20261019/rebuilt_parity.md)。这些私有证据不进入 Action 或发行包。

### 12.2 本轮动态证据与历史记录

| 证据 | 可以支持的结论 | 不能据此推导 |
| --- | --- | --- |
| [身份与仓库快照](../tests/audio_plugins_audit_20261019/inventory.json) | 指定文件的哈希、PE 信息和源码版本 | 宿主运行时一定加载该文件 |
| [AAC 保存后重开](../tests/audio_plugins_audit_20261019/aac-reopen-results.json) | 指定 DLL 在受控 Commit 路径出现双 moov／重开失败 | 原版 GUI 每次保存必定损坏，或原 DLL 在简化宿主保存成功 |
| [OGG／FLAC／APE Commit 对照](../tests/audio_plugins_audit_20261019/parity/commit-notimpl-results.json) | 未实现 Commit 时的持久化、回滚和返回值差异 | 所有系统／所有流分支均失败 |
| [TTA flags 对照](../tests/audio_plugins_audit_20261019/parity/tta-flags-results.json) | 原件与新版在只读信息模式的 Read 次数、字节数和返回值 | 合法头但无音频数据的文件能够正常播放 |
| 各项目原有 docs 与测试日志 | 记录中指定构建、样本和系统的历史结果 | 本轮新构建或全部参数组合自动通过旧系统验证 |

Commit 探针输出中的 `PASS`／`saved on release` 是测试程序自身文字；本报告另外检查了 HRESULT、文件哈希和重开结果。尤其 OGG 的“已保存”文字不能覆盖实际文件未改变的事实，APE 的返回失败也不能覆盖编码数据与正常组完全相同的事实。

### 12.3 原始问题覆盖状态

| 用户问题 | 状态 | 本轮答案与剩余边界 |
| --- | --- | --- |
| 哪些音频插件尚未重建？ | 已回答 | 当前工作区 8 个；已排除依赖 DLL、非音频组件与新格式工程 |
| 原实现如何工作？ | 主要路径已回答，完整边界部分回答 | 已核对工厂、输入／输出、外部内核与关键保存分支；TAK／VQF Seek 与全部特殊格式仍待深入 |
| 是否可以完全重建？ | 可行路线已回答；全行为等价尚未证明 | 开放内核适配可重建，外部核心与系统服务分层评估；未承诺取回原源码或零差异 |
| 重建时可以改善什么？ | 已回答 | 按各插件列出流、容量、Unicode、定位、取消、保存、并发和体积方向 |
| 引用库能否更新？ | 已核实主要候选，个别来源未确认 | FAAD2／Monkey's Audio 可更新；Xiph／LAME 等已是核查到的稳定版；a52dec／TAK 更晚版本未确认 |
| 已重建插件是否还有差异？ | 已回答并复现重点问题 | AAC、OGG、FLAC、APE 保存与 TTA 信息模式有本轮证据，其余分开标为静态边界或未验证 |
| 是否已完成修复与所有系统验收？ | 本轮未实施 | 本次是审计；第 10 节为后续修复顺序，第 11 节为必须补做的验收 |
