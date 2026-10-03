# Opus 音频与 ttp_opus 插件可行性分析

分析日期：2026-10-03。

后续补充：已进一步对当前与旧版 `ttp_ogg.dll` 做伪代码和实际读取验证，见 [ttp_ogg 分析与扩展方案](ORIGINAL_OGG_PLUGIN_ANALYSIS.md)。若同时维护 Vorbis 与 Opus，可改用独立构建的双后端 `ttp_ogg` 项目；本篇的单独 Opus 插件仍适用于只增加一种编码、控制现有 Vorbis 回归范围的目标。

## 1. 结论与证据边界

**可以创建独立的 `ttp_opus` 项目，输出同时供原版 TTPlayer 5.7.9 和重建版加载的 x86 `AddIn/ttp_opus.dll`。推荐采用 FLAC 插件的“Reader 内部解码并输出 PCM”结构，复用 ttp_aac 已验证的 ABI、构建与发布框架，内部使用 libopusfile + libopus + libogg。**

这是有二进制、伪代码和现有宿主实现支持的工程判断。当前尚未实现、编译或实测 ttp_opus，不能将 AAC 的成功运行记录视为新插件已经通过兼容验证。

本次完成：

- 对工作区 `AddIn/ttp_flac.dll` 做 PE、导入导出、资源、虚表及 Ghidra 伪代码分析。
- 初次导出 347 个函数；补建虚表入口和短回调后导出 **374/374 个已识别函数**。这不等于已经识别二进制中所有函数，也不代表获得原始源码。
- 用现有本地读取器检查程序，仅加载该 FLAC DLL，验证真实解码和跳转。
- 对照 `ttp_aac/include`、`src`、`cmake`、兼容验证文档以及重建版插件管理器。
- 核对 Opus 官方格式规范、API、版本和许可证。

所有本次反编译、分析脚本、日志和插件副本放在 `rebuild/tests/opus_analysis/`，不加入发行包或 Actions 测试。没有修改现有播放器、AAC 插件或原始 FLAC DLL。

## 2. Opus 音乐文件实际包含什么

### 2.1 编码与容器需要分开理解

