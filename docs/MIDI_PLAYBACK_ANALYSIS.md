# 原版 MIDI 播放原理与重建版差异

分析日期：2026-09-19。对象为 TTPlayer 5.7.9.0；原 EXE 的 SHA-256：
`c7999ea5823469c1684148cac1bd196009824348ccac5cdd45978f8fb574e28c`。

## 结论

原版将 `.mid`、`.midi`、`.rmi` 分流到独立的 DirectShow 播放对象。
它创建 Filter Graph Manager，调用 `IGraphBuilder::RenderFile`，通过 `IMediaControl`、
`IMediaSeeking`、`IBasicAudio` 控制播放、定位及音量。没有经普通解码器将 MIDI 转成 PCM 再送入
千千静听自身音频处理／输出链，也不是直接调用 MCI。

内置的 MIDI Reader 主要承担格式、时长查询。其固定返回的 44.1 kHz／16 位／双声道格式是
播放器接口的占位信息，不能作为系统 MIDI 合成器实际输出参数的证据。

分析之后已按本页证据恢复重建版的 MIDI 播放与信息查询路径，实施和验证见第 8 节。
函数名为恢复后的语义说明，不是原始符号。

## 证据和分析边界

- 主依据：`reverse/decompiled/TTPlayer.exe.pseudo.c`。
- Ghidra 未导出为独立函数的短入口，通过原 EXE 的虚函数表及 x86 反汇编核对。
- COM 标识符从 EXE 原始数据读取，再与 Windows SDK 的 `strmif.h`、`control.h` 对照。
- 系统 MIDI Parser／Renderer 的内部原理引用微软文档；并未反编译 quartz.dll。
- 未在原版运行进程中枚举实际过滤器图，因此机器最终选择的 MIDI 设备、第三方过滤器、音色库
  和真实采样率均不能仅凭本次静态分析确定。
- 邻近的 `004E9DF6` 是另一套有 RIFF/WAVE 数据读写路径的对象，不能因位置接近 MIDI
  的 `MID|MIDI Music` 字符串，就把它当作 MIDI 的 PCM 合成器。

## 1. 识别与分流

`004AA982` 从路径最后一个点取扩展名，以 `_wcsicmp` 判断 `.mid`、`.rmi`、`.midi`。
资源字符串 `0x811F` 列出的过滤条件也是 `*.mid;*.midi;*.rmi`。

`004AB1C4` 是重要分流点：

| 路径 | 创建对象与后续行为 |
| --- | --- |
| 非 MIDI | 创建普通音频读取对象，协商 PCM 参数与缓冲，再选择 WaveOut／DirectSound／KS／ASIO 输出 |
| MIDI | 分配 `0x58` 字节对象，调用 `004E28C9` 构造、`004E29B2` 打开路径，直接返回该播放对象 |

所以在正常 MIDI 分支上，千千静听的普通 PCM 解码、DSP 与输出设备选择分支被绕过。
这里说明的是千千静听内部的数据路径；系统合成器和系统音频服务内部仍会产生音频波形。

## 2. 创建 DirectShow 播放图

`004E2A40` 的关键调用可恢复为：

```cpp
CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                 IID_IGraphBuilder, &graph);
graph->RenderFile(path, nullptr);
graph->QueryInterface(IID_IMediaControl, &control);
graph->QueryInterface(IID_IMediaSeeking, &seeking);
graph->QueryInterface(IID_IBasicAudio, &audio);
```

对应 EXE 常量：

| 常量地址 | GUID | 已确认含义 |
| --- | --- | --- |
| `00516FC8` | `E436EBB3-524F-11CE-9F53-0020AF0BA770` | CLSID_FilterGraph |
| `00524DD0` | `56A868A9-0AD4-11CE-B03A-0020AF0BA770` | IID_IGraphBuilder |
| `00524DB0` | `56A868B1-0AD4-11CE-B03A-0020AF0BA770` | IID_IMediaControl |
| `00524DC0` | `36B73880-C2C8-11CF-8B46-00805F6CEF60` | IID_IMediaSeeking |
| `00524DA0` | `56A868B3-0AD4-11CE-B03A-0020AF0BA770` | IID_IBasicAudio |

