# mp3PRO 输入解码桥接恢复

## 原版证据

本次依据 `reverse/decompiled/TTPlayer.exe.pseudo.c`，并用原版 EXE 与
根目录 `mp3PRO.dll` 的 x86 反汇编核对伪代码没有展开的回调槽位。
该 DLL 实际是 `THOMSON mp3PRO Decoder v1.3` 的 Winamp 输入插件，
导出 `winampGetInModule2`，不是 `ttpGetSoundAddIn`、DSP 或输出插件。

| 原版地址 | 职责 | 重建对应 |
| --- | --- | --- |
| `004CA86C`、`004E6D95` | 检查可选 DLL，选择普通/扩展 MPEG reader | 普通 reader 外包 `Mp3ProSource`，符合条件时检查 DLL |
| `004E6CBD` | 一次加载尝试，检查 `In_Module.version == 0x100`，填充宿主回调 | `LoadDecoder`、`InstallCallbacks` |
| `004E6FB6` | 先成功打开普通 MPEG，再有条件尝试增强解码 | `Mp3ProSource::Open` |
| `004E6DE6` | 从 Out.Open 接收 PCM 采样率、声道和位深 | `OutputOpen` |
| `004E6E4A`、`004E6E64` | 查询可写容量、向环形缓冲写 PCM | `OutputCanWrite`、`OutputWrite` |
| `004E6EAF`、`004E738F` | 标记解码 EOF，排空缓冲后才报告结束 | `OutputIsPlaying`、`Read` |
| `004E727B`、`004E72FF`、`004E732E` | 启动/恢复、暂停、停止 | `SetPaused`、读前预填充、`CloseDecoder` |
| `004E73D9` | 清空旧 PCM，调用 SetOutputTime，再清空 | `Seek` 与 `OutputFlush` 的异步确认 |
| `004E71D8`、`004E7233` | 保留压缩码率，报告 `mp3PRO\|mp3PRO Audio` | `DisplayFormat` |

触发条件来自 `004E6FB6`：本地 MPEG-2、Layer III、基础采样率小于
32000 Hz、实际解码请求，且进程内没有另一 reader 占用该 DLL。
不是“文件名为 .mp3pro 就强制使用插件”，也不是“所有 MP3 都使用插件”。
现在通过普通 reader 的 MPEG 类型和相邻帧头核实格式。

按照原版执行 `Play(filename)` → `Pause()`，只有 Play 成功且
Out.Open 提供非零、不同于基础 MPEG 采样率的 PCM 时才接受增强解码。
没有把条件扩大为任意 MP3，也没有缩窄成“必须恰好两倍采样率”。
缺失 DLL/导出、接口不符、格式未增强或初始化失败时保留普通解码。
不检查 DLL 的文件版本号；`0x100` 是内存接口布局，不是 1.3.0.0 文件版本。

## 实现

- `src/audio/winamp_input_abi.h`：cdecl 的 x86 `In_Module`（0x98 字节）
  和 `Out_Module`（0x50 字节），关键槽位用 static_assert 固定。
- `src/audio/mp3pro_source.cpp`：可选加载、单 reader 占用、PCM 环形缓冲、
  暂停/恢复/定位/停止；普通 reader 继续提供 metadata、ReplayGain、封面。
- `audio_engine.cpp`：在普通本地 MPEG 回退路径接入；已注册 Sound AddIn、
  URL、压缩包成员和其它专用 reader 仍走原有路径。CUE 内层也使用同一来源。
  增强后的整数 PCM 继续进入已有 processor、PCM 转换和输出链。
- 缓冲大小核对到 `004E7112`：以替换前的基础 PCM 计算
  `max(0x48000, 2 * nAvgBytesPerSec + 576 * nBlockAlign)`，再保证输出帧对齐。
  该处 `MulDiv(x, block, block)` 并非向 1152 帧对齐。
- 停止前先释放阻塞中的 Write，再让 DLL 的 Stop 回收线程。
  若 Stop 发生结构化异常，隔离该进程内的 decoder 占用并保留停止态桥接，
  后续歌曲回退普通 reader，防止旧线程把 PCM 写入新歌曲。
