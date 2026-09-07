# 主要功能恢复与伪代码映射

本文记录本轮针对 `TTPlayer.exe.pseudo.c` 的行为级恢复。地址表示能够从现有
EXE、伪代码或随附 DLL 交叉确认的调用边界；不能由这些材料推出的私有对象布局，
不会标成“逐字源码”或“二进制完全一致”。文件选择器继续采用用户指定的现代
Common Item Dialog，这是有意兼容改动。

## 1. Sound AddIn ABI

原版加载链为：

1. `004C8421` 将 EXE 同目录 `AddIn` 追加到当前进程搜索路径；
2. `004CABC0` 仅扫描 `ttp_*.dll`；
3. `004C88D9` 调用 `ttpGetSoundAddIn`；
4. `004C8954` 枚举接口；
5. `004CDA07`、`004CDAD6`、`004CDB93`、`004CDC47` 分别保存 reader、
   decoder、encoder 和 lyric-search provider。

这纠正了早期“writer/processor/encoder 三类 AddIn”的错误分析：Sound AddIn
没有独立 writer 或 processor 注册类别。标签/封面写回位于 reader 的 metadata
和 `ISoundThumbnail` QI；实时/离线 PCM processor 来自 `ttpcomm.dll` ordinal
103/104 及 Winamp DSP。

当前实现位于：

- `src/plugins/plugin_manager.cpp`：四类注册表、factory/session 生命周期、
  x86 SEH 边界；
- `src/audio/audio_engine.cpp`：reader/decoder 解码与统一源；
- `src/ui/playlist_transforms.cpp`：encoder 配置和批量转换；
- `tools/file_info_probe.cpp`：metadata 与 thumbnail 写回。

reader 使用 `IStream` 打开并通过六槽宿主 buffer 输出；decoder 选择遵循
`004CD045`，读/seek/reset 对应 `004E3CF3/004E3E64`。encoder 遵循
`004CD21E/004CD28F/004CD30C` 的 creator/open 顺序和 `00412723` 的
Start/Write/Finalize 工作线程。metadata 使用槽 3--6；thumbnail 使用能力位
`4` 以及槽 3/4/5/8/9，替换时先逆序删除旧图，再写入新图。

## 2. 原生输出设备

设备键由 `004918B1/004918FE` 解释：零尾 GUID 的 `Data1.high/low` 表示
backend/ordinal；非零尾 GUID 表示 DirectSound GUID。当前四条路径互不假冒：

- waveOut：`004E2414--004E28A5` 的 callback、有限 `WAVEHDR` 队列和
  `WOM_DONE` 回收；
- DirectSound：`004C3170`，按选中 GUID 创建 primary/stream buffer，并区分
  hardware/software buffer；
- KS：`004E029E` 等，SetupAPI 过滤 Audio+Render pin，`KsCreatePin`，
  `IOCTL_KS_WRITE_STREAM`，overlapped packet 所有权、position/reset/teardown；
- ASIO：`004E131C/004E1383/004E1450/004E14BD/004E1610/004E1860`，
  CLSID 激活、单实例 callback、period queue、channel sample conversion 和
  dispose barrier。

设置页的 `004E207B` 只读能力链也已接入：init、output channel、preferred
buffer、output channel 0 valid bits，以及原表中的 13 次 `canSampleRate`；四行
使用 `%d Channels`、采样率列表加 `Hz`、`%d Bits`、`%d Samples`。原版在选择
后懒计算，重建版为避免 UI 进入旧驱动而在受 Job/四秒超时保护的同位数 helper
快照中预先物化，这是明确的隔离时序差异。

选定 KS/ASIO 后端失败时返回明确错误，不回退到 waveOut/DirectSound。原 ASIO
输出对象的 SetVolume/SetBalance 是 `E_NOTIMPL`，因此 ASIO 保持 unity。KS
`004E061B` 的硬件 topology volume-node 遍历仍是设备特定的未证实边界；当前
只对尚未提交的 PCM packet 施加用户音量和平衡。

