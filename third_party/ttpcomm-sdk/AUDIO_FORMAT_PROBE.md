# Optional audio-format ABI v1

2026-10-20。声明：`include/ttpcomm/audio_format_probe.h`。可选辅助实现：`audio_format_probe_impl.h`。

## 用途和兼容性

协议由 Reader 工厂与 Reader 经 QueryInterface 提供，不增加旧接口槽位，不增加 ttpcomm.dll 导出，不注册 COM，不要求插件链接 ttpcomm。原版播放器继续使用旧接口；重建播放器识别新 IID 后才使用新方法。原版 EXE 不会因为换了 DLL 而自动增加“错误后缀识别”调用。

ABI 面向进程内 Windows x86、`__stdcall`、IUnknown 引用计数。不得跨边界传 STL、异常、分配器所有权或宿主可释放的不明指针。

| 接口 | IID | IUnknown 后的槽 3 |
|---|---|---|
| `ttp::format::Probe` | `7E892943-481A-43DB-A89B-5993F50B6049` | `HRESULT ProbeFormat(const Request*, Result*)` |
| `ttp::format::SourceInfo` | `B58EE1A2-812C-4920-920E-5BE9379F3132` | `HRESULT GetFormatInfo(Result*)` |

所有 QI 成功分支 AddRef。新增接口 QI 到 IUnknown 必须返回与旧接口相同的对象身份。接口指针不能直接当旧 Reader 指针使用；多重继承会调整地址。

## Request

`#pragma pack(8)`；x86 大小 32 字节。调用方构造默认值再填写输入；协议版本为 1。

| 字段 | 含义 |
|---|---|
| `DWORD size, version` | `sizeof(Request)` 和 1 |
| `const BYTE* data` | 调用期间有效的只读头部，不可保存此指针 |
| `DWORD bytes` | 头部实际长度，不能超过 65536 |
| `uint64_t file_size` | 原文件长度；不知道时为 UINT64_MAX |
| `uint64_t data_offset` | data 对应原文件偏移，通常为 0；跳过合法 ID3 后可能非零 |

Probe 只能检查提供的字节；不执行 I/O、不弹窗、不改文件、不修改输入。识别新格式时不要根据 `data_offset` 假定前缀一定是音频；仍检查边界和结构。

## Result

`#pragma pack(8)`；大小 584 字节。无指针，输出复制到调用者提供的结构。ASCII 的 container/codec、UTF-16 的扩展名/说明都必须以 NUL 结尾。

| 字段 | 含义 |
|---|---|
| `size, version` | 调用方初始化；不支持版本/大小返回 E_INVALIDARG |
| `family` | 共用实现的分类位；宿主不依赖它判断未知插件是否可用 |
| `confidence` | 见下表 |
| `flags` | HasLeadingTag：跳过前导标签；ContainerOnly：编码尚不明确 |
| `required_bytes` | NeedMore 时希望取得的前缀长度；不是必须执行的 I/O 指令 |
| `container[24], codec[24]` | 稳定的格式名称，如 `mp4` / `aac`，不是 UI 显示字符串 |
| `extension[16]` | 建议外层后缀，含点；v1 为小写 ASCII 字母、数字或 `+` |
| `aliases[160]` | `;` 分隔的合法后缀，含点；比较忽略大小写 |
| `display[80]` | 提示中的格式说明 |

| confidence | 用途 |
|---|---|
| 0 NoMatch | 无匹配，Probe 返回 S_FALSE |
| 1 NeedMore | 证据不足，不按匹配候选处理 |
| 2 Tentative | 可尝试 Open 的弱候选 |
| 3 Strong | 结构匹配，优先尝试 Open；不代表整个文件完整或可播放 |
| 4 Opened | 仅 SourceInfo 在 Reader 成功 Open 后报告 |

未知新格式可以自定义 Probe/SourceInfo；不需要宿主认识 family 的位定义。宿主必须验证版本、字符串终止和建议后缀，不能把 `display` 当作路径使用。

## 宿主流程

1. 保留真实逻辑路径和 IStream。读取一次可恢复位置的有界前缀。
2. 查询所有 Reader 工厂的新 IID；不要先用扩展名排除工厂。
3. 有效 Strong 优先于 Tentative，再按旧扩展名路径；同级保持原枚举顺序。
4. 新 IID 不存在、没有确定匹配时继续旧行为；Probe 不代替真正 Open。
5. 每次尝试从位置 0 开始。取消、访问、传输、内存等终止性错误不能被后续格式尝试吞掉。
6. Open、格式和输出缓冲通过检查后，查询 SourceInfo。只能使用 `S_OK + Valid + Opened` 的结果建议更名。
7. 更名由宿主负责：播放后提示、总开关、合法别名、冲突处理、文件版本校验、用户确认。

容器格式优先于内部编码：MP4/AAC 不改成 `.aac`，WebM/Opus 不改成 `.opus`，压缩包仍交给外层包格式逻辑。Original Reader 的 PCM Format、CodecName 和 Decoder Supports 均不能替代 SourceInfo。

## 共用实现的范围

`ReadSnapshot` 先查询位置及有限长度，通常读取 8192 字节；完整 Ogg 标识页需要更多时最多读取 65536 字节。跳过合法 ID3 前导的偏移上限为 1 MiB。累积短读，成功、错误、取消后都恢复原位置；恢复失败返回失败。未知长度和不可 seek 流返回 S_FALSE，保持原流式路径。

工厂可继承 ProbeSupport 并实现 ProbeFamilies。Reader 可继承 InfoSupport，Open 前调用 PrepareFormatInfo；真正 Open 的结果交给 CompleteFormatInfo。只有 S_OK 才转为 Opened。容器解析得到具体编码后用 SetSourceCodec 补全；未知编码可以保持 ContainerOnly，不猜测。

InfoSupport 是保守辅助实现。超出其前缀预算、无可靠签名或新格式不在共用表内时，SourceInfo 可能返回 S_FALSE，即使原扩展名路径仍能正常播放。自行实现 SourceInfo 可使用真正解析器已验证的格式结果，不必再扫描文件。

## 同步与测试

规范头文件在本 SDK；独立仓库携带同内容副本。`tools/sync_audio_format_sdk.py` 同步；`--check` 只检查。当前覆盖 AAC、OGG、FLAC、APE、WebM、AC3/DTS、MOD、MPC、ASF、RM、TAK、VQF 与播放器。

播放器 `tests/audio/audio_format_probe_tests.cpp` 覆盖版本、输入限制、QI 身份、原工厂 Create 槽、短读、ID3、位置恢复、取消、不可 seek、真实文件选择和更名建议。完整新插件工程说明见相邻播放器仓库的 `docs/AUDIO_PLUGIN_DEVELOPMENT_GUIDE.md`。
