# ASIO／Kernel Streaming 原版对照修复（2026-10-19）

## 1. 结论与验证边界

本次修复的是已从 5.7.9 原版伪代码和当前源码确认的输出条件、ASIO 双缓冲时序、KS 枚举与格式兼容问题。设备能列出，不代表驱动已经成功打开，也不能单凭反馈断言所有声卡失败都属于同一个原因。

已在本机 Windows 11、XP（5.1.2600）和 Win7（6.1.7601）运行隔离的原生输出回归程序；三套系统全部通过。测试注入模拟 IASIO 和 KS transport，没有安装、注册、加载实体 ASIO 驱动，没有打开实体 KS 输出，也未更改系统音频设置。因此这些结果证明本次合约、转换和生命周期修复可在三个系统执行，不代表用户声卡已发声验证通过。

本报告补充 [四类用户反馈原版分析](ORIGINAL_FEEDBACK_REVIEW_20261019.md) 第 7 节；该旧报告的行号记录的是修复前状态。以下路径对应本次修复后的文件。

## 2. 原版证据

目标为根目录 `TTPlayer.exe`，x86 PE，1,882,544 字节，SHA-256：

`c7999ea5823469c1684148cac1bd196009824348ccac5cdd45978f8fb574e28c`

伪代码基准：`reverse/decompiled/TTPlayer.exe.pseudo.c`。表中的地址是原版虚拟地址。

| 原版地址 | 原版行为 | 本次对应工作 |
|---|---|---|
| `004918B1`、`004918FE` | 输出类型和序号使用同一 16 字节 key | 保留旧 key 读取；新选择追加稳定身份形式 |
| `004E1C87` | ASIO 注册项发现、显示名和 CLSID | 保留 x86 驱动视图及延迟能力探测 |
| `004E207B` | 普通 ASIO 用 CLSID 同时作类和接口；特定 KS-ASIO shim 有 DLL 工厂分支及 COM 回退 | 恢复完整激活尝试顺序 |
| `004E14BD` | ASIO 样本布局／转换表 | 保留格式映射，修正整数 unity 转换的 1 LSB 偏差 |
| `004E1610` | 实际声道数为 `max(2, min(源声道, 输出声道))`，实际打开以 setSampleRate 结果为准 | 不再把设备总输出数大于 64 或 canSampleRate 否定当成必然失败 |
| `004E19E5` | 启动前填第二块缓冲 | 先填 B，再 start，首次回调填 A |
| `004E1995`、`004E1AF2` | 累计已结束周期，返回已播放位置 | 驱动缓冲被归还后才计入完成字节；尾周期完成前不报告耗尽 |
| `004E194F`（二进制核对） | ASIO 消息回调返回引擎版本 2 等能力 | 补协商，只宣称实现的能力 |
| `004E029E`、`004F066E` | KS 原位宽／扩展格式尝试，失败后尝试 16 位 | 保留同一过滤器内尝试，补可诊断的阶段、格式和 pin |
| `004F01E4`、`004EFEBC` | 支持 GUID_NULL 格式通配 | 枚举接受音频／PCM／WaveFormatEx 对应通配范围 |
| `004C2FD5`、`004C3000` | 1／2／4／6／8 声道掩码表和扩展格式构造 | 恢复 `0x4/0x3/0x33/0x3f/0xff` |

`004E207B` 的特殊分支不能简单概括成“全部驱动都从 DLL 导出激活”：只有名称为 `ASIO Kernel-Streaming driver` 的兼容分支采用该路线；普通驱动仍按原版 COM 合约打开。IClassFactory 查询失败时尝试直接 DllGetClassObject(CLSID, CLSID)，工厂实例化失败直接进入 COM 回退。

## 3. 已实现的 ASIO 修复

源码：[asio_sink.cpp](../src/audio/asio_sink.cpp)、[asio_sink.h](../include/ttplayer/audio/asio_sink.h)。

### 3.1 激活、采样率和错误诊断

