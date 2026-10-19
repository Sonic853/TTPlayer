# 宿主内置 MPEG Decoder 恢复

日期：2026-10-19。

## 问题

XP 的系统 WMA DMO 不一定能够初始化 MP3 格式。即使 ASF Reader 已正确读出 MP3 压缩包，仅枚举外部 DLL 的重建宿主仍可能没有可用 Decoder。原版播放器可播放同文件，是因为 EXE 另有内置 MPEG 解码器。

## 原版证据

基准 `TTPlayer.exe` SHA-256：`c7999ea5823469c1684148cac1bd196009824348ccac5cdd45978f8fb574e28c`。伪代码为 `reverse/decompiled/TTPlayer.exe.pseudo.c`。

| 地址 | 行为 |
| --- | --- |
| `004CA48E` | CSoundLibrary_Initialize 先调用 `004CA86C`，再扫描外部 AddIn |
| `004CA86C` | 注册由 `004C9AC1` 创建的内置 MPEG Decoder Creator |
| `004C96D5` | 接受 MPEG Layer III `0x55`、MPEG `0x50` 及 ADU 子类型 `0x55756461` |
| `004C9748` | 验证一／双声道，通过 TTPCOMM ordinal 11 创建解码状态；ordinal 10 查询输出位深，输出 IEEE float |
| `004C98D0` | 累积未消耗压缩字节，更新输入／输出就绪状态 |
| `004C95E5` | 通过 TTPCOMM ordinal 12 释放状态 |
| `00507F90` 至 `00507FAE` | EXE 导入表明确将 ordinal 10／11／12／13／14 绑定到 TTPCOMM.DLL |
| `004C9C1F` | 另外存在通用 ACM decoder，尝试 acmStreamOpen；不是此专用 MPEG 内核 |

`mp3PRO.dll` 是独立的可选 Winamp 输入增强路径；不是 ASF 压缩包 Decoder。重建版 `http_mpeg_source.cpp` 已使用同一 TTPCOMM DecoderIo 布局，但它是整个 IStream 的播放源，不能直接替换 packet Decoder。

## 修复

- 新增 `src/plugins/builtin_mpeg_decoder.cpp`，保持原十槽 Decoder ABI，以现有 `ttpcomm.dll` ordinal 10–14 执行解码，输出一／双声道 float64。
- PluginManager 优先尝试内置 MPEG 专用候选，其后仍按现有顺序枚举外部 Decoder。没有将 ACM 作为 MP3 默认路径。
- 保留 ASF Reader、元数据与定位流程；只补足压缩数据到 PCM 的缺失承接。
- 缓存尚不完整的 MPEG 帧，支持跨任意输入分片；按已声明 PCM 容量输出，限制压缩缓存为 64 MiB。
- 保留此前 `cbSize=22` 输出协商及 VBR 压缩格式允许平均码率为零的修正。
- 仅内置 MPEG 支持私有 EOF 排空接口。末尾补足核心需要的 8 字节前瞻，避免丢掉最后完整帧；不会向 AAC、AC3、WMA 等外部 Decoder 发送空包或改变其 EOF 约定。
- Seek 重置内置核心、分片缓存和宿主 EOF 标记。原 ordinal 14 保留部分混音历史，因此不将定位后第一帧宣称为重新创建实例后的逐字节相同状态。

## 本地验证

私有测试位于 `rebuild/tests/audio_plugins_fix_20261019/builtin_mpeg`，不进入仓库或 Action。

- 44.1 kHz 双声道样本：裸流整块、7 字节及 4096 字节输入逐字节一致，均输出 **8,497,152 字节**。独立 ffprobe 计数为 **461 帧**，等于 `461×1152×2×8`，包含末帧。
- 22.05 kHz 单声道样本：三种输入粒度逐字节一致，均输出 101,376 字节。
- 两类样本在 EOF 后 Reset 并再次解码，总 PCM 长度一致。
- 同一 MP3 封装在 ASF 内，通过实际 PluginManager / LegacyReaderSession 输出同样 8,497,152 字节。
- Windows 11 的 19 项插件集成检查 `FAILURES=0`，涵盖全部本轮 Reader、高精度 WMA、ASF/MP3、AAC、Vorbis、Opus 和 TTA。
- Release EXE 通过 XP／Win7 静态导入检查；最终 XP SP3 与 Win7 SP1 各自执行同一 19 项集成，均 `FAILURES=0`。
- 使用最终 EXE 与最终 ASF DLL，三系统分别运行原版和重建版播放器播放同一 MP3-in-ASF。原版/重建版播放时间为 Win11 3831/3810 ms、XP 3330/2840 ms、Win7 3820/3790 ms，均确认插件路径、无错误窗口、正常关闭。日志见私有 `hosts/*-final-asf-mp3.log`。

边界：本次恢复内置 MPEG 路径，没有完整补回原通用 ACM Decoder，也没有改变已冻结的插件 DLL。更多异常 MPEG、ADU 与通道变化语料仍需独立覆盖。
