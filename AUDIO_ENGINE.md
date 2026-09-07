# 音频引擎恢复记录

## 2026-09-07：歌词/进度条松手时短暂回跳

上一轮只修复了 seek 淡化，仍在释放鼠标时立即撤销 UI 预览。音频线程
尚未消费请求或仍在重置输出缓冲时，`AudioEngine::Position()` 返回的是
旧设备时钟，导致歌词、进度和 LED 先回旧位置再跳到目标位置。只在提交
请求时写一次 `position_ms_` 仍会被旧缓冲的最后一次时钟更新覆盖；只看
请求队列是否为空也不对，因为取走请求不代表 seek 已完成。

现在由音频引擎统一持有已接受的目标位置和递增请求序号：

- 入队即通过 `Position()` 发布目标，UI 不等待解码线程或阻塞消息循环。
- 工作线程取走请求后仍保持目标；waveOut/KS/ASIO、DirectSound 和 MCI
  在新输出位置准备好后确认请求，先发布新时钟，再撤销目标覆盖。
- 确认必须匹配最新序号，防止旧请求完成后覆盖连续拖动的新目标，亦
  覆盖连续两次拖到同一毫秒的情况。停止、失败或工作线程退出时清理
  未完成目标，迟到的确认不得恢复旧目标。
- 歌词 `WM_LBUTTONUP` 先提交目标，再 `ReleaseCapture`，避免同步的
  `WM_CAPTURECHANGED` 在目标提交前触发旧位置绘制。

原版 `004ABDE4` 在同步 seek 内重置处理器/输出并用 `InterlockedExchange`
更新位置后才返回；这次修复是重建版异步线程模型下的位置交接，不是
把原版同步等待直接搬到 UI 线程。上一轮的无 seek 淡化行为保持不变。

`progress_seek_tests` 新增队列已消费但解码未完成、旧设备时钟晚到、前后
快速 seek、同目标不同序号、停止/失败后的迟到确认，以及普通/迷你歌词
真实松手处理函数的回归检查。宿主机显式播放测试从松手返回开始连续
采样 250 ms，不再仅比较第 250 ms 的最终结果；同时检查前进、后退和
暂停 seek。样本仍为 `Fighting INMU Way.wav`，输出音量为零。

## 2026-09-07：主窗口进度条松手后直接定位

原版伪代码调用链：

- `00460AB1` 区分主进度控件（主对象 `+0x1B10` 的 HWND）和音量等
  控件，仅在通知低字为 `4`（`SB_THUMBPOSITION`）时调用播放器接口
  `vtable+0x3C` seek；拖动通知只经 `0045CE05` 更新位置/LED 预览。
- `00428DCD` 在滑块 `+0x3C` 的 tracking 标志非零时拒绝外部位置更新，
  因此播放时钟不会把鼠标正在拖动的滑块拉回。
- `004ABDE4` 的通用 seek 除模式位外还检查私有 fade helper、音量及
  排队音频量，并非所有 seek 都无条件执行整段淡出、淡入。

重建版此前在 `WM_LBUTTONDOWN` 和每个 `WM_MOUSEMOVE` 都调用通用
`AudioEngine::Seek`，反复重设淡出计时，而 `WM_LBUTTONUP` 不提交最终
位置。这同时造成拖动过程中提前 seek、松手后等待淡出及丢失最后落点。

现改为独立的进度预览状态：按下/移动只更新滑块和 LED，松手时以最终
坐标调用一次 `SeekWithoutFade`；失去捕获或取消时清除预览且不提交。
横/竖进度条及普通/迷你皮肤共用此路径，坐标继续采用原有一像素 inset
及滑块尺寸。直接定位保持播放/暂停状态，保留开始、暂停、停止和键盘
通用 seek 的原有淡化设置。若松手打断旧的通用 seek，则取消其淡出或
后续淡入阶段，恢复 seek 衰减增益，防止旧目标稍后覆盖新目标。

`progress_seek_tests` 通过隐藏测试 HWND 向实际 `PlayerWindow` 鼠标处理
函数发送消息，检查普通/迷你、横/竖方向、端点/越界松手、无最后 move、
取消/失去捕获、快速重复拖动及两阶段 seek fade 取消。不启动完整应用，
不读写用户播放列表或配置；常规测试在音频命令边界检查，不依赖声卡。
显式 `--playback` 模式额外使用宿主机真实解码和输出设备，音量为零。

宿主机样本 `Fighting INMU Way.wav`：waveOut 和软件 DirectSound 各测
普通/迷你模式，在启用 5000 ms seek 淡出时，四组均在松手后 250 ms
检查点到达目标并继续推进，`transition_gain == 1` 且没有 pending fade；
暂停后定位保持暂停。此验证不等同于逐样本对比原版音频输出，也没有
将私有输出缓冲 ABI 的所有分支宣称为逐字重建。