- 将特殊 DLL 工厂、直接导出、标准 COM 回退抽为同一可测试流程。失败记录保留实际阶段和 HRESULT。
- 设备总输出数允许合理的上限 4096，实际分配仍仅使用源需要的声道，源格式限制和队列容量保护继续生效。128 输出设备播放双声道只申请两路缓冲。
- canSampleRate 否定记入诊断，继续调用 setSampleRate；setSampleRate 真正失败才拒绝该采样率。
- init、getChannels、getBufferSize、setSampleRate、getChannelInfo、createBuffers、start／stop 的失败都有对应阶段。支持原生驱动返回码，输出样本类型异常包含通道编号。
- createBuffers 部分失败或返回空指针都先 dispose，再释放驱动。
- start 失败后先停止可能已部分启动的驱动，再恢复尚未完成的 PCM；保留原始 start 错误。

这里没有新增静默切换到 WaveOut／DirectSound 的行为。驱动缺失、被占用、固定采样率或外部时钟问题应通过具体错误定位。

### 3.2 双缓冲所有权和播放位置

修复前将“刚复制到驱动缓冲”的字节直接算作已播放，播放器可能提前报告进度并关闭最后一段音频。现在分别保留软件队列、两块驱动缓冲中的有效源字节以及已完成字节。

| 时点 | 缓冲 B | 缓冲 A | 完成计数 |
|---|---|---|---|
| start 前 | 填入第一周期 | 清零 | 0 |
| 首次回调申请 A | 第一周期尚未完成 | 填入第二周期 | 0 |
| 驱动归还 B | 第一周期已播放，可填第三周期 | 第二周期待播放 | 增加第一周期有效字节 |
| 最后有效周期归还 | 可填静音 | 可填静音 | 此时才达到全部源字节 |

不足一周期的尾部会补静音，但静音不计为源音频。1、4、5、12、13 帧样本覆盖短于周期、恰好周期和不整周期的边界。

暂停在 Stop 完成后，将尚未归还的 PCM 按原先的提交顺序放回软件队列；恢复播放重新预填。定位重置会丢弃旧队列和两块驱动缓冲，并重置完成计数。Start／Stop 不能持 PCM 锁，Dispose 不能持全局回调锁，因为驱动允许同步回调。

**精度边界：** 当前完成计数按驱动周期更新；暂停发生在周期内部时，尚未确认完成的周期可能重播部分样本，误差受一个周期约束。没有实现基于 getSamplePosition 的周期内精确暂停。模拟测试验证周期边界的无丢失／重复，实体驱动的首回调顺序、真实延迟和周期内暂停仍须硬件验证。

### 3.3 协商与输出数据

- ASIO 引擎版本返回 2，支持 time-info 回调。
- outputReady 在创建缓冲后探测一次；不支持时不在每次实时回调中反复调用。
- reset 请求或采样时钟改变通过原子标志和事件转交播放工作线程；不从驱动实时线程重入 COM 或销毁缓冲。
- 工作线程在 SetPaused 的状态检查中收到明确错误并关闭当前输出。**本次没有实现通知后自动重开／无缝恢复；用户重新播放时重开设备。** 不将确认 reset 通知描述成自动恢复成功。
- 不宣称尚未实现的 timecode、resync、latency-change 能力。
- 整数 PCM 转换按 `2^(位数-1)` 缩放、正上界钳制，修正 16／24／32 位 unity 处理对正负一 LSB 的损失；保留大端、低有效位存储和单声道复制到首对声道。

## 4. 已实现的 KS 修复

源码：[output_devices.cpp](../src/ui/output_devices.cpp)、[native_output_contract.cpp](../src/audio/native_output_contract.cpp)、[kernel_streaming_sink.cpp](../src/audio/kernel_streaming_sink.cpp)。

