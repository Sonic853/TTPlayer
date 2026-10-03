# ttp_ogg.dll 伪代码分析与 Vorbis / Opus 扩展方案

日期：2026-10-03。

## 1. 结论

**可以按 ttp_ogg 的外部接口和行为重写源码，再加入 Opus 解码后端。若目标是同时维护 Vorbis 与 Opus，它比以 FLAC 插件为主要参照更合适。**

可复用的是 Ogg 文件接入、IStream 回调、PCM Reader、文本标签和宿主生命周期的设计；不能把内部 Vorbis 解码器换个名字就变成 Opus 解码器，也不能只给原 DLL 增加 `*.opus` 字符串。

推荐新建独立构建的 `ttp_ogg` 源码项目，最终输出新的 `ttp_ogg.dll`，内部按实际头包选择 Vorbis / Opus 后端。继续沿用 `ttp_aac` 已验证的 ABI 定义、现代编译器、旧系统兼容和日期版本框架。

本次仅做分析、原 DLL 的隔离验证与文档，没有替换用户现有插件，也没有生成新的可发行 DLL。

## 2. 必须区分两份 DLL

当前 AddIn 目录的 DLL 与旧版安装目录的 DLL 不同，不能混用函数地址或依赖结论。

| 项目 | 当前工作区版本 | 旧版参照 |
| --- | --- | --- |
| 路径 | `AddIn/ttp_ogg.dll` | `TTPlayer5719/AddIn/ttp_ogg.dll` |
| 大小 | 192,352 字节 | 142,256 字节 |
| SHA-256 | `ee58d0603a5c4c7596219590d5e92e12748187b395e14dc1275256e0904dcdc5` | `f04295825000c93370c195f6cfaf576d1b52ad3eddb8dbc8c41cf5a5de0f4174` |
| 架构 / ImageBase | x86 / `60550000` | x86 / `60550000` |
| 入口地址 | `605542FD` | `60553307` |
| C 运行库 | `MSVCR110.dll` | `MSVCRT.dll` |
| 内部证据 | 导出表模块名为 `dec_ogg.dll`；包含 libVorbis 1.3.3 字符串 | 导出表模块名为 `ttp_ogg.dll`；未据此确认精确 libVorbis 版本 |
| 本次成功导出的已识别函数 | 620 / 620 | 初次 350，补齐接口入口后 363 / 363 |

函数导出成功不等于整个二进制的所有函数都已识别，也不等于取得原始源代码。旧版写入函数的部分反编译类型/栈变量明显失真，本报告以两版对照、较清晰的当前版代码、汇编及本地行为共同判断。

本地证据均在 `rebuild/tests/ogg_analysis/`，不提交测试代码或加入 Actions 运行。

## 3. 插件实际注册了什么

两版都只有一个命名导出 `ttpGetSoundAddIn`，返回 AddIn 对象。

枚举函数：当前版 `60554095`，旧版 `605533C3`。

- index 0：返回 ReaderCreator。
- category GUID：`476D15A5-D863-416A-8A59-A9C7D72CE04E`。
- index ≥ 1：返回 `E_INVALIDARG`，结束枚举。
- 当前版资源 32000：`Vorbis/Ogg 音频文件(*.ogg)`。
- 没有注册独立 Decoder，没有注册 Encoder。