`IGraphBuilder` 虚表 `+0x34` 正是 `RenderFile`。对象字段 `+0x44/+0x48/+0x4C/+0x50`
分别保存播放图、控制、定位和音量接口；`IMediaControl` 缺失会返回 `E_NOINTERFACE`。

## 3. MIDI 怎样变成声音

MIDI 保存的是音符、乐器选择、力度、控制器及节奏等事件，文件本身不等同于 MP3／FLAC 中的音频波形。

根据微软对默认 DirectShow MIDI 组件的说明，典型链路为：

```text
MID / MIDI / RMI 文件
  → 文件源过滤器
  → MIDI Parser（读取 MIDI 并生成事件样本）
  → MIDI Renderer（按时序交给 WinMM MIDI stream）
  → 系统选择的 MIDI 输出设备／合成器
  → 声音输出
```

Parser 支持 MID 和 RMI，接受文件源输出的流；Renderer 读取 MThd 的时间划分参数，通过
`midiStreamProperty` 设置流属性，以 `midiStreamOut` 提交事件。微软文档说明默认 Parser
的样本约包含一秒 MIDI 数据，并在每个缓冲开头携带恢复渲染状态所需的命令。

这是系统默认组件的行为。原版调用 `RenderFile` 自动组图，没有在该函数中固定所有过滤器实例。
安装了其他过滤器或 MIDI 驱动后，实际图和音色可能变化。未发现这条原版路径直接加载 SF2／DLS
音色库的调用，不能据此断言它固定使用某个音色库或某个软件合成器。

## 4. 播放、暂停、停止与定位

| 操作 | 原入口 | 实际接口／行为 |
| --- | --- | --- |
| 播放／继续 | `004E2B9C` | `GetState(100, ...)`，需要时 `IMediaControl::Run()`，本地状态置 2 |
| 暂停 | `004E2BDE` | 需要时 `IMediaControl::Pause()`，本地状态置 1 |
| 停止 | `004E2C20` | `IMediaControl::Stop()`，随后定位到开头，本地状态置 0 |
| 时长 | `004E2A40` | `IMediaSeeking::GetDuration()`，除以 10000 转为毫秒 |
| 当前时间 | `004E2D6E` | `IMediaSeeking::GetCurrentPosition()`，同样换算毫秒 |
| 跳转 | `004E2DAF` | 毫秒乘 10000，检查 `GetAvailable()` 范围，再绝对定位 |

跳转使用 `SetPositions(&target, 1, nullptr, 0)`；1 为 `AM_SEEKING_AbsolutePositioning`。
原版没有在这条跳转函数里扫描 MIDI 音符或自行补发控制器事件，交由系统过滤器和设备完成。

`004E29B2` 安装间隔 `0x32`，即 50 ms 的线程定时器。未独立导出的 `004E2EA2` 回调比较当前
位置与总时长；到末尾且本地状态非停止时，通过 `004AAC0F`／`PostMessageW` 通知上层。
它承担结束检测，不是逐个音符的调度时钟。自然结束后的下一曲由播放器上层继续处理。

## 5. 音量和左右平衡

原版确实有 MIDI 音量控制。`004E2C72` 的主体被伪代码导出遗漏，反汇编可确认它调用
`IBasicAudio::put_Volume`（虚表 `+0x1C`）。对 0～100 的正常 UI 输入，大致公式为：

```text
volume > 0: 2000 × log10(volume / 100)
volume <= 0: -10000
```

结果转换为整数后提交，单位是百分之一 dB。100 对应 0，50 约对应 -602，0 对应 -10000。
因此不能看到主 EXE 没有 `midiOutSetVolume` 调用就断言 MIDI 不支持音量。

`004E2CCE` 调用 `IBasicAudio::put_Balance`（虚表 `+0x24`）。0 居中，正值衰减左声道，
负值衰减右声道；按剩余幅度的对数转换为 -10000～10000，而不是简单乘 100。
实际控制是否成功仍取决于播放图提供的接口及设备返回值。

## 6. 内置 MIDI Reader 的真正职责

注册路径：`004CA48E → 004C907B`；Creator 名称入口 `004C90D0` 返回 `MIDI Reader`，
`004C902B` 创建仅 `0x0C` 字节的 reader：虚表、引用计数、缓存时长，初始时长为 -2。

reader 主虚表位于 `005230E8`：