1. 期限为 0 表示使用默认单次查询等待，不再在 pin 循环第一步就判定超时。显式全局期限仍被检查。该缺陷直接影响详情查询，不能单独解释所有实际播放失败。
2. 支持音频／PCM／WaveFormatEx GUID_NULL 通配。只有通用 KSDATARANGE、没有音频扩展上下限的有效范围也可列入，实际具体格式由 KsCreatePin 决定。
3. topology／硬件音量查询失败只显示未知，不再否决已经存在标准 PCM render pin 的设备；软件音量仍可使用。这是对能力缺失的兼容改进，不声称原版每一种 topology 错误都采取相同行为。
4. WAVEFORMATEXTENSIBLE 恢复原版 1／2／4／6／8 声道掩码表。其余声道数不凭空指定声道位置。当前 sink 接口仍只接收 WAVEFORMATEX，任意源文件自定义 speaker mask 的贯穿传递另属后续工作。
5. 没有可用 pin 和最终 KsCreatePin 失败均记录明确阶段；后者附上请求采样率、位宽、声道数、最后尝试的 pin 和选定过滤器路径。
6. 现有的数据包所有权、异步完成、取消、复位和 16 位回退保持原有职责；不因设备失败转向另一块设备。

仍未实现全部 WaveRT 路线、任意拓扑硬件音量以及原版偏好 pin 的完整排序策略。现有策略在选定过滤器的标准 render pin 内按格式组合尝试，最终以实际打开结果判断。

## 5. 设备稳定身份与旧配置兼容

原版旧 key 把后端和枚举序号存入 GUID。枚举前方设备超时、驱动增删后，相同序号可能变成另一块设备。本次保持工作进程目录快照中的旧 key，新增设置用的稳定形式：

| 后端 | 新 DeviceType 值 | 解析规则 |
|---|---|---|
| ASIO | `native:asio:{CLSID}` | 匹配驱动 CLSID |
| KS | `native:ks:<设备接口路径>` | 匹配接口路径，不按排序行号 |
| WaveOut／DirectSound／WASAPI | 既有格式 | 保持既有路由 |

`StableOutputDeviceKey` 生成稳定值，`OutputDeviceMatchesKey` 同时接受新值和旧 GUID，`ResolveLegacyNativeOutputDevice` 拒绝缺失、歧义或无效描述符。目录序号不再影响已经保存为稳定 key 的选择。

设置页已在列表匹配、当前选中恢复、保存及即时设备切换中接入这些 helper，DeviceBackendFromKey 使用共同解析。缺失设备以惰性占位保留：UI 允许无物理描述符的占位按完整 key 匹配，播放层仍要求有效 CLSID／接口路径，不能把 UI 占位当成可播放设备。播放引擎继续使用 ParseOutputDeviceKey 路由，ASIO 非阻断诊断写入现有诊断记录。这些调用方属于同一第 4 类集成提交。

旧 GUID 配置仍可按原序号读取，成功解析后在保存选择时迁移。若用户在第一次迁移前已改变设备顺序，旧配置本身没有足够信息恢复历史物理设备，不能声称可以无损推断。KS 接口在驱动重装后若改变，也应显示原设备不可用，要求重新选择。

## 6. 私有测试与结果

测试仅存于 `rebuild/tests`，不提交到播放器仓库，不加入 Action，不加入发行包。

隔离构建目录为 `rebuild/tests/native_output_20261019`。项目从已配置的生产构建提取 x86 MSVC、VC-LTL 和 YY-Thunks 编译／链接配置，直接编译本次修改的源码，输出不覆盖主 build 目录。本机测试设置 45 秒进程超时；虚拟机通过 MCP guest 执行并正常返回，未出现崩溃或超时。

| 测试程序 | Windows 11 | XP 5.1.2600 | Win7 6.1.7601 |
|---|---|---|---|
| `asio_sink_tests.exe` | 353 检查通过 | 353 检查通过 | 353 检查通过 |
| `kernel_streaming_sink_tests.exe` | packet lifecycle 通过 | 通过 | 通过 |
| `native_output_contract_tests.exe` | key/catalog/ASIO contract 通过 | 通过 | 通过 |
| `options_native_keys_tests.exe` | 55 检查通过 | 55 检查通过 | 55 检查通过 |

虚拟机运行目录：`C:\TTPlayerNative20261019`。ASIO 测试采用“先记录已播放缓冲内容，再归还给主机”的模拟设备时钟，既检查计数，也核对真实输出样本顺序，避免测试只重复错误实现。覆盖：