```powershell
cmake --build rebuild/build --config Release --target progress_seek_tests
ctest --test-dir rebuild/build -C Release -R progress_seek_tests --output-on-failure
& ./rebuild/build/Release/progress_seek_tests.exe . --playback `
  'C:\Users\Sonic853\Music\test\Fighting INMU Way.wav'
```

## 2026-09-07：M4A / MP4 炸音修复

样本为宿主机 `C:\Users\Sonic853\Music\test` 中的
`Fighting INMU Way.m4a` 与 `Fighting INMU Way.mp4`。两者均由
`ttp_aac.dll` 的 MP4 reader 处理，时长 292664 ms，输出 44100 Hz 双声道。

根因不是容器扩展名或读取块之间的拼接，而是播放时漏传浮点格式协商位：

- 原版 `FUN_004b0d1a` 的机器码在 `004B0D80` 明确 `push 3`，随后在
  `004B0D82` 调用 `FUN_004b0b51`；`004B0BDF` 将该 flags 参数传入
  `FUN_004e323b`。反编译文本丢失了部分调用实参，因此同时核对了 EXE 反汇编。
- `FUN_004e323b` 在 `004E3A90`—`004E3A99` 按 `(flags & 2) | 1`
  设置 decoder 输出的 `wFormatTag`，再调用 `004CD045`。`flags=3`
  表示启用解码并请求 `WAVE_FORMAT_IEEE_FLOAT`；元数据路径仍使用 `flags=0`。
- 重建版 `PluginManager::OpenReader` 之前使用 `flags=1`，请求的是整数 PCM。
  实测随附 AAC 插件此时返回 tag 1 / 32-bit，但实际样本仍是 float32；
  修改 flags 后两文件的完整原始解码字节校验和均保持
  `42ee4ea2f600c8ba`，只有格式标记正确变为 tag 3。
- 将 float32 的位模式当作 int32 音量值，会把大部分正负样本推至接近
  ±0.5；尤其 `-0.0f` 的 `0x80000000` 会变成负满幅，造成炸音。
  修复恢复原版 flags 协商，不按扩展名强改格式，也不把真实整数 PCM 当作浮点。

宿主机静默解码各文件前 30 秒（每文件 2646000 个声道样本），独立使用
Windows Media Foundation 解码作参考。允许 AAC priming / edit-list 的
1024 帧起始差异后，结果如下；该对齐仅用于测试，不改动播放时间轴。

| 指标（两文件相同） | 修复前 | 修复后 |
| --- | ---: | ---: |
| 插件输出格式标记 | PCM / 32-bit | IEEE float / 32-bit |
| 归一化峰值 | 1.0 | 0.861668 |
| 与参考的 RMS 误差 | 0.371258 | 0.00000878984 |
| 与参考的最大绝对误差 | 1.0 | 0.0000154972 |
| 误差超过半幅的样本数 | 177274 | 0 |

修复后参考误差低于一个 16-bit LSB；8192 与 65536 字节分块得到逐样本相同
的结果。两文件均完整读取至 EOF（103243776 字节 / 12605 块），seek 后继续
返回 8192 字节。另测 `陈慧娴 - 千千阙歌.flac` 保持 tag 1 / 16-bit，完整解码
校验和仍为 `bb35ad7ed2874bb0`。

新增 `pcm_integrity_probe`，只读样本，不打开声音输出设备，不修改标签。
`--verify-reference` 会在参考误差超过一个 16-bit LSB 时返回失败；此严格比较
适用于本次 AAC 回归，不作为所有独立解码器必须逐样本相同的假设。

```powershell
.\rebuild\build\Release\pcm_integrity_probe.exe --verify-reference `
  .\rebuild\build\Release\AddIn `
  'C:\Users\Sonic853\Music\test\Fighting INMU Way.m4a' `
  'C:\Users\Sonic853\Music\test\Fighting INMU Way.mp4'
```

验证：Release 全量编译成功，CTest 21/21 通过。新增 ABI 回归限制播放 reader
必须接收 flags 3、压缩 reader 后续 decoder 必须请求 tag 3，同时验证仅元数据
格式不被解码替换；PCM 转换回归覆盖 float32 正负零、正负幅度及真正的 int32。
宿主机 `audio_playback_probe` 使用现有音量 8%、DirectSound 设置，两个文件
分别推进至 3230 / 3240 ms，保持 playing、无播放错误后正常停止。当前硬件
缓冲不可用，按既有逻辑使用 DirectSound 软件缓冲；该诊断不是本次炸音根因。
播放测试前后样本和 Release `TTPlayer.xml` 的 SHA-256 均未改变。
以上是数据对比及实际输出路径检查，不声称已进行主观听音或与原版逐位一致。

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
| 3 | `HRESULT Open(IStream* stream, DWORD flags)`；`flags & 1` 创建实际 decoder，`flags & 2` 请求浮点输出；播放传 3，元数据传 0 |
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