## 3. 统一 PCM 转换链

`004B0D1A` 是输出转换入口；格式判断、整数/浮点转换和 dither 可追至
`004C8492`、`004AA360`、`004AA8B0`、`004AAFAF`、`004AAC63`。当前
`PcmOutputTransform` 被播放与离线转换共同使用，支持：

- PCM 8/16/24/32 bit；
- IEEE float 32/64 bit；
- `WAVEFORMATEXTENSIBLE` container/valid-bits/subtype；
- `ttpcomm.dll` ordinal 102 SSRC 及其三种质量模式；
- 输出位深和四种 dither 设置；`004AAC63/004AAEC1` 的 97 项 MSVCRT
  `rand()` 洗牌、16384 项循环 TPDF、8 组最多 21 阶误差反馈系数、采样率
  选择、round-to-nearest-even 和 reset 清空语义均已恢复；
- 有界 block、frame 对齐、flush 和 reset。

AddIn、Media Foundation、AIFF/AIFC/AU、CDA 与 CUE 都进入同一链。处理顺序
为 ReplayGain -> EQ ordinal 103 -> Surround ordinal 104 -> Winamp DSP ->
输出格式转换，不再为“播放”和“转换格式”维护两套不一致的 WAV/MF 路径。
`AutoScanGain` 按 `004B107E/004B1950/004B1A5C` 挂在同一播放 double PCM
上，并位于 ReplayGain 乘法之前；原代码只检查该开关，因此已有 gain/peak 仍会
被完整自然播放后的新结果覆盖。seek/stop/失败会销毁分析器，live 写回不清除只读
属性，也不使用后台第二次解码；共享冲突按路径去重并在约五秒内有界结束。
`SkipScanGain` 仍只控制 `004A4E8C` 的手动扫描路径。
原调用者只使用 distribution 1 的 TPDF 分支；未暴露的其他 distribution 没有
伪造为设置项。原 EXE 使用 x87 扩展精度，现代编译器在极少数量化临界点仍可能
出现最后一位差异。

## 4. 淡入淡出

`CSound::ReadThreadProc` `004AC605` 读取五个模式位；`004ACD95/004ACE17`
维护过渡状态。当前开始、暂停、seek、显式停止、自然结束分别消费对应模式位，
普通开始/恢复使用 `FadeDuration[0]`，暂停使用 `[1]`，seek 使用 `[2]`，停止
使用 `[3]`，曲目自然结束使用 `TrackFadeDur`。设置页 Apply 会提升音频配置
revision，下一次输出重建会实际读取这些值。

waveOut/DirectSound 执行完整过渡。ASIO 的 unity 行为由原 vtable 证明；KS
因未恢复设备 topology volume 而不预填淡变 packet，避免在硬件队列前插入长
静音。MIDI/MCI 不经过 PCM worker。

## 5. 媒体库

启动、读取、保存和查询链分别为：

- `004C03FD -> 004AF271`：创建容量；
- `004C038B -> 004AF7F4`：读取 EXE 同目录 `Music.library`；
- `004616BD -> 004AF838`：退出保存；
- `0048A38E/0048A818/00483C59/004843CC`：查询、树菜单、命令和目录。

重建版已实现库树、分类/查询结果、播放快照、评分、排序、复制、重命名、回收站
删除、属性编辑、目录初扫和 `ReadDirectoryChangesW` 监视。后台扫描持有
`PluginManager` 生命周期租约；匹配 AddIn 的文件通过原 reader metadata ABI，
只有没有匹配 reader 的内建格式才使用 Shell property store。`MaxItemCount`
仅是下一次 hash capacity hint，不截断结果。

## 6. 文件信息与标签编辑

`00464A94` 将命令路由到当前播放项或播放列表选择；属性页行为来自
`0042FCA7/0042FF7C`，AddIn metadata/thumbnail 写回来自
`004ADD9C/004AD663`。当前隔离 helper 支持：