| 偏移 | 入口 | 行为 |
| --- | --- | --- |
| `+0x0C` | `0040BE91` | 流式 Open 返回 E_NOTIMPL |
| `+0x10` | `004E9C6E` | 返回能力数值 `0x0A` |
| `+0x14` | `004E9C88` | 返回缓存时长 |
| `+0x18` | `004E9CA5` | 返回固定 PCM 格式描述 |
| `+0x1C` | `004E9CEC` | 返回固定缓冲建议 1024 |
| `+0x20` | `004E9D06` | 返回 `MID|MIDI Music` |
| `+0x38` | `00407310` | Read 返回 E_NOTIMPL |
| `+0x3C` | `00407310` | reader Seek 返回 E_NOTIMPL |
| `+0x40` | `004E9BF5` | 路径打开／信息读取；需要时临时创建 DirectShow 对象取得时长 |

`004E9BF5` 遇到解码标志 bit 0 会返回 E_NOTIMPL。固定格式为 PCM、44100 Hz、2 声道、16 位、
块对齐 4、每秒 176400 字节。播放对象初始化也填写同样描述。两处常量均未证明实际合成器输出
采用该格式；reader 的 Read 没有提供音频样本。

因此这条原版路径不能直接向播放器的 EQ、DSP、ReplayGain 分析器或编码器提供 MIDI 合成后的 PCM。
不能把显示了采样率理解为支持普通音频文件那样的解码／转换能力。

## 7. 修复前重建版对比

修复前，`src/audio/audio_engine.cpp` 按扩展名进入 `MciWorker`，并绕过普通 PCM worker，
实际使用 `mciSendCommandW`。以下表格保留修复前的问题记录。

| 项目 | 原版 | 修复前重建版 |
| --- | --- | --- |
| 播放后端 | DirectShow Filter Graph | MCI |
| 扩展名 | MID／MIDI／RMI | 相同 |
| 主路径 | 系统渲染；绕过播放器 PCM 链 | 系统播放；同样绕过 PCM 链 |
| 暂停／继续 | IMediaControl | MCI_PAUSE／MCI_RESUME，失败时从当前位置重播 |
| 定位 | IMediaSeeking 绝对定位 | MCI_STOP 后 MCI_PLAY + MCI_FROM；暂停态再暂停 |
| 时长／进度 | 100 ns 时间单位，换算为毫秒 | 显式设置 MCI_FORMAT_MILLISECONDS，再 MCI_STATUS |
| 结束检测 | 50 ms 定时器比较位置和时长 | 20 ms worker 轮询 MCI_STATUS_MODE |
| 音量／声道平衡 | IBasicAudio，使用对数转换 | ApplyVolumeLocked 仅处理 WaveOut 和 DirectSound，没有 MCI 控制分支 |

结论是：重建版提供了 MIDI 播放替代路径，但并未完整恢复原版实现。尤其音量、静音所依赖的音量
应用以及左右平衡，目前没有从播放器控制接到 MCI 播放设备；系统音量仍可能影响它。

## 8. 已实施的恢复（2026-09-19）

### 实现对应

| 原版依据 | 重建版实现 |
| --- | --- |
| `004AA982`／`004AB1C4` | `.mid/.midi/.rmi` 不区分大小写，分流到 `AudioEngine::MidiWorker` |
| `004E2A40` | 新增 `MidiPlayer`，创建 Filter Graph，RenderFile，然后查询控制、定位和音量接口 |
| `004E2B9C`／`004E2BDE` | GetState(100) 后按需 Run／Pause；暂停定位不再先 Stop、Play 再暂停 |
| `004E2C20`／`004E2DAF` | 停止后回到零；以 100 ns 单位检查可定位范围，再绝对定位 |
| `004E2C72`／`004E2CCE` | 音量、静音值、左右平衡接入 IBasicAudio，保留原版对数换算与截断 |
| `004E2EA2` | 50 ms 检查位置与时长，通过既有引擎状态通知上层结束；定位到末尾也能结束 |
| `004E9BF5`／`004E9CA5`／`004E9D06` | 文件信息与播放列表信息使用临时图读取时长，返回能力 0x0A、固定格式和 `MID\|MIDI Music` |
| MIDI Reader 的 E_NOTIMPL 槽 | 解码／转换入口明确拒绝 MIDI PCM 读取，不再尝试 Media Foundation 普通音频解码 |

源文件：