- 特殊驱动工厂／直接导出／COM 的成功和失败组合；普通驱动仅 COM。
- 128 个物理输出、实际两路播放；canSampleRate=false 而 setSampleRate=true。
- 首周期 B 预填、首次回调不提前计数、短尾、准确 EOF、静音不计数。
- 暂停／恢复、定位、同步 Stop 回调、失败 Start 清理与重试。
- 16／24／32 位满幅和正负一 LSB、大端、低有效位整数、单声道复制。
- outputReady 一次探测、版本／time-info 协商、reset／采样率改变通知。
- 空缓冲和部分分配失败的释放次序、第二个回调拥有者拒绝。
- KS 数据包的完成／取消／重置／失败、整数转换和 16 位回退。
- deadline=0／未来／过期；精确和通配格式；非音频格式拒绝；原版 mask 表。
- 前方设备移除／序号移动时 ASIO CLSID 和 KS 路径仍解析到原设备；缺失设备不改指其他设备；旧 key 保留；无效 key 拒绝。

设置页专项测试直接抽取当时生产代码中的 `DeviceOptionEntry`、身份／后端辅助函数、占位插入、排序和 ComboBoxEx 选择段落，使用隐藏的真实 ComboBoxEx 控件，不加载播放器和实体驱动。涵盖 KS／ASIO 两类旧 GUID 保存为稳定 key、重开设置页、序号改变、完整和失败枚举下设备拔除、保存占位、重新接入以及旧 GUID 在枚举失败时保持原值。仅在私有测试副本恢复修复前 matcher 时，测试准确失败于“拔除后跳至其他设备”；重新抽取修复后的生产段落，三系统均 55 检查通过。该方法验证实际选择段落和控件 item data，未将整个选项窗口的视觉检查冒充为已完成。

`QueryKsAudioCapabilities` 对缺少 topology 的具体系统调用分支目前由源码核查覆盖，尚未通过伪造完整 KS 过滤器进行端到端注入。因此不能把纯函数和 transport 测试当成实际内核驱动探测已经全部验证。

## 7. 仍需真实硬件验证

使用同一块声卡、同一版 x86 驱动和相同文件，对原版 5.7.9 与重建版对照：

- 44.1／48／96 kHz，16／24／32 位，单／双／4／6／8 声道；驱动真实支持的范围外应得到明确错误。
- 首音、尾音、暂停周期内部、定位、循环／换曲；关注首回调顺序及外部时钟。
- 设备被另一进程占用、拔出／重连、驱动 reset、采样率改变、可选 outputReady 不实现。
- KS 普通格式／扩展格式／16 位回退，通配 data range，缺 topology，实际声道位置。
- 不同 COM 初始化／驱动线程要求。当前播放工作线程与独立详情探测线程的 apartment 不完全相同，特定驱动仍可能要求另行适配。

播放器仍为 x86。64 位系统中只有 x64 ASIO DLL 时，不能通过读另一注册表视图让 x86 播放器直接加载该 DLL。此次测试没有把 VirtualBox 虚拟声卡当作实体专业 ASIO／KS 硬件发声验证。

## 8. 官方参考

- [Steinberg 开发者与 ASIO SDK 入口](https://www.steinberg.net/developers/)：接口与驱动集成背景；本次旧版调用次序以原版二进制证据为准。
- [Microsoft：WAVEFORMATEXTENSIBLE](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ksmedia/ns-ksmedia-waveformatextensible)：有效位、声道掩码及扩展格式。
- [Microsoft：音频数据格式与范围](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/audio-data-formats-and-data-ranges)：格式范围不等于某个具体格式已成功打开。
- [Microsoft：KSDATAFORMAT](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ks/ns-ks-ksdataformat)：MajorFormat／SubFormat／Specifier 的约束。
- [Microsoft：注册表重定向](https://learn.microsoft.com/en-us/windows/win32/winprog64/registry-redirector)、[运行 32 位应用](https://learn.microsoft.com/en-us/windows/win32/winprog64/running-32-bit-applications)：注册表视图与进程／DLL 位数边界。
