# 音频引擎恢复记录

## 原版调用链证据

`TTPlayer.exe.pseudo.c` 中已经能够直接确认以下顺序：

1. `TTPlayer_wWinMain`（`004C0E8F`）在创建播放器前初始化声音库，并在界面退出后逆序关闭。
2. `004C8421` 把 EXE 同目录的 `AddIn` 追加到 `PATH`；`004CABC0` 只加载 `ttp_*.dll`，调用导出函数 `ttpGetSoundAddIn`，再枚举四类接口。
3. reader creator 类别 GUID 为 `{476D15A5-D863-416A-8A59-A9C7D72CE04E}`。creator 必须真实创建成功，格式才会进入文件对话框；DLL 文件存在本身不代表格式可用。
4. `004E323B` 把 reader 保存到播放器对象 `+0x58`，依次取得时长、输出格式和建议缓冲长度。
5. `004E3CF3` 调用 reader 的解码槽位，把 PCM 写入宿主 buffer；`004E3E64` 以 `DWORD*` 毫秒位置调用 seek。
6. waveOut writer 位于 `004E2414`—`004E28A5`：使用 `CALLBACK_FUNCTION`、有界 `WAVEHDR` 队列和 `WOM_DONE` 回收，不会把整首歌曲一次性读入内存。

## 已恢复的 x86 私有 ABI

主 reader IID（`DAT_005187C8`）为：

```text
{30C7C165-C0A9-4204-995D-456A764998FB}
```

reader creator 的槽位布局：

| 槽位 | 方法 |
| ---: | --- |
| 0—2 | `QueryInterface` / `AddRef` / `Release` |
| 3 | `HRESULT CreateReader(void** reader)` |
| 4 | 取得短名称 |
| 5 | `HRESULT GetDescription(wchar_t** text)`，返回值由 `CoTaskMemFree` 释放 |

主 reader 的已验证槽位：

| 槽位 | 原版语义 |
| ---: | --- |
| 0—2 | `IUnknown` |
| 3 | `HRESULT Open(IStream* stream, DWORD flags)`；`flags & 1` 创建实际 decoder |
| 5 | `HRESULT GetDuration(DWORD* milliseconds)` |
| 6 | `HRESULT GetFormat(WAVEFORMATEX** format)`；返回值由 `CoTaskMemFree` 释放 |
| 7 | `HRESULT GetBufferSize(DWORD* bytes)` |
| 8 | `HRESULT GetCodecName(wchar_t** text)` |
| 9 | `HRESULT GetBitrate(DWORD* bits_per_second)` |
| 10 | `HRESULT GetBitsPerSample(WORD* bits)` |
| 14 | `HRESULT Read(IPlayerBuffer* buffer)`；`S_FALSE` 表示流结束 |
| 15 | `HRESULT Seek(DWORD* milliseconds)`；插件可以回写实际位置 |

槽位 3 的第一个参数不是文件名。APE、TAK、VQF 都会对它调用 `AddRef`、`IStream::Seek`（`+0x14`）和 `IStream::Stat`（`+0x30`）。重建版因此先用 EXE 侧文件路径创建只读 `IStream`，再调用 `Open(stream, 1)`。把路径指针直接传入该槽位会在三个插件中一致触发访问异常。

宿主 buffer 的六槽 ABI：

| 槽位 | 方法 |
| ---: | --- |
| 0—2 | `QueryInterface` / `AddRef` / `Release` |
| 3 | `HRESULT SetLength(DWORD valid_bytes)` |
| 4 | `HRESULT GetCapacity(DWORD* capacity)` |
| 5 | `HRESULT GetBuffer(BYTE** data, DWORD* valid_bytes)` |

该布局同时由原版 `004E3CF3` 和三个 DLL 的解码函数确认。重建版会拒绝插件报告的越界有效长度，并在所有跨 DLL 调用外设置 SEH 边界，插件异常会变成播放错误而不是让窗口消失或留下后台进程。

## DLL 交叉验证

三个 reader 的主 vtable 地址及关键解码槽位如下：

| 插件 | 主 vtable | `Read` 槽位 14 | `Seek` 槽位 15 |
| --- | --- | --- | --- |
| `ttp_ape.dll` | `60118320` | `6010284A` | `601028E4` |
| `ttp_tak.dll` | `1000D218` | `100015D7` | `10001664` |
| `ttp_vqf.dll` | `801D6158` | `801D1B4B` | `801D1C55` |

三者的槽位 5、6、7、8、9、14、15 具有相同的参数数目和返回约定。APE/TAK 输出 PCM；VQF 样本输出 32 位 IEEE Float PCM。VQF 的延迟依赖 `tvqdec.dll` 与 reader DLL 一样必须来自 EXE 同目录的 `AddIn`。

## 当前重建实现

- `PluginManager` 保留 AddIn、reader、decoder、encoder 和歌词搜索 provider
  的 factory/session 引用直到最后一个后台消费者结束；关闭顺序保证不会在
  DLL 卸载后调用私有 vtable。原版实际只有这四个注册类别，不存在独立的
  Sound AddIn writer/processor 类别。
- `OpenReader` 按已注册扩展名依次尝试 creator；压缩流所需的 decoder creator
  由 `004CD045` 选择。metadata 槽 3--6 和 thumbnail 槽 3/4/5/8/9 同样由
  reader QI 提供，文件信息页可在隔离 helper 中提交文本标签和封面事务。