- `include/ttplayer/audio/midi_player.h`、`src/audio/midi_player.cpp`。
- `src/audio/audio_engine.cpp`、`src/audio/builtin_file_info.cpp`。
- `src/app/file_info_worker.cpp`。

MIDI 不使用普通音频的输出设备配置、PCM DSP、EQ 或 PCM 淡入淡出。信息查询不会运行播放图，
也不把固定格式描述当作真实合成器参数。损坏 MIDI 的查询错误直接返回，避免 Shell 回退把它误报成功。

### 适配现有线程与错误处理

重建版已有独立播放线程，因此创建、控制、定位及释放 COM 对象均在其所属线程完成。
UI 只提交播放／暂停、定位、音量意图并唤醒线程；没有跨 COM apartment 裸传接口。
继续保留打开超时、取消、受控退出及不能重叠开启旧 worker 的保护。

保留原版以位置判断结束的主路径，同时排空并释放 DirectShow 事件参数，接收运行时错误；
未知时长才用 EC_COMPLETE 补充结束判定。新的定位请求优先于旧位置的结束判定，避免误切下一曲。
打开／播放／定位／运行时错误保留 HRESULT 并进入既有播放失败提示；音量接口失败记录到
`LastDiagnostic()` 与调试输出，不把仍能播放的文件误标为解码失败。

这些是对现有线程结构的适配，不是声称已逐字节复制原版对象布局或线程定时器实现。

### 本地验证与系统限制

本地测试代码只保留在 `rebuild/tests/audio/midi_playback_tests.cpp`，不上传，不加入发行包；
Actions 仍设置 `BUILD_TESTING=OFF`，不会编译或执行这些测试。

测试使用自行生成的 MIDI 0、MIDI 1、RIFF RMID 文件，包含节奏变化；验证：

- 三种扩展名的准确时长、固定格式、只读能力和独立信息查询。
- DirectShow 与引擎的播放、暂停、暂停定位、继续、停止归零。
- 自然结束、暂停定位到末尾、重复开关、错误文件后重新播放。
- MIDI 绕过 PCM 淡入和输出设备配置；解码转换入口返回 E_NOTIMPL。
- IBasicAudio 包装与直接按原版公式调用 COM 的 HRESULT、读回值一致。

本机、XP SP3、Win7 SP1 已通过上述播放控制回归。另已通过文件信息、普通音频恢复、进度定位回归。
XP／Win7 Release 静态导入审计通过：x86、子系统 5.01，19 个 DLL、647 个导入。
本地日志位于 `rebuild/out/midi-check-20260919/`。

必须区分接口恢复与设备实际支持：

| 实测环境 | put_Volume | put_Balance |
| --- | --- | --- |
| XP SP3 虚拟机 | S_OK，读回存在设备幅度量化 | S_OK |
| Win7 SP1 虚拟机 | E_FAIL（0x80004005） | VFW_E_MONO_AUDIO_HW（0x80040253） |
| 当前 Windows 主机 | E_FAIL（0x80004005） | VFW_E_MONO_AUDIO_HW（0x80040253） |

在相同系统图上绕过重建包装、直接执行原版所用的 COM 调用，得到相同结果。
因此已经修复重建版漏接音量控制的问题，但不能宣称 Win7／现代 Windows 的每种 MIDI 驱动
都支持播放器音量／静音／平衡。失败时 GetVolume 还可能返回缓存值，不能据此判定声音已变化。
本次没有用系统总音量或自行合成 PCM 替代原版接口，也未进行音频回环录制或原版 EXE 的听感 A/B 比较。

## 系统行为参考

- [IGraphBuilder::RenderFile](https://learn.microsoft.com/en-us/windows/win32/api/strmif/nf-strmif-igraphbuilder-renderfile)
- [MIDI Parser Filter](https://learn.microsoft.com/en-us/windows/win32/directshow/midi-parser-filter)
- [MIDI Renderer Filter](https://learn.microsoft.com/en-us/windows/win32/directshow/midi-renderer-filter)
- [IMediaSeeking::SetPositions](https://learn.microsoft.com/en-us/windows/win32/api/strmif/nf-strmif-imediaseeking-setpositions)
- [IBasicAudio::put_Volume](https://learn.microsoft.com/en-us/windows/win32/api/control/nf-control-ibasicaudio-put_volume)