这不是一个“所有 Ogg 编码通吃”的插件，而是一个 **Ogg Vorbis Reader**。Ogg 是容器，Vorbis 与 Opus 是不同编码。官方 `ov_open_callbacks` 的明确失败条件包括输入不含 Vorbis；Opus 则由另一套 libopusfile API 处理。[Vorbis 打开接口](https://xiph.org/vorbis/doc/vorbisfile/ov_open_callbacks.html)、[Opusfile 概览](https://opus-codec.org/docs/opusfile_api-0.12/index.html)

## 4. Reader 播放结构

### 4.1 外部接口

当前 Reader 虚表 `6056D7D4`；旧版 `60562168`。公开槽位与 `ttp_aac/include/ttp_aac_abi.h` 一致。

| 方法 | 当前版地址 | 旧版地址 | 行为 |
| --- | --- | --- | --- |
| QueryInterface | `605527F4` | `60551101` | IUnknown、Reader、Metadata |
| Open | `6055268D` | `605511FB` | IStream 回调打开 Vorbis，建立格式、时长、标签 |
| Capabilities | `605521B6` | `605512E7` | 网络、定位、编辑能力位 |
| Duration | `60551FE8` | `6055131C` | 缓存的毫秒时长 |
| Format | `605521DC` | `6055133C` | 以 COM 内存分配返回输出格式副本 |
| BufferSize | `60551F7E` | `60551360` | `nBlockAlign × 1024` |
| CodecName | `60551FA7` | `60551387` | `Ogg\|Vorbis/Ogg Audio` |
| Bitrate | `605522BD` | `605513B9` | Vorbis 压缩码率 |
| BitDepth | `605522E8` | `605513E2` | 固定返回 16 |
| SetCallback | `60553325` | `605513FB` | 转发宿主流回调 |
| Start / Stop | `605527EF` / `6055333C` | 均为 `60551176` | 简单成功返回 |
| Read | `605528F1` | `60551410` | 填充宿主缓冲区 |
| Seek | `60552EFA` | `605515F6` | 毫秒转秒，调用 Vorbis 时间定位 |

### 4.2 打开阶段

当前版 `6055268D` 的主要流程：

1. 由 `6055323E` 持有宿主 IStream，读取 Stat、记录文件名/打开方式，探测 Seek 能力。
2. 通过 `60557E57` 打开内置 Vorbisfile。其参数、内部调用及控制流对应 `ov_open_callbacks`。
3. 获取当前流的采样率和声道数，取得总时长。
4. 分配 `WAVEFORMATEX`，填写 `wFormatTag=3`、`wBitsPerSample=64`、`nBlockAlign=channels×8`。
5. 读取 Vorbis Comment，映射为宿主 Metadata。

当前版开 Vorbis 失败时返回 `0x8BDA0007`；旧版返回 `E_FAIL`。两者都不是 Opus 解码分支。

### 4.3 一个重要细节：实际交付 64 位浮点 PCM

两版的真实输出格式是 **IEEE float / 64-bit**，不是 Read 方法交付 16-bit 整数。

`BitDepth()` 固定返回 16 是另一个查询接口的旧行为，不能拿这个数值计算音频缓冲区长度。实际缓冲区布局以 Format 为准。

当前版解码核心 `6055299A`、旧版 `60551470`：

- 调用对应 `ov_read_float` 的函数，取得各声道分别存放的 float 采样。
- 把 float 提升为 double，交错写入宿主缓冲区。
- 以完整采样帧数计算输出长度。

这种 float→double 转换不会恢复压缩中丢失的精度。重写 Vorbis 分支时可先保持 64-bit 交付以减少接口回归；Opus 分支可以设计为 32-bit float 或统一提升为 double，但必须让 Format、Read 字节数和实际宿主输出链一致。[ov_read_float](https://xiph.org/vorbis/doc/vorbisfile/ov_read_float.html)

### 4.4 IStream 回调与网络支持边界

当前版回调表 `6057E000`：read `60551000`、seek `60551018`、close `6055103B`、tell `6055103E`。

流读适配器 `60552873` 优先查询宿主可选流接口；存在时走带事件和 60,000 ms 参数的读取，否则调用标准 IStream::Read。说明它利用宿主提供的网络/文件流，没有自行创建完整 HTTP 客户端。

旧版 seek 回调 `60551000` 额外拒绝 URL 标记的流，`605512E7` 也会清除这类流的可定位位；当前版主要依赖实际 Seek 能力。两者在网络定位政策上并非完全相同。

新实现宜由宿主负责 I/O，按流的真实能力报告可定位性；不要根据“名字看起来是 URL”就自动认为能定位，也不要重新 fopen 丢失宿主流语义。非 seekable 首包探测需要缓存已读取的前缀，不能只依赖 Seek(0)。

### 4.5 多声道与 chained stream

两版只有六声道的显式重排：

```text
输入 Vorbis 索引：0, 1, 2, 3, 4, 5
输出读取顺序：  0, 2, 1, 5, 3, 4
```

其余声道数走直接按索引交错路径。新插件应明确处理完整支持范围内的各声道布局，不能只复制六声道分支便宣称支持所有多声道。

解码核心检查当前逻辑流编号：切换到新 link 后，若采样率或声道数不同，返回 `E_NOTIMPL`。本次同采样率链式样本通过；44.1 kHz→48 kHz 的样本会提前停止，详见测试部分。

定位走与 `ov_time_seek()` 相符的路径，参数以秒为单位，返回失败会转换成 HRESULT；定位成功后重置当前 link 标记。[官方定位契约](https://xiph.org/vorbis/doc/vorbisfile/ov_time_seek.html)

## 5. 已确认的读取错误传播问题

当前版 `605528F1` 调用 `6055299A` 后，没有检查返回的 HRESULT，直接以输出长度是否为 0 决定返回 S_OK 或 S_FALSE；旧版 `60551410` 有同样结构。

已对当前版 `60552936`–`6055294B` 的汇编核对：内部调用返回后直接设置缓冲长度，没有检查 EAX 中的解码错误。

因此核心即使报告损坏数据、链间格式不兼容等失败，外层也可能把它当 EOF。发生错误前已写入当前缓冲区、尚未更新最终长度的有效音频也可能丢失。

新实现需要：

- 分开正常 EOF、可恢复的数据洞、不可恢复解码错误。
- 给恢复循环设进度检查和次数上限，避免损坏输入导致忙循环。
- 已有有效 PCM 与后续错误的交付策略明确化，不把错误永久抹掉。
- 处理小于一帧、非对齐容量；不能因为库收到 0 采样容量就返回 EOF。

这是重写时应修正的旧行为，不应为了“与原版一致”而复制。

## 6. 标签编辑的实际实现

### 6.1 文本标签

当前版 Metadata 虚表 `6056D81C`；旧版 `60562148`。

当前版 `60552B41` 用 UTF-8（代码页 65001）转为 UTF-16，以第一个 `=` 分割字段；没有有效键名的条目归入 comment。读取时保留条目数组，Get 按不区分大小写的键名查找。

`60552FF4` / 旧版 `6055174E`：

1. 检查可编辑能力位 `4`。
2. 标记 dirty。
3. 更新第一个匹配条目或添加新条目。
4. 空值在写盘时被过滤，相当于删除该值。

当前版还有旧版没有的通用标签后备对象：必要时动态取得宿主 `CreateStdContent`，在没有 Vorbis 注释时回退查询；setter 也会通知这个对象。它不是 OpusTags 解析器。

同名多值必须单独设计：旧 Get/Set 主要针对第一个匹配值，不能据此认为它已完整实现“替换或删除所有同名条目”。

### 6.2 最终释放时保存

当前析构 `60551491`：结束 Vorbis 解码，dirty 时调用 `60553619`，再释放流和标签。旧版相应为 `60551197` → `6055195C`。

当前 `60553619` 的可辨识写入流程：

1. 必要时通过宿主 `CreateStreamOnFile` 以读写方式重开源文件。
2. 在源目录生成临时文件。
3. `605559D2` 解析 Vorbis 头和注释；重新生成注释内容。
4. `60555C17` 重新组织 Ogg 输出。
5. 将临时文件按 8 KiB 分块复制回源流，设置最终文件长度。
6. 成功路径关闭并删除临时文件。

注意：它是“临时生成后复制回原文件”，**不等于原子替换**。复制途中失败、空间不足、进程中断、临时文件清理、ANSI 路径转换，以及析构中无法直接向调用方报告错误，都需要在重写时改进。这里只确认静态风险，未声称这些失败场景已逐项复现。

### 6.3 这套写入为什么不能直接用于 Opus

三个具体函数证明它依赖 Vorbis 编码规则：

- `605559D2`：验证 Vorbis 头，取得三个头包；保存 identification 和 setup/codebook 头。
- `605556FF`：写出类型 3、`vorbis` 字符串、vendor、注释列表，最后写 Vorbis framing bit。
- `605556CB`：用前后 Vorbis block size 的四分之一之和计算包的采样推进量；`60555C17` 据此处理 granule position。

Opus 则是 `OpusHead` + `OpusTags` 两个头包，时间位置按 48 kHz 计算，还有 pre-skip、末尾裁剪等规则。Vorbis framing bit 和 block size 推导均不能照搬。[Vorbis 规范](https://xiph.org/vorbis/doc/Vorbis_I_spec.html)、[Ogg Opus 规范](https://www.rfc-editor.org/rfc/rfc7845.html)

这些解析、编辑和写出结构与 Xiph `vcedit` 系列实现相符，但本次没有确定 DLL 对应的精确上游提交。可以参考它的行为，不能将它当成无需改动的通用 Ogg 标签编辑器。

### 6.4 封面接口缺失

两版 QueryInterface 都只接受 IUnknown、Reader 和 Metadata，没有 `ISoundThumbnail` 分支。

所以：文件里即使包含图片相关注释，旧插件也不会通过宿主封面接口直接提供图片。本次通用元数据测试亦没有进入 Thumbnail 操作分支。

重写时可以同时给 Vorbis 和 Opus 增加封面读取/编辑能力，但这属于新增功能。应将图片条目解析、图像字节生命周期、前封面优先级及宿主限制单独实现，不能只增加一条 QueryInterface 返回值。

## 7. 本次真实 DLL 验证

在主机隔离目录中分别只加载当前版和旧版 DLL。使用本地生成的音频样本，通过已有 `aac_validation.exe` 的通用 PluginManager/Reader 路径验证；未运行新插件，也未在本轮重复执行虚拟机或原版 GUI 测试。

| 样本 / 操作 | 当前版 | 旧版 | 结论 |
| --- | --- | --- | --- |
| 1.25 秒、44.1 kHz、双声道 Vorbis | 通过 | 通过 | 均输出 float64，882,000 字节；0 / 中间 / 末尾附近 seek 通过 |
| Opus 内容，故意也使用 `.ogg` 后缀 | `0x8BDA0007` 拒绝 | `0x80004005` 拒绝 | 不是后缀问题，内部没有 Opus 解码分支 |
| 两段相同 44.1 kHz、不同流序列号的 Vorbis 链 | 通过 | 通过 | 总时长 2,500 ms，1,764,000 字节；定位通过 |
| 44.1 kHz→48 kHz 的 Vorbis 链 | 提前 EOF | 提前 EOF | 报时长 2,500 ms，但只交付 880,640 字节；定位到第二段后读到 S_FALSE |
| 标签写入、关闭后重读、删除自定义字段 | 通过 | 通过 | 包含 Unicode、AlbumArtist、Genre、ReplayGain 等字段 |
| 标签编辑前后音频包检查 | 相同 | 相同 | 均为 56 个压缩音频包，包摘要保持不变 |

1.25 秒正常文件应输出 `44100 × 1.25 × 2 × 8 = 882000` 字节，实测一致。变采样率链的结果与“内部拒绝格式变化、外层吞掉失败”吻合。

测试工具只对 32-bit float 计算 RMS/peak；本插件输出 64-bit float，日志中 RMS/peak 为 0 不能解读为音频无声。本次判断基于格式、样本数量、实际读取与定位状态，不宣称已经做听感验证。

当前版与旧版的 PCM 摘要并不相同，不能声称它们逐采样一致。新版本升级 libVorbis 后同样需要音频容差/参考解码验证，不能把“升级依赖”当作与旧版字节一致。

证据：`tests/ogg_analysis/native-results.json`、两组解码/标签日志、生成样本、当前/旧版伪代码和函数 CSV。测试修改的仅为生成样本副本。

## 8. 推荐的重写扩展结构

```text
ttp_ogg.dll
  ttpGetSoundAddIn
    OggReaderCreator (*.ogg;*.oga;*.opus)
      OggReader：公共宿主接口、IStream、格式、错误、元数据
        VorbisBackend → libvorbisfile + libvorbis
        OpusBackend   → libopusfile + libopus
      libogg：共用 Ogg 基础设施
      VorbisComment / OpusTags：各自解析与保存规则
      Thumbnail：共用图片描述与字节管理
```

建议一个 ReaderCreator 内部完成内容分流，避免宿主枚举顺序决定 `.ogg` 归哪一个 Reader。通过合法 Ogg 页及 BOS 包探测 `01 + vorbis` 或 `OpusHead`；不能仅在文件任意位置搜索字符串，也不能假设只有一个逻辑流。

开文件仍由各后端最终验证。Vorbis 路径调用 Vorbisfile，Opus 路径调用 Opusfile；不需要为了共享容器而另写一套底层音频解码算法。

### 可共用与必须分开的部分

| 模块 | 共用程度 |
| --- | --- |
| x86 ABI、GUID、引用计数、CoTaskMemAlloc | 可以共用 |
| IStream 所有权、可选宿主回调 | 可以共用设计，但两库回调签名需分别适配 |
| Ogg 页、CRC、serial、packet 重组 | 可共用容器设施 |
| 文件识别与宿主错误处理 | 共用外层，区分后端错误 |
| 文本字段、图片字节、宿主接口 | 可以共用数据模型 |
| Vorbis / Opus 解码和 seek | 分别使用对应库 |
| 三个 Vorbis 头 / 两个 Opus 头 | 必须分别处理 |
| granule position、裁剪、header gain | 保持编码专用规则 |
| ReplayGain / Opus R128 | 不能自动视作同一含义的字段 |
| 标签写回 | 共享事务和 I/O；包内容及时间规则分别实现 |

原版已有动态插件枚举能力，新的 Reader 输出普通 PCM 后原则上不需要修改原版 EXE 的解码主链。打开过滤器、拖入、文件关联、属性窗口和转换输入仍需两端宿主实际验收。

重建版的错误后缀路由仍是另一处工作：目前识别后缀提示和选择 Reader 的探测范围不同，不能因为新 DLL 声明三个后缀，就认为误命名成 `.mp3/.flac` 的 Opus 也已恢复。

## 9. 与独立 ttp_opus 的选择

| 方案 | 优点 | 代价 / 边界 |
| --- | --- | --- |
| 独立 `ttp_opus.dll` | 只新增 Opus，现有 Vorbis 功能回归面小 | Ogg 后缀探测需考虑与旧插件共存；旧 Vorbis 插件缺陷和运行库依赖继续保留 |
| 源码重建 `ttp_ogg.dll`，含 Vorbis + Opus | 一套 Ogg 入口，可同时完善封面、错误处理、旧系统依赖和保存 | 必须验收全部原有 Vorbis 功能，包括标签写入和链式文件 |
| 直接补丁修改旧 DLL | 极小的后缀/文本变化容易 | 无法仅靠这些变化加入 Opus 算法、结构和状态，不推荐作为完整扩展方案 |

**如果仅要求尽快增加 Opus，独立插件仍是范围较小的方案；如果接受同步恢复和维护 Vorbis，推荐按本报告重建 ttp_ogg，再接入 Opus。** 这补充了上一份 [Opus 插件可行性分析](OPUS_PLUGIN_FEASIBILITY.md)，并非发现 FLAC 式 PCM Reader 路线不可用。

发行时应明确选择单个扩展后的 ttp_ogg，或旧 ttp_ogg + 独立 ttp_opus；不同时安装多个功能重叠的重写变体并依赖加载顺序。

## 10. 构建、依赖与许可证

可沿用 ttp_aac 的现代 MSVC x86 + VC-LTL + YY-Thunks，目标为 XP/Win7/新系统共用 DLL。**当前旧插件的 MSVCR110 依赖不会因为只改播放器的构建方式而自动消失**；只有重建该插件并验证最终导入才能解决。

解码依赖建议使用固定版本和 SHA-256：libogg 1.3.6、libvorbis 1.3.7（包含 Vorbisfile），以及前文核对的 libopus 1.6.1、opusfile 0.12。源码可在构建时下载，保留版权、许可证、来源和版本记录。libogg / libvorbis 当前发布版本见 [Xiph 下载页](https://xiph.org/downloads/)。

一个需单独核对的许可证差别：libvorbis 使用 BSD 类条款，而官方 `vcedit.c` 文件头明确标注 GNU Library General Public License version 2。若采用该编辑器源码，不能一并按解码库的 BSD 条款处理；应遵守其实际许可证。若按格式规范重新编写保存模块，也要清楚记录实际采用的代码来源，不能仅通过改名就视为自行编写。[libvorbis COPYING](https://raw.githubusercontent.com/xiph/vorbis/v1.3.7/COPYING)、[官方 vcedit.c](https://raw.githubusercontent.com/xiph/vorbis-tools/master/vorbiscomment/vcedit.c)

不建议把反编译出的旧版 libVorbis 算法和编译器运行库逐行搬入工程；采用上游源码，重建宿主适配层更便于升级与审计。新 DLL 的体积、XP 静态导入和 CPU 指令要求均需实际构建后测量，不能从旧 DLL 大小直接预测。

## 11. 实施顺序

1. **先独立重建 Vorbis Reader**：枚举、打开、PCM、seek、EOF、Unicode 标签读取，建立当前和旧版行为对照。
2. **恢复已有编辑能力**：保存/删除标签、未知字段和音频包保留；安全写回、错误传播。完成这一阶段前不将只读版本作为原插件的完整替代品。
3. **接入 Opus 后端**：实际内容分流，时长、裁剪、定位、声道和 header gain；分别报告尚未实现的编辑能力。
4. **补充两种编码的封面与标签编辑**：Thumbnail、多图片策略、OpusTags 与 R128/ReplayGain 集成。
5. **兼容与发行验收**：原版/重建版在 XP、Win7、Win10/11；真实模块路径、PCM、属性保存、转换输入、并行元数据加载和扫描增益。

每一步保留独立后端，避免一个编码的错误路径破坏另一个。链间格式改变的处理可以选择稳定输出格式转换或明确报错，绝不能重现“显示完整时长却无提示提前结束”。