- 不调用原版桥接中没有调用的 Winamp `Init`、`Quit`、`Config`。
  频谱/波形宿主回调为空操作，DSP 回调为恒等操作；这些处理由播放器自己的链完成。
- CMake 在根目录存在 `mp3PRO.dll` 时将其复制到构建 EXE 同目录，
  它不是启动必需项，也不增加其它 EXE 依赖。

## 定位与兼容边界

根目录 DLL 的 `SetOutputTime`（`10002620`）只是写入请求；解码线程
`10002AB4` 稍后执行定位，并调用 Out.Flush（`10002AEE`）。本实现暂时
丢弃旧 Write，等待 Flush 后才接收新位置 PCM，避免拖拽后混入旧缓冲。
原版宿主的 Flush 是空操作；这里的确认是针对异步桥接的明确加固，
不能描述为与原二进制逐字相同。该 DLL 本身的整秒 seek 粒度没有改写。

宿主实测还发现 waveOutReset 后才在预填充末尾 Pause，会使暂停定位的
时钟推进约 20 ms。现改为重置后、第一次 waveOutWrite 前立即恢复暂停。
Read 在暂停定位的预填充阶段可临时恢复 producer，但不恢复输出设备播放。

其它明确差异/限制：

- 按用户既有要求，只从当前 EXE 目录加载 DLL，不采用原版 SearchPathW
  可能搜索的工作目录/PATH；系统 DLL 依赖只允许系统目录和该 DLL 目录。
- 普通 MPEG 后备仍是重建版的 Media Foundation，不是原版私有 MPEG 类。
- 文件名使用 DLL 的 ANSI ABI；不允许有损字符替换，必要时尝试短路径；
  无法安全表示则保留普通解码。
- 加入相邻帧头、PCM 格式和必要回调验证；格式初始化最多额外等待 500 ms，
  PCM/seek 确认等待上限为 1 秒。发生超时报告失败或在初始化阶段回退，
  不无限等待。但外部 DLL 内部 Play/Stop 或其线程挂死，不受此等待预算完全约束。
- 此插件与原版一样在进程内运行；SEH 入口防护不是 DLL 沙箱。
- 格式表没有增加独立的“mp3PRO 插件”分类：原版本来将其用于 MPEG reader 的增强。

## 宿主机验证

`mp3pro_source_tests` 使用独立临时 EXE 目录和不同工作目录，测试用 DLL
不复制到播放器安装目录。覆盖增强 PCM、ID3 前缀、同采样率回退、错误 ABI、
无导出/缺回调、非法 PCM、Play 失败、无格式、无 seek 确认、读取停滞、
MPEG-1/2.5/Layer II 排除、畸形数据、无 DLL、仅工作目录有 DLL、Stop 异常隔离。

增强分支用可预测 PCM 模拟输入验证：逐帧顺序/左右声道、环形回绕、EOF 尾部、
定位后无旧样本、暂停预填充、重复打开、单占用与满缓冲停止；另经真实宿主
waveOut/DirectSound 静音输出测试播放、暂停定位、恢复和后退定位，并覆盖 CUE。

根目录真实 DLL 使用合成的 MPEG-2 Layer III 静音文件验证：成功加载插件，
随后正确保持 `MPEG Layer-3 / 22050 Hz` 的普通解码，读取到 EOF 且 PCM 全零，
定位后可继续读取。此文件不是 mp3PRO 编码，因此该测试只证明实际 DLL 的
加载与非增强回退，不证明 SBR 音质或增强解码已与原版逐样本一致。

当前 `C:\Users\Sonic853\Music\test` 没有真正的 mp3PRO 样本。
仍需该类文件与原版做相同位置的 PCM/听音对照；不得把模拟插件验证写成真实歌曲验证。

Release 全量构建成功，宿主机 CTest **28/28 通过（24.28 秒）**。
新增测试的 19 个子场景通过；修复后的暂停定位测试多次重复通过。
构建后恢复原有运行配置，27 个配置/播放列表/皮肤 XML 的 SHA-256 与构建前一致。
`rebuild/build/Release/ttplayer_rebuild.exe` 和同目录的可选 `mp3PRO.dll`
已生成；没有覆盖 `D:\Programs\TTPlayer` 的安装文件。