Opus 是有损音频编码，结合 SILK 与 CELT 技术，适用于语音和音乐。将 FLAC 转成 Opus 会损失信息；以后再转回 FLAC 只能保存解码结果，不能恢复原音频。编码规范见 [RFC 6716](https://www.rfc-editor.org/rfc/rfc6716)。

日常 `.opus` 通常是 **Ogg 容器内的 Opus**，不能把文件整体直接交给 `opus_decode()`：

```text
Ogg 页 → 还原 Opus 数据包 → Opus 解码 → 处理采样裁剪/声道/增益 → PCM
```

两个必要头包分别是 `OpusHead` 和 `OpusTags`。Ogg 页负责组织数据包、流编号、时间位置和校验；一个包可能跨页。扩展名只是线索，实际打开仍须验证内容。[RFC 7845](https://www.rfc-editor.org/rfc/rfc7845.html)

| 文件或传输形式 | 建议首版范围 | 原因 |
| --- | --- | --- |
| `.opus` 中的 Ogg Opus | 支持 | libopusfile 对应的主要输入 |
| `.ogg` / `.oga` 中的 Opus | 支持 | 内容探测后接受，非 Opus 应交还其他读取器 |
| Ogg Vorbis / Ogg FLAC | 不由 ttp_opus 接管 | 共用容器，不共用音频编码 |
| WebM / Matroska 中的 Opus | 后续独立扩展 | 需要 EBML 解复用器 |
| MP4 中的 Opus | 后续独立扩展 | 需要 MP4 Opus sample entry、时间及裁剪处理 |
| RTP 或裸 Opus 包 | 不作为普通音乐文件首版支持 | 缺少同一套文件组织与时间信息 |

不能以“libopus 能解码”推导“上述所有容器均已支持”。[libopusfile 概览](https://opus-codec.org/docs/opusfile_api-0.12/index.html)

### 2.2 采样率、时长与无缝边界

- libopusfile 的 PCM 输出统一为 **48,000 Hz**。`OpusHead.input_sample_rate` 是原始输入采样率提示，不应直接作为输出 WAVEFORMATEX 的采样率。[OpusHead](https://opus-codec.org/docs/opusfile_api-0.12/structOpusHead.html)
- 应通过库处理 `pre-skip`、末尾裁剪和跨页数据包；手工按“包数 × 固定时长”计算会有误差。
- 文件内部使用 64 位采样位置；旧插件接口使用 DWORD 毫秒。转换要检查溢出，超出接口范围时须明确限制，不可回绕。
- `Seek(ms)` 转成 `ms × 48` 个采样位置，交给 `op_pcm_seek()`，由库完成定位及解码预热。跳转后清空插件自身尚未交给宿主的 PCM 缓存。[跳转 API](https://opus-codec.org/docs/opusfile_api-0.12/group__stream__seeking.html)

正确裁剪能避免插件额外引入开头/末尾静音；跨曲目最终是否无缝仍取决于宿主的曲目切换及输出设备策略。

### 2.3 多声道与串接文件

Opus 可使用不同 channel mapping family。首版建议明确支持 family 0 的单声道/双声道及 family 1 的常用 1–8 声道布局，其他 family 明确拒绝或另行实现，不能把任意通道当作扬声器。

libopusfile 输出采用 Vorbis 声道顺序。多声道交付 Windows 前需要重排，并核对 `WAVEFORMATEXTENSIBLE.dwChannelMask`。例如六声道不能把 Vorbis 的 `FL, FC, FR, RL, RR, LFE` 原样当作 Windows 的 `FL, FR, FC, LFE, BL, BR`。

Ogg chained stream 可以串接多段音频，段与段之间可能改变声道数。旧宿主通常只在打开时建立输出格式，不能在中途偷偷变更缓冲区的通道数。建议：

1. 普通文件及各段格式一致的 chained 文件保持原声道数。
2. 可定位文件在打开时检查所有 link；布局变化时采用明确的固定立体声降混策略。
3. 若首版不实现该策略，则应显式报告不支持，不能播放到一半才错位。
4. 非 seekable 流无法预先保证后续布局；网络流支持单独验收。

API 已提供固定立体声读取方式；保留多声道的方式需要调用方处理链间变化。[解码 API](https://opus-codec.org/docs/opusfile_api-0.12/group__stream__decoding.html)

### 2.4 标签、封面和增益

`OpusTags` 提供 UTF-8 文本字段，可映射 Title、Artist、Album、AlbumArtist、Genre、Date、TrackNumber、DiscNumber、Comment、Lyrics 等。应保留未知字段和重复值，不能把内部存储简单压成单值字典。宿主只接受零结尾字符串时，需要对嵌入 NUL 的异常文本定义显示策略。[注释规范](https://xiph.org/vorbis/doc/v-comment.html)

封面通常是 `METADATA_BLOCK_PICTURE`，内容为 Base64 编码的 FLAC picture 结构。libopusfile 有 `opus_picture_tag_parse()`；解出的 JPEG、PNG 等图片字节可接到现有 Thumbnail 接口。还要处理 URL 型图片条目、超长字段及损坏 Base64，不能把 URL 当图像字节。[标签与封面 API](https://opus-codec.org/docs/opusfile_api-0.12/group__header__info.html)

增益是单独的兼容工作：

- 必须保留并应用 Opus 头中的 output gain。
- libopusfile 默认使用 **OP_HEADER_GAIN**，并不会默认再叠加 R128_TRACK_GAIN；建议明确设置此模式以固定行为。
- `R128_TRACK_GAIN` / `R128_ALBUM_GAIN` 是 Q7.8，即 1/256 dB 单位的有符号数。不要将字符串中的整数直接当作 dB。
- 当前重建版 AudioEngine 查询的是 `replaygain_track_gain` / `replaygain_track_peak`，没有直接消费 R128 字段。
- 首版可应用 header gain 并如实暴露原始 R128 标签。若要由宿主的“自动增益”开关控制 R128，需要定义虚拟字段转换、参考响度及优先级，并实测两端宿主。不能让插件应用一次 R128、宿主再应用一次。
- ReplayGain 扫描写入普通 ReplayGain 字段，不等同于标准 R128 测量。不可仅换字段名就写成 R128，更不能为写入扫描结果直接修改 header gain。

默认增益行为见 [op_set_gain_offset](https://opus-codec.org/docs/opusfile_api-0.12/group__stream__decoding.html)；具体落地前还需核对宿主增益计算及扫描保存策略。

## 3. ttp_flac.dll：伪代码与二进制结论

### 3.1 分析对象

```text
文件：AddIn/ttp_flac.dll
大小：86,152 字节
SHA-256：e2833c023377787fa9705d52215d3a02b7f5a742bdda4012f9d639c2e1f64dbd
架构：PE32 x86
ImageBase：0x60300000
唯一命名导出：ttpGetSoundAddIn，RVA 0x4EEA
导入模块：KERNEL32.dll、ole32.dll、MSVCRT.dll
内部版本字符串：reference libFLAC 1.2.1 20070917
```

这是本机这一份 DLL 的结论，不能自动套用到所有历史版本的 ttp_flac.dll。libFLAC 版本来自二进制字符串，不是根据文件名推测。

### 3.2 注册的服务并非“FLAC Decoder”

`ttpGetSoundAddIn`（`60304EEA`）创建 AddIn 对象；其虚表 `60312860` 的 slot 3 指向 `60304F93`。

`60304F93` 的枚举行为：

| index | category | 工厂 | 资源中的格式声明 |
| --- | --- | --- | --- |
| 0 | ReaderCreator | `60304FE9`，虚表 `60312870` | 32000：`FLAC 音频文件(*.flac;*.fla)` |
| 1 | ReaderCreator | `6030513D`，虚表 `60312888` | 32001：`TTA 音频文件(*.tta)` |
| ≥ 2 | 无 | 返回 `E_INVALIDARG` | 枚举结束 |

category 的原始 16 字节解读为 `476D15A5-D863-416A-8A59-A9C7D72CE04E`，与 ttp_aac 及重建版 ReaderCreator 完全相同。

**该 DLL 没有枚举独立 Decoder 或 Encoder。** Reader 已经负责读取、解码和交付 PCM。这是 ttp_opus 采用一个 Reader 工厂即可工作的直接先例。

### 3.3 FLAC Reader 的播放链

Reader 虚表在 `60312170`，与 `ttp_aac/include/ttp_aac_abi.h` 的 16 个公开槽位对应。

| 槽位 | 地址 | 实际行为 |
| --- | --- | --- |
| 0–2 | `6030112C` 等 | QueryInterface / AddRef / Release |
| 3 | `603012BC` | 持有 IStream、探测文件、创建内部 FLAC 解码器及流回调、读取元数据、构建 PCM 格式 |
| 4 | `60301620` | 返回流的能力位 |
| 5 | `60301644` | 返回缓存的毫秒时长 |
| 6 | `60301661` | 返回分配给宿主的格式副本 |
| 7 | `60301682` | 建议缓冲大小为 `nBlockAlign × 1024` |
| 8 | `603016A6` | `FLAC\|FLAC Audio` 描述 |
| 9 | `603016D8` | 压缩音频码率 |
| 10 | `603016F5` | 读取格式中的位深 |
| 11 | `60301717` | 将宿主回调交给流的可选扩展接口 |
| 12、13 | `603011BC` | Start / Stop：直接成功 |
| 14 | `6030172C` | 按宿主容量输出 PCM，保存未读完的帧缓存，结束时返回 S_FALSE |
| 15 | `60301876` | 毫秒换算成采样位置，清空缓存并调用内部 FLAC seek |

重要细节：

- `60301000` 读取并回退流位置，验证 `fLaC` 标识；不是仅依据后缀接受文件。
- `603012BC` 构造的是 `WAVE_FORMAT_PCM`，而不是 FLAC 压缩格式。宿主不必知道 FLAC 压缩包长什么样。
- `6030238A` 等回调把 libFLAC 的读、定位、位置、长度请求映射到宿主 IStream。
- `60302469` 把 libFLAC 按声道分开的整数采样转成交错 PCM；`603024EA` 根据 STREAMINFO 建立采样率、声道、位深等参数，同时读取注释和图片块。
- `6030172C` 保留大帧的剩余 PCM，适配较小的宿主读取请求，并拒绝运行中意外变化的采样格式。
- `60301876` 的反编译结果显示旧实现没有检查内部 seek 返回值；新 Opus 插件应传播定位失败，不应为模仿旧代码而报告虚假成功。

### 3.4 标签与封面接口

`6030112C` 还支持：

- Metadata：`7AD84E00-5FEF-4481-B532-FBBD677E67C2`，对象偏移 `+4`，虚表 `60312150`。
- Thumbnail：`B5E770AF-DFB0-43E5-9B0C-3EE98E7B6248`，对象偏移 `+8`，虚表 `60312128`。

`60301F0B` 按 `KEY=VALUE` 读取 Vorbis Comment；`60301FCC` 把 FLAC 图片块装入供宿主读取的图片结构。

原 DLL 的 Thumbnail 策略明确限制：`60301B84` 返回最大 **60,000 字节**，`60301B9E` 返回最多 **1 张**。因此“原版这一插件只展示一张小封面”不是 FLAC/Opus 文件格式本身的限制。新插件应报告自己的合理上限，按前封面优先选择兼容展示项，并实测原版的图片处理能力。

标签设置 `60301A8F` 检查能力位 `4`，修改内存字段并标记 dirty。析构 `603011DD` 释放解码器和流，dirty 时经 `6030236F` → `6030205A` / `603021F9` 写回注释和图片。这说明**保存行为包含 Reader 生命周期**，不能只实现一个总是返回 S_OK 的 Set 接口。

这套 FLAC 元数据块写入算法不能直接用于 Opus：OpusTags 位于 Ogg 包中，长度变化会影响页组织与 CRC。

### 3.5 本次本地运行检查

使用现有 `aac_validation.exe` 的通用 Reader 解码路径，在隔离目录中只放原 FLAC DLL；测试输入是本地 3 秒 FLAC fixture。

```text
readers=2 decoders=0 encoders=0
format=1/16 rate=44100 channels=2 duration=3000
bytes=529200 blocks=131
seek 0 / 1500 / 2970 ms：均成功且随后读到 PCM
PASS
```

字节数正好为 `44100 × 3 × 2 × 2`。日志在 `tests/opus_analysis/flac-native-decode.log`。该旧工具仅对 float 输出计算 RMS/peak，因此日志中的 `rms=0 peak=0` **不能解读为此次整数 PCM 无声**。

本次未重复运行 FLAC 在 XP、Win7 或原版 GUI 中的测试，也未做 Opus 音频测试。

## 4. ttp_aac 项目中哪些能复用

| 文件或模块 | 已实现内容 | 对 ttp_opus 的意义 |
| --- | --- | --- |
| `include/ttp_aac_abi.h` | AddIn、Creator、Reader、Decoder、Metadata、Thumbnail 等 ABI | 提取通用定义，保持 GUID、stdcall、槽位及 x86 结构布局 |
| `src/plugin.cpp` | 导出、类别枚举、工厂、引用计数 | 保留框架，新插件初期仅枚举一个 Opus ReaderCreator |
| `src/common.h` | COM 所有权、HRESULT 异常边界、CoTaskMemAlloc、IStream 与字符串工具 | 可移植；须收紧不规范 UTF-8 与长度的处理策略 |
| `src/reader.cpp` 的 raw AAC 路径 | 直接输出 float PCM、处理小容量读请求和缓存 | 比 MP4 压缩包路径更接近 Opus Reader |
| `src/reader.cpp` 的 MP4 路径 | 输出压缩包，交给独立 Decoder | 可参考接口调用关系，但不是首选结构 |
| `src/decoder.cpp`、FAAD2 | AAC 解码 | 不能解码 Opus，不应复制到新项目 |
| `src/mp4.cpp` | MP4 box、sample table、AAC 配置、标签保存 | 不适用于 Ogg Opus；未来 MP4 Opus 是另一项扩展 |
| `src/nero.cpp` | 可选 Nero AAC 编码器 | 与 Opus 播放无关 |
| `cmake/legacy_windows.cmake` | 现代 MSVC + VC-LTL + YY-Thunks，XP 目标 | 可移植并对新依赖重新做静态导入审计 |
| `cmake/faad2.cmake` | 固定版本下载、SHA-256、构建目录适配 | 复用方法，依赖换成 Opus 三个库 |
| 版本、资源、打包脚本 | 北京时间日期版本、DLL 属性、独立发布 | 保持与 AAC 一致的项目独立性和版本规则 |

当前 AAC 枚举：MP4 Reader、AAC Reader、AAC Decoder、Nero Encoder。它并不要求所有新插件也实现这四项。尤其是 DecoderCreator 的 slot 5 接收的是 **音频子类型 GUID**，而 ReaderCreator 的 slot 5 返回格式描述，不能混用。

AAC 项目已有原版 TTPlayer 5.7.9 及 XP 的实际模块加载/播放记录，也有重建版和 Win7 验证。详见 [原版宿主兼容记录](../../ttp_aac/docs/AAC_ORIGINAL_HOST_COMPATIBILITY.md)、[XP 验证](../../ttp_aac/docs/AAC_XP_VALIDATION.md)。这些记录证明 ABI 路线可行，新 Opus DLL 仍需重新验收。

## 5. 两端宿主如何接入

### 5.1 原版

原版 `TTPlayer.exe`：

- `004CABC0` 扫描 `ttp_*.dll`。
- `004C88D9` 加载 DLL 并获取 `ttpGetSoundAddIn`。
- `004CABC0` 对枚举返回的类别再执行对应 QueryInterface，登记工厂。
- `004CBC1F` 按格式提示挑选 Reader；具体内容由 Reader 打开判定。

因此新 DLL 命名为 `ttp_opus.dll` 并放到 AddIn 目录，具备正确工厂和 PCM Reader 后，原则上无需修改原版 EXE 的解码主链。原版对新扩展名的文件选择、拖入、目录扫描、关联界面和错误后缀行为仍应逐项实测，不能只凭 DLL 被枚举就称为完整支持。

### 5.2 重建版

`src/plugins/plugin_manager.cpp` 已支持同一 ABI，并在 Reader 输出 PCM 或 IEEE float 时直接构建 LegacyReaderSession。读取到压缩格式才寻找另一个 Decoder，因此不需要为 Opus 发明宿主私有格式。

需要留意现有两条探测路径不同：

- `src/audio/extension_correction.cpp` 已识别 `OpusHead`，用于后缀提示。
- `src/audio/format_probe.cpp` 的 Reader 路由仅读 16 字节，没有 Ogg Opus 分流；PluginManager 还会依据工厂声明的后缀筛选。

所以加载新插件后，正常 `.opus` 可以由新工厂接收；声明 `.ogg/.oga` 后可以参与相同后缀的探测。但**错误命名为 `.flac/.mp3` 的 Opus 不能仅靠目前的后缀提示保证进入新 Reader**。实施时应补充有边界的 Ogg 头探测与路由测试，避免大封面、非目标 Ogg 流及多个 Reader 的优先级产生回归。

本机是否通过某个系统解码器或其他插件偶然能播放 Opus，与这个兼容方案不同；当前代码证据不能证明已经具有完整、跨系统的原生 Opus 支持。外部转换器的 `libopus` 编码配置也不等于输入端已支持播放。

## 6. 推荐的 ttp_opus 结构

```text
ttp_opus/
  CMakeLists.txt / build.ps1
  include/ttp_sound_abi.h
  src/plugin.cpp          # ttpGetSoundAddIn 与 ReaderCreator
  src/reader.cpp          # Reader、输出格式、缓存、定位、生命周期
  src/stream_callbacks.*  # IStream ↔ OpusFileCallbacks
  src/metadata.*          # OpusTags ↔ 宿主 Metadata / Thumbnail
  src/ogg_tags_writer.*   # 后续标签安全写入
  src/ttp_opus.def / version.rc.in
  cmake/                 # 固定依赖、XP 兼容、日期版本、打包、导入审计
  third_party/            # 版权、许可证、来源与版本说明
  docs/
```

这是建议目录，尚未创建。项目应与 ttp_aac、rebuild 分别构建；插件可复制给两端宿主使用。

### 6.1 Reader 核心契约

| 方法 | 建议实现 |
| --- | --- |
| `Open(IStream*, flags)` | 持有流；建立回调；验证 Ogg Opus；打开 libopusfile；建立稳定的输出格式和元数据 |
| `Capabilities` | 只报告实际可定位/可编辑能力；首版未实现保存则不报告 bit 4 |
| `Duration` | 从可播放 PCM 总量换算毫秒；不可定位且未知时长时明确报告未知 |
| `Format` | 首选响应播放路径的 float 请求；兼容提供 16-bit PCM；多声道补齐布局 |
| `BufferSize` | 报告合理建议值，不能假设宿主总按这个大小读取 |
| `CodecName` | 例如 `OPUS\|Opus Audio` |
| `Bitrate` | 压缩流码率，不应返回 float PCM 的数据率 |
| `BitDepth` | 指输出 PCM 的位深；Opus 本身没有无损 PCM 意义上的“原始 16/24 bit” |
| `Read` | 使用库解码；容量对齐完整 PCM 帧；正确保留余量；EOF 前先输出残余数据 |
| `Seek` | 验证能力和目标范围，调用库定位，清除旧缓存，返回实际结果 |
| `Metadata / Thumbnail` | Unicode 字段、稳定图片内存、正确报告未实现的写入操作 |

`Read` 最容易发生单位错误：libopusfile 的 buffer capacity 是采样值个数，成功返回值却是**每声道采样数**；最终字节数需要再乘以声道数和每采样字节数。不能把容量字节数直接传给库，也不能把返回的帧数直接当字节长度。

边界处统一采用 COM 分配和释放约定；所有异常转换为 HRESULT，禁止 C++ 异常穿越 DLL；各 Reader 拥有独立解码器、缓存、流和标签，满足并行加载元数据/扫描增益，不在插件内部额外启动播放线程。

### 6.2 IStream 回调

用 `op_open_callbacks()` 直接读取宿主传来的流，避免重新按窄字符路径 fopen 丢失中文路径、压缩包成员或宿主流语义。

IStream 的 `S_FALSE` 搭配非零读取字节数仍是有效尾部数据，不能当作 I/O 失败；库所需的 EOF 为 0 字节，错误为负值。seek/tell 使用 64 位，回调不允许异常跨越 C 库边界。[回调 API](https://opus-codec.org/docs/opusfile_api-0.12/group__stream__callbacks.html)

建议 Reader 统一持有 IStream，库回调不自行释放，销毁时先 `op_free` 再释放流。特别注意 `op_open_callbacks()` 失败时不会代为调用 close；不能在失败分支漏释放或重复释放。[打开与关闭 API](https://opus-codec.org/docs/opusfile_api-0.12/group__stream__open__close.html)

### 6.3 标签写入不是解码库自动提供的功能

libopusfile 的标签增删 API 修改内存对象，不会替调用方保存 Ogg 文件。完整编辑需要：

1. 解析并保留 OpusTags、vendor、未知字段及应保留的尾部数据。
2. 改写标签包，保持压缩音频包原样，不重新编码音乐。
3. 正确维护页分段、连续包标志、granule position、serial、序号、BOS/EOS 与 CRC。
4. 有足够 padding 的等长路径和需要重排页的增长路径分别处理。
5. 用临时文件、完整校验和明确提交点处理写入；失败恢复原文件。宿主持有流时的文件共享/替换限制需要专门设计，不能假定所有 IStream 都对应可替换的普通文件。
6. 对 chained / multiplexed、只读和非文件流定义保存范围；没有支持时在能力层关闭编辑。
7. 保存后重开，核对标签、图片、时长及压缩音频包摘要。

原 FLAC 插件在最终释放 Reader 时写盘，这一生命周期需要兼容，但新实现不能仅在无法报告错误的析构函数中无条件吞掉写入失败。应尽量在可返回 HRESULT 的操作中验证和完成保存，并由宿主重开校验持久化结果。

## 7. 依赖、旧系统和发行策略

本次查阅官方发布页，建议固定：

| 依赖 | 版本 | 作用 |
| --- | --- | --- |
| libopus | **1.6.1** | 音频解码 |
| opusfile | **0.12** | Ogg Opus 文件读取、定位、裁剪和标签解析 |
| libogg | **1.3.6** | Ogg 基础容器处理 |

版本依据：[Opus 官方下载](https://opus-codec.org/downloads/)、[Xiph 官方下载](https://xiph.org/downloads/)。正式构建固定下载地址与 SHA-256；源码留在构建缓存，仓库保留各依赖完整版权/许可证和来源说明。

建议将三库静态链接进一个 DLL，使用现代 MSVC x86、VC-LTL、YY-Thunks，保持与 AAC 一致的 XP/SSE2 基线。libopusfile 的 opusurl/OpenSSL 路径不纳入首版；流访问由宿主提供，因此播放本地音乐不必引入额外 HTTPS 库。

以体积优先：关闭上游命令行、测试目标、DRED/OSCE 等非首版目标，启用 Release 大小优化与链接裁剪。1.6.1 确实提供 DRED/OSCE 和 x86 指令分派开关，但应按上游选项构建，不能假设存在任意名称的“decoder-only”开关。[Opus 1.6.1 CMake](https://raw.githubusercontent.com/xiph/opus/v1.6.1/CMakeLists.txt)

不能为缩小体积全局启用 AVX2；旧系统 CPU/操作系统状态保存能力要一起验证。沿用 AAC 的兼容框架只证明方法可复用，新 DLL 仍须检查 PE 目标版本、全部静态导入及运行时指令路径。当前没有新 DLL 的实际文件大小数据。

libopus、opusfile、libogg 使用宽松的 BSD 类许可证，通常不产生 FAAD2 GPL 的对应源码交付要求，但必须保留各版本实际要求的版权、条款和免责声明，Opus 还应保留相关专利授权文件。二进制发行所附材料也必须满足许可证，不能认为“仅仓库留一行声明”一定足够。[Opus 许可证](https://opus-codec.org/license/)、[opusfile COPYING](https://raw.githubusercontent.com/xiph/opusfile/v0.12/COPYING)、[libogg COPYING](https://raw.githubusercontent.com/xiph/ogg/v1.3.6/COPYING)

如继续采用“压缩包只有 DLL + SHA256SUMS”的发布形式，需要另外确保与发行一起提供完整 notices，例如配套许可证附件，并核对每项依赖要求。

## 8. 实施顺序与验收

### 第一阶段：播放和读取

1. 独立项目、ABI、固定依赖、XP 导入审计。
2. 单/双声道 Ogg Opus PCM Reader，完整 EOF、跳转和缓存处理。
3. 标签与 JPEG/PNG 封面读取，明确只读能力。
4. family 1 多声道重排、同格式 chained 文件；变化格式的明确策略。
5. 原版及重建版各自在 Windows 11、Win7、XP 验证真实加载模块、实际 PCM 和播放进度。

### 第二阶段：完整功能

1. OpusTags 安全写入和图片增删，Unicode/未知字段/重复值保留。
2. R128 与宿主 ReplayGain 的衔接及扫描保存；避免双重增益。
3. 重建版错误后缀路由、打开过滤器、目录添加和系统关联集成。
4. 原版属性窗口、保存、歌词、增益、转换输入的实际兼容性验证。

### 第三阶段：按需扩展

非 seekable 网络流、更多 channel family、WebM/MP4 容器，以及基于 libopusenc 的编码器可分别扩展。它们不应阻塞普通本地 `.opus` 的首版播放，也不应被提前标为支持。

### 测试矩阵

| 类别 | 必须验证的情况 |
| --- | --- |
| 内容识别 | `.opus/.ogg/.oga`；非 Opus Ogg；错误后缀；中文和长路径 |
| 解码 | SILK/CELT/混合、单/双声道、多声道独立音调、不同包长及码率 |
| 边界 | pre-skip、末尾裁剪、短文件、空音频、损坏页/包、截断、OP_HOLE 有界恢复 |
| 缓冲 | 小容量和非整帧请求、残余 PCM、EOF 后重复读取、Seek 后无旧数据 |
| 定位 | 0、中间、最后几十毫秒、末尾、越界；检查目标附近实际 PCM |
| 串接 | 同布局和不同布局 link、不同标签、时间总量 |
| 元数据 | UTF-8、重复字段、自定义字段、AlbumArtist、Genre、Lyrics |
| 图片 | JPEG、PNG、多图、无封面、大图、损坏 Base64、URL 类型 |
| 增益 | header 0/正/负，R128 有无，宿主增益开关、扫描前后，无重复应用 |
| 编辑 | 增长/缩短、删除、只读、保存失败、保持音频包和无关元数据 |
| 并行 | 多 Reader、多曲目元数据加载和扫描，状态隔离 |
| 宿主 | 原版 5.7.9 与重建版；XP、Win7、Win10/11；输出设备及转换输入 |

以官方参考解码结果核对时长、有效采样数、声道和增益；浮点/SIMD/抖动路径需要约定容差或固定配置，不应只拿任意两个解码器的全文件字节摘要当作唯一音质判断。

**最终建议：采用“FLAC 式直接 PCM Reader + AAC 的兼容工程框架 + 官方 Opus 三库”，先完成可验证的本地播放/标签/封面读取，再补齐写入及增益集成。**