- AddIn 文本 metadata 更新/删除；
- AddIn 封面 BMP/JPEG/JPG/GIF 替换和删除，并兼容 PNG（按签名传递
  `image/png`）；
- 原版内建 MP3 路径 `004D9AC8/004D9B56` 的 ID3v1、ID3v2、APEv2；
- `004D9B0B/004D9C09/004D9C7E/004DC59D` 的读取优先级、写入类型、
  padding 和 ID3v2 编码策略；
- sibling 临时文件、atomic replace、未知私有 ID3 frame/APE item 保留。

WAV RIFF/INFO 与没有 reader metadata-write QI 的 Shell 格式保持只读；CUE 逻辑子曲目、压缩
包成员不能写物理标签。helper 有超时/Job/协议版本边界，第三方 DLL 崩溃不会让
播放器 UI 消失或留下无限等待。

## 7. CD、VCD 与抓轨

`0047D023` 先读取光盘 TOC，过滤 data track；失败时依次枚举 Shell `*.cda`
和 `MPEGAV/*.dat`。`0048398F` 将结果替换进当前列表并播放首项。重建版的
Audio CD 对话框支持“播放”和“添加到播放列表”，CDA source 使用
`IOCTL_CDROM_READ_TOC` 与 `IOCTL_CDROM_RAW_READ`，每帧严格 2352 bytes。
CDA 也进入统一转换/encoder 工作线程，因此可作为批量抓轨源。VCD 的原程序
可见入口是 MPEGAV DAT 列表；当前交给 Media Foundation 播放。

本机没有物理光驱，故 TOC、raw read、换盘/错误恢复尚无硬件验证；没有证据支持
把 PBC、视频标题/章节导航或光驱门控制伪造成原版功能。FreeDB/CDDB 服务已经
退役，入口/设置保留但不会报告虚假成功。

## 8. URL 播放

资源 204 对话框对应 `004809FC`，提交由 `00483A64` 执行；`0041B6EA`
检查字面量 `://`。当前 URL 成功进入播放列表后才调用
`SHAddToRecentDocs(SHARD_PATHW)`，播放通过统一 source 的
`MFCreateSourceReaderFromURL`，本地 HTTP WAV 已验证可推进。

旧 netacc reader、代理、缓存、边听边下载、鉴权和远程播放列表协议未从现有材料
恢复，相关 Network 设置不影响 Media Foundation URL。这些字段作为兼容数据保存，
不会伪装为已经连接失效服务。

## 9. `/reg` 与 `/unreg`

`004C0900` 表明 `/reg` 不是静默全注册，而是打开仅包含 About 与系统关联页的
两页属性表，并定位到关联页；当前实现保持该行为。`/unreg` 按 `0049CC0E` 的
动态格式源和 `0049CD41` 的删除路径处理 reader/播放列表扩展名、AudioCD 与
Directory shell verb，批量结束后只发送一次 `SHCNE_ASSOCCHANGED`。

当前注册采用现代 Windows 的 per-user、所有权感知策略，不删除其他播放器接管的
关联，也不依赖 `ttpsvr.exe` 或 HKLM 提权。文件关联测试使用隔离注册表根，不会在
回归时修改宿主机用户默认应用。

## 10. 设置的运行时消费者

序列化入口为 `004B605A`，页面 Apply/关闭为 `0049FE39`，运行时通知和输出重建
可追至 `0046228D/0045BF4B`。本轮补齐的实际消费者包括：

- Device backend/buffer/output bits/resample/SSRC/dither；
- AutoGain、AutoScanGain 的实时乘法/播放流分析与 SkipScanGain 手动扫描策略；
- SoundFadeMode、四个 FadeDuration、TrackFadeDur；
- Convert 的 encoder、bits、rate、ReplayGain/EQ/Surround、目标目录、命名、
  添加到列表和 worker priority；