- encoder creator 按 `004CD21E/004CD28F/004CD30C` 执行配置、创建和目标
  path/`IStream` 打开；转换线程按 `00412723` 调用 Start、逐块 Write 和
  Finalize，而不是把编码器名称作为不可执行的格式列表。
- `CreateDecodedAudioSource` 是播放和离线转换共用的输入入口，覆盖 AddIn、
  Media Foundation、AIFF/AIFC/AU、CDA 和 75 Hz 精确 CUE 分段；建议缓冲长度
  与 `TTPlayer.xml` 的文件/输出缓冲共同决定有界块大小。
- 解码 PCM 依次通过 ReplayGain、ttpcomm ordinal 103 EQ、ordinal 104
  Surround、Winamp DSP 以及统一输出转换。输出转换消费位深、ordinal 102
  SSRC、质量模式和 dither 设置；dither 已恢复 `004AAC63/004AAEC1` 的
  97 项洗牌、16384 项 TPDF、八组误差反馈系数和 reset 语义，再送入明确选中的 waveOut、DirectSound、
  KS 或 ASIO 后端；KS/ASIO 打开失败不会静默回退到另一类设备。
- `AutoScanGain` 按 `004B107E/004B1950/004B1A5C` 直接分析 ReplayGain
  之前的实际播放 double PCM；即使已有 gain/peak 也会重新分析。只有完整自然
  EOF 才提交标签，stop、seek、失败、URL、CUE、CDA 和压缩包成员均不回写；
  live 提交保留只读属性，按路径去重且共享冲突重试最多约五秒，不会另开第二个
  解码器或留下无限后台任务。`SkipScanGain` 只属于手动扫描窗口。
- 开始、暂停、seek、显式停止和自然结束使用 `004AC605` 的五个模式位以及
  `FadeDuration[0..3]`/`TrackFadeDur`。ASIO 对应原 vtable 的音量/平衡槽是
  `E_NOTIMPL`，因此保持 unity 是原版边界，不应人为加入另一套软件增益。

## 真实样本验证

`sound_addin_probe` 支持在 AddIn 目录参数后附加任意样本路径，执行完整的 creator/open/format/read/seek/read 链路。例如：

```powershell
.\rebuild\build\Debug\sound_addin_probe.exe .\AddIn sample.ape sample.tak sample.vqf
```

本次使用 FFmpeg 回归样本得到：

| 格式 | 输出格式 | 时长 | 建议块 | 读至 EOF | seek 后读取 |
| --- | --- | ---: | ---: | ---: | ---: |
| APE | PCM，2 声道，44100 Hz，16 bit | 15665 ms | 4096 | 2763372 bytes / `S_FALSE` | 4096 bytes |
| TAK | PCM，2 声道，44100 Hz，16 bit | 9500 ms | 44100 | 1675800 bytes / `S_FALSE` | 44100 bytes |
| VQF | IEEE Float，1 声道，22050 Hz，32 bit | 150104 ms | 4096 | 10600448 bytes / `S_FALSE` | 4096 bytes |

三个样本均成功打开并完整解码，末次读取按 ABI 返回 `S_FALSE`；中点 seek 返回 `S_OK`，随后继续输出非空数据。

本轮宿主机还用提供的 `陈慧娴 - 千千阙歌.flac` 验证了 FLAC reader：时长
299106 ms，44.1 kHz/16-bit/stereo，解码至 EOF 共 52,762,340 PCM bytes，
FNV-1a 为 `bb35ad7ed2874bb0`。同一文件进入 DirectSound 后播放位置推进至
3.24 秒；本机没有物理光驱、可接受的 KS endpoint 或 ASIO 注册项，相关后端
只声明确定性 ABI/生命周期测试通过，不声明硬件可听验证。

## Release 播放回归

`ttpcomm.dll` ordinal 103 的 EQ 配置对应原版独立函数 `FUN_004B182C`，处理顺序则由
`FUN_004B1B81` 固定为槽 4 后接槽 5。VS 18 的 Win32 `/O2 /Ob2` 曾把恢复版的配置
方法折叠进 processor 构造函数，随后槽 5 在 Release 中持续报告 0 个输出采样；因此
reader 能完整解码，但 waveOut 得不到任何可提交的块，主窗口显示“状态：无效”和
`00:00`。Debug 以及 `/Ob0` 不会触发，旧测试中的 `assert` 又在 `NDEBUG` 下被删除，
所以该问题只在最终 Release 程序暴露。

重建版现保留 `FUN_004B182C` 的独立调用边界，只禁止这一配置方法被内联，其余解码、
double processor 和 waveOut 循环继续使用 Release 优化。`audio_recovery_tests` 的关键
检查改为 Release 也执行的显式失败，`audio_playback_probe` 则读取真实 `TTPlayer.xml`，
要求插件加载、EQ/Surround 链打开并让播放位置至少前进 500 ms。干净 Windows Sandbox
入口见 `tools/windows_sandbox/run_release_playback_sandbox.ps1`。

## 后续恢复

metadata IID/槽位、ReplayGain 数学处理、ttpcomm EQ/Surround 顺序、CUE
子曲目和 CDA raw-sector source 已继续恢复，证据、实现边界和验证命令见
[`AUDIO_RECOVERY.md`](AUDIO_RECOVERY.md)。APE/TAK/VQF 已完成真实样本
reader/decoder/buffer/metadata 回归；CDA 因当前机器没有物理 Audio CD，仍明确
保留硬件实测边界，不把仅通过 SDK 结构和错误路径验证表述为光驱实测完成。
