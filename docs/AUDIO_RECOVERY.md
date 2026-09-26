# metadata、ReplayGain、CUE、CDA 与 processor 链恢复

本轮实现以 `reverse/decompiled/TTPlayer.exe.pseudo.c` 的实际调用点为准，
没有根据现代播放器接口臆造旧 ABI。主要证据与实现如下。

## metadata 私有接口

- `FUN_004139B5` 对打开后的 reader 执行 `QueryInterface`，IID 为
  `{7AD84E00-5FEF-4481-B532-FBBD677E67C2}`。
- 接口槽 3 是 `GetCount(DWORD*)`，槽 4 是
  `GetAt(DWORD,wchar_t**,wchar_t**)`，槽 5 是
  `GetValue(const char*,wchar_t**)`，槽 6 是
  `SetValue(const char*,const wchar_t*)`。
- 槽 4/5 返回的字符串均由调用方 `CoTaskMemFree`；reader 槽 4 返回的
  capabilities 中 bit `0x4` 是写标签前的原版检查条件。
- `LegacyReaderSession` 现在保留 metadata 接口到 reader session 结束，
  支持枚举、按名称读取和受 capability 保护的写入。Media Foundation
  fallback 也会从 Windows Property System 读取 Title、Artist、Album。
- 播放成功后 Title、Artist、Album 会回填 Track，并进入主皮肤 info 轮播。

真实 VQF 样本验证结果：接口枚举得到四项
`Title/Artist/Copyright/Comment`；同一次探针中 APE、TAK、VQF 继续完整
解码至 `S_FALSE`，中点 seek 后均继续返回非空数据。

## ReplayGain 与 processor 顺序

- `FUN_004B107E` 读取 `replaygain_track_gain` 和
  `replaygain_track_peak`。只有两项都存在时才建立增益。
- 增益为 `pow(10.0, gain_db * 0.05)`。
- `FUN_004B1950` 的 double stream 以 `[-0.5,+0.5]` 为标称满幅范围；
  超出范围时保留原版 `tan` 软限幅公式，而不是替换成常见的 `tanh`。
- `FUN_004B1B81` 的顺序是 ReplayGain，然后 EQ 的槽 4/5，最后
  Surround 的槽 2。seek 时按 `FUN_004B16FE` 重置 EQ 槽 6 与 Surround
  槽 3。
- EQ/Surround 分别由 EXE 同目录、已成功加载的 `ttpcomm.dll`
  ordinal 103/104 创建。EQ 初始化槽 1、参数槽 2、处理槽 4/5；Surround
  初始化槽 1、处理槽 2。所有调用保持 x86 `__thiscall`，并置于 SEH
  边界内。
- `/Playback` 的 `AutoGain/AutoScanGain/SkipScanGain` 与 `/Equalizer` 的
  `Profile/Surround/Current` 已读入设置。`AutoGain` 开启且两个标签均
  存在时才执行 ReplayGain；`Profile == -2` 保持原版关闭 EQ 的状态。

## CUE 子曲目

- 原版 `FUN_004E323B` 将外部子曲目参数减一后按数组取轨道；公开编号是
  从 1 开始的**解析顺序序号**。原版解析后也重写 Tracknumber，不保留文本
  TRACK 跳号作为公开编号。重建版现已采用该顺序编号，同时保留源 TRACK 数字。
  对旧重建版播放列表，仅在缓存标题或越界轨号能明确识别时迁移；歧义数据不猜测。
- 当前重建版解析器支持 ANSI、UTF-8 BOM/无 BOM、UTF-16 LE/BE，以及
  `FILE/TRACK/TITLE/PERFORMER/INDEX 00/01`，保存单轨 REM 和未知原始行。
  UTF-16 支持及正确的无 BOM UTF-8 解码是保留的改进，不能据此认定原版相同。
- 当前相对 FILE 按 CUE 所在目录解析；分段 source 打开实际音频，再限制
  duration/read/seek。正常情况下使用本轨 INDEX 01 与同文件下一轨 INDEX 01
  划分范围；没有有效的下一轨边界时使用实际音频长度。保留精确采样帧边界，
  Seek 不再量化到 75 Hz，拒绝结尾定位，并利用旧插件返回的实际位置补读。
  已恢复 CUE 同名文件的原扩展名、APE、TAK 后备查找，播放与信息读取共用候选列表。
- 导入时展开逻辑曲目，每项保留 CUE 路径和 subtrack，不生成独立音频文件。
  TTBL flags bit `0x1` 后的 `uint16` 已恢复读写，但仍需区分上述编号语义。
- 文件属性读写现已传入子曲目，标签来源改为独立 CUE 标签集合。保存仅合并
  修改字段并更新 CUE，保留编码、FILE、TRACK、INDEX 和未知行；轨号及 Lyrics
  不可写，包内 CUE 只读。只读、过期快照和替换失败均返回错误，不覆盖原文件。
- 手动及播放时 ReplayGain 扫描已接通分段 PCM 和 CUE 标签写入。

详细函数依据见 [原版 CUE 实现分析及重建版差异](CUE_IMPLEMENTATION_ANALYSIS.md)；
修复内容及测试结果见 [CUE 修复与验证记录](CUE_RECOVERY_VALIDATION.md)（2026-09-27）。

## CDA 数字抓轨

- 路径保持原版 `X:\TrackNN.cda` 语义。
- `IOCTL_CDROM_READ_TOC` 取得轨道与 lead-out，拒绝 data track。
- 输出格式固定为原版的 PCM 44100 Hz、2 声道、16 bit，block align 4，
  `nAvgBytesPerSec = 176400`。
- 抓取使用 `IOCTL_CDROM_RAW_READ`、`TrackMode=CDDA`、2352 字节/扇区、
  75 扇区/秒；`DiskOffset` 按 Windows 接口要求使用 `LBA*2048`。
- 每次最多读取原版的 24 扇区；失败时按 24、20、16、12、8、4 的顺序
  回退，以兼容拒绝大块 raw transfer 的旧光驱。

当前机器没有可读取的物理 Audio CD，因此 TOC/LBA/请求结构和无硬件
错误路径已经编译、测试，真实光盘的驱动返回数据仍需在有光驱的 x86
Windows 环境做最终硬件验证。这一点不应被表述成已经完成硬件实测。

## 验证入口

```powershell
cmake --build rebuild/build --config Debug --parallel
ctest --test-dir rebuild/build -C Debug -R "audio_recovery_tests|ttpcomm_api_test|ttplayer_ui_smoke" --output-on-failure
rebuild/build/Debug/sound_addin_probe.exe rebuild/build/Debug/AddIn sample.ape sample.tak sample.vqf
rebuild/build/Debug/ttpcomm_processor_probe.exe rebuild/build/Debug/ttpcomm.dll
```

`audio_recovery_tests` 独立覆盖 CUE 轨界、CUE album/performer、TTBL
subtrack round trip、XML processor 设置和四类 AddIn registry 枚举，不依赖
耗时且偶发阻塞的 Windows 压缩文件夹皮肤解包测试。
`ttpcomm_processor_probe` 已实际完成 ordinal 103/104 的 create、init、process、
reset、destroy，两个对象均通过 x86 ABI 探测。