- ShowHotKeyInTips、MenuTips、TipsOnOpen；
- AutoLoadLyric、AutoVisible、TrimSpaces 和本地歌词 Folders/DownloadFolder；
- `Histroy/SoundPath`、`Folder`、`PlayListPath` 的现代对话框初始目录；
- media-library directories/monitor/capacity；
- 窗口 snap、标题滚动、曲目间隔、失败后停止、自动关机等通用播放行为。

退役 MSN/更新/FreeDB/在线歌词/推荐/网络缓存下载字段仍只有兼容序列化；没有对应
服务时不执行假的成功路径。`menu_bar_playlist`、部分歌词标签自动写回和 tag-pattern
历史也仍缺少可证明消费者。

## 11. 启动与框架

原版 `TTPlayer_wWinMain` `004C0E8F` 的 ttpcomm `0x50700` 检查已于
2026-09-07 按用户要求取消：启动与 `TtpComm_LoadApi` 均不调用版本查询，
也不要求版本导出存在，允许接口兼容的其他 x86 DLL 版本。后续仍依次执行
五秒单实例互斥/转发、TLS、OLE、common-controls、EXE 同目录资源验证、
AddIn/声音库、线程 hook、CoolSB、应用会话，退出时严格逆序释放。资源初始化
按 `004C077E` 加载 `ttpres.dll` 并验证 string `0x80` 的产品名，而不是只凭同名
DLL 成功加载。

兼容边界：DLL 加载、资源身份及功能所需导出/结构的检查仍保留，不能保证
导出序号或调用约定不同的任意 DLL 可用。`ttpcomm_compatibility_tests` 用
不同版本和无版本导出的模拟 DLL 验证加载层、实际 `TtpCommRuntime` 启动层、
版本查询零调用、可选导出为空及 EXE 本地路径约束；真实运行仍以提供的 DLL
验证，不将模拟 DLL 的加载通过等同于其他发行版的完整功能验证。

消息泵按 `004B5470/004B54F2` 在队列空时进入 idle，并仅在非
`WM_PAINT/WM_NCMOUSEMOVE/WM_TIMER/0x0118/WM_MOUSEMOVE` 后重新允许 idle；
`GetMessage == -1` 重试，`00449DA8` 的 PreTranslate 消费后不会重复 Dispatch。
thread call-window hook 已恢复 `004B56E7/004B5713` 的 tooltip topmost/timer
行为。

仍不能从伪代码安全构造的是 `00411C0A` 的私有线程诊断对象、`004B591D` 的 WTL
command-bar/button wrapper、`004B5B13` 的 CBT shadow/theme wrapper，以及
完整 C++ 对象布局/message-map。重建版在自己的 owner-draw 菜单、ComCtl32 v6
按钮和 `CS_DROPSHADOW` 层实现相同可见职责，不会向未知私有布局写内存。

## 12. 宿主机验证

- Win32 Release 全量构建成功；CTest 20/20 通过（13.12 秒）。
- 15 个 `ttp_*.dll` 成功注册：14 个 reader 格式、4 个 decoder creator、
  5 个 encoder creator、4 个 lyric provider。
- 提供的 FLAC：299106 ms、44.1 kHz/16-bit/stereo；读至 EOF 52,762,340
  PCM bytes，FNV-1a `bb35ad7ed2874bb0`，seek 后继续输出；DirectSound 播放
  位置推进至 3240 ms。
- 本地 HTTP WAV：4000 ms，DirectSound 播放位置推进至 3260 ms。
- 媒体库持久化不改 numbered TTBL；监视 add/rename/delete 均被观察到，进程
  全部正常退出。
- 设置窗口宿主机 `CloseLifecycle` 自动化通过：打开、关闭、主窗响应、进程退出
  及配置提交均成功，没有强制终止或长时间未响应。
- 此宿主机只有 waveOut/DirectSound 可接受输出，无物理光驱、KS 接受项和
  ASIO 注册项；后三项只通过确定性 contract/sink/error-path 测试。
