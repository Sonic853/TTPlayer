# 外部命令行编码器

这些程序只供“转换格式 → 命令行编码器”使用，不是播放器启动、播放或原生 DLL
编码器的依赖。原有预设使用运行中的 EXE 旁的 `Encoders`；新增 FFmpeg 预设使用
用户提供的 `ffmpeg.exe`，通过播放器继承的 PATH 查找（例如 Scoop shims）。

## 用户自备 FFmpeg 预设

`../AddIn/ttp_clienc.xml` 末尾追加以下 13 项，原有 29 项及 `current` 不变。
Debug/Release 的同名配置也已同步追加；常规构建继续从根安装目录复制该配置。
不下载、复制 FFmpeg，不写入用户名、Scoop 版本目录或绝对路径，不修改系统 PATH。

在“转换格式 → 命令行编码器 → 配置”的预设列表中选择以 `FFmpeg` 开头的项目。
所有新预设的 `encoder` 均为 `ffmpeg.exe`。可先在启动播放器的终端验证：

```powershell
where.exe ffmpeg.exe
ffmpeg.exe -hide_banner -encoders
```

若安装后播放器尚未退出，须从继承了新 PATH 的环境重新启动。`ffmpeg.exe` 不等同于
`Encoders\ffmpeg.exe`：后者是显式相对路径，不会在缺失时自动改为 PATH 搜索。
播放器目录/当前目录中的同名程序可能优先于 PATH 中的程序。

| 预设 | 输出 | 编码参数 |
| --- | --- | --- |
| FFmpeg AAC-LC 128K / 256K | `.m4a` | `aac`，LC，128 / 256 kbit/s |
| FFmpeg MP3 VBR V0 / CBR 320K | `.mp3` | `libmp3lame`，`-q:a 0` / `-b:a 320k` |
| FFmpeg Opus VBR 128K / 192K | `.opus` | `libopus`，audio，VBR，48 kHz |
| FFmpeg Vorbis Q5 | `.ogg` | `libvorbis`，`-q:a 5` |
| FFmpeg FLAC 16-bit / 24-bit | `.flac` | `flac`，压缩级别 8，明确输出位深 |
| FFmpeg ALAC 24-bit | `.m4a` | `alac`，24-bit |
| FFmpeg WavPack 24-bit | `.wv` | `wavpack`，24-bit，无损 |
| FFmpeg WAV PCM 16-bit / 24-bit | `.wav` | `pcm_s16le` / `pcm_s24le`，RF64 auto |

调用链仍是播放器解码 → CLI 插件提供 WAV/PCM → FFmpeg 编码，不是把原始文件交给
FFmpeg；不会自动扩大播放器支持的输入格式，也不提供压缩码流直接复制。除 Opus
明确转换至 48 kHz 外，未强制统一采样率/声道；实际输出还受转换窗口设置与编码器限制影响。
无损预设只保证对收到的 PCM 无损编码；预设位深、转换窗口重采样等仍可能改变输入。

公共参数是 `-hide_banner -loglevel error -nostdin -y -f wav -ignore_length 1 -i pipe:0
-map 0:a:0`。WAV 输入的 `-ignore_length 1` 必须放在 `-i` 前，适配旧插件流式 WAV
长度头；`-nostdin` 禁止交互命令，不禁止读取媒体 stdin。`%d` 保留引号，指向现有
转换事务的临时输出，成功后才提交；`-y` 不绕过播放器对最终目标的覆盖处理。
参考 [FFmpeg 格式文档](https://ffmpeg.org/ffmpeg-formats.html#wav)、
[编码器文档](https://ffmpeg.org/ffmpeg-codecs.html) 和
[命令行文档](https://ffmpeg.org/ffmpeg.html)。

新预设统一 `tagtype="0"`，不调用尚未完整恢复的旧插件自动标签写入；不声称保留
原标题/艺术家等标签或封面。AAC 使用 FFmpeg 原生 AAC-LC，不冒充 Nero HE-AAC、
QAAC 或 `libfdk_aac`。如果用户换用精简 FFmpeg 构建，须检查对应编码器是否存在。

2026-09-11 宿主机验证：使用 Scoop shims 中的 FFmpeg 9.0.1 full build，不向测试
播放器目录复制 FFmpeg。13/13 新预设均通过真实 Release 转换窗口完成生成的
3 秒、44.1 kHz、16-bit 立体声 440 Hz 音频转换，所有测试播放器正常退出。
ffprobe 检查编码格式、声道、采样率和无损输出位深均符合预设；全部输出再用 FFmpeg
解码为 44.1 kHz PCM，长度均为 3.000 秒，RMS 0.258384–0.260921，峰值小于 0.390。
六个无损/PCM 预设解码回 16-bit 后与该输入 PCM 逐样本完全相同。此短样本不代表
全部采样率、多声道、长文件或播放器回放路径已覆盖，也不代表测试了原生 24-bit 源精度。
测试命令（在根安装目录）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File rebuild/tools/probe_conversion_window.ps1 -Encoder 3 -CliPreset 29 -Convert -Complete -Seconds 3 -SampleRate 0 -Tone
```

依次将 `-CliPreset` 改为 29–41。日志和 PCM 校验汇总保存在
`build/external-encoders/ffmpeg-presets/`。根配置、Debug、Release 的原有 29 项节点均
与修改前一致；各自 `current` 仍为 20、0、28。只修改预设和文档，没有重编译 EXE、
改动用户音乐或运行目录的其它设置；未将先前的 CTest 结果当作本次重跑结果。

## 本地安装

本次部署到 `build/Release/Encoders`。从源码重新部署时，在 `rebuild` 目录运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File cmake/install_cli_encoders.ps1 -IncludeApple -IncludeNero -RepairPresets
```

需要 `7z.exe` 和 `curl.exe`。可用 `-Destination` 指定另一份 EXE 旁的 `Encoders`。
脚本只在显式执行时下载，普通构建不联网。缓存位于 `build/external-encoders`，
按固定 SHA-256 核验；遇到不同的已安装文件会停止，不覆盖用户自备编码器。
Apple/Monkey's Audio 的 EXE 安装包仅被解压，没有运行安装程序、MSI 自定义动作、
修改注册表或系统 PATH。Apple MSI 的 File 表以只读方式解析，恢复 DLL 真实文件名。

| 预设 | 组件及来源 |
| --- | --- |
| QAAC | [QAAC 3.07](https://github.com/nu774/qaac/releases/tag/v3.07)，x86；Apple CoreAudioToolbox 7.10.9.0 放在 `QTfiles` |
| LAME 五个预设 | 用户原先提供的 `lame-4.0-x86.zip`，保留现有文件 |
| Musepack 四个预设 | [项目 SV7 发布包](https://www.musepack.net/index.php?pg=win)，保留 `mppenc.exe` 和旧 `--xlevel` 参数，不替换为 SV8 |
| FAAC 两个预设 | [FAAC 2.1 MSVC](https://github.com/knik0/faac/releases/tag/faac-2.1)，x64；配套 `faac-1.dll`，需要 x64 Windows 的 VC14 运行库 |
| Vorbis 三个预设 | [foobar2000 Free Encoder Pack 2026-03-13](https://www.foobar2000.org/encoderpack) 的 x86 OggEnc2，仅将文件名映射为预设使用的 `oggenc.exe` |
| APE 四个预设 | [Monkey's Audio 13.26](https://www.monkeysaudio.com/download.html)，x86 `MAC.exe` + 同包 `MACDll.dll` |
| FLAC | [FLAC 1.5.0](https://github.com/xiph/flac/releases/tag/1.5.0)，x86 `flac.exe` + `libFLAC.dll` |
| Nero AAC 六个预设 | Nero AAC 1.5.4.0，来自下面说明的原始 `NeroAACCodec-1.5.1.zip` 存档 |
| True Audio | [项目 SourceForge](https://sourceforge.net/projects/tta/files/tta/ttaenc-win/)，`ttaenc-3.4.1.zip`，保留预设所需的 `-e -o` 接口 |
| TAK | [作者发布包](http://www.thbeck.de/Tak/Tak.html)，TAK 2.3.3 x86 `Takc.exe` |
| Opus | [Opus 官方下载页](https://opus-codec.org/downloads/) 链接的 Mozilla opus-tools 0.2 / libopus 1.3 x86 包 |

外部 EXE 的位数不必与 32 位播放器相同，但其自身依赖 DLL 必须匹配。此部署选择
FAAC x64，其余编码 EXE 为 x86；不能把 x64 FAAC 的 DLL 放进 QAAC 的 `QTfiles`。
QAAC 的 Apple 依赖来自 [Apple 官方 iTunes 12.10.11 x86 安装包](https://support.apple.com/en-us/106373)，
SHA-256 和 Apple 数字签名均已验证。默认 QAAC 预设仍为 CVBR 256，没有改为另一种 AAC 编码器。

## Nero 存档和许可

原始地址 `http://ftp6.nero.com/tools/NeroAACCodec-1.5.1.zip` 已返回 404。
按用户要求从 [Wayback 2016-02-22 05:42:50 快照](https://web.archive.org/web/20160222054250id_/http://ftp6.nero.com/tools/NeroAACCodec-1.5.1.zip) 取回。

- 大小：2,050,564 字节。
- SHA-256：`e0496ad856e2803001a59985368d21b22f4fbdd55589c7f313d6040cefff648b`。
- 与 [FreeBSD ports 2017Q2 的 distinfo](https://raw.githubusercontent.com/freebsd/freebsd-ports/2017Q2/audio/linux-neroaaccodec/distinfo) 完全一致。
- 包名为 1.5.1，`neroAacEnc.exe -help` 报告 1.5.4.0，不应因此误判下载错版本。

保留 `licenses/Nero/license.txt`。包内许可限制为个人非商业/技术评估用途，不授予
再分发权；这些本地二进制不能随社区版 Release 发布或提交到源码仓库。Apple 的
私有运行库也不应被当作开源组件随包发布。其它组件的再分发须分别核对原许可证
和源代码提供义务；本脚本用于本机按需取得，不构成统一再分发授权。

## 补齐组件时发现并修复的问题

1. `ttp_clienc!6020619C` 在 DLL 初始化时从主程序查找 `CreateStreamOnFile`。
   `%s` 临时 WAV 分支在 `6020147F` 检查该指针，为空立即返回 `80004005`；
   所以先前 APE、TTA、Opus 即使 EXE 齐全仍在 Start 阶段失败，尚未启动子进程。
   根据 `TTPlayer.exe.pseudo.c!004C51D3`，现在由主 EXE 导出同名 ordinal 3 的
   `stdcall(LPCWSTR, DWORD, IStream**)`，复用带完整 `Stat` 文件名的流实现。
   `602014D4` 的调用模式是 `0x1022`。没有修改任何原生 AddIn DLL。
2. 六个 Nero CLI 预设把 `-ignorelength` 误写为 `-ignorelenth`。安装脚本可只修复
   这些 Nero 节点，保留当前预设、输出参数和其它设置；构建复制默认 AddIn 配置
   后也做同一项修正，避免下次构建重新引入拼写错误。随后按用户要求直接修正了
   `../AddIn/ttp_clienc.xml` 源配置和 Debug 配置；Release 已是正确拼写。
3. 原有原生 Nero AddIn 的依赖加载和输出句柄修复仍有效。Nero CLI 和 Nero 6
   DLL AddIn 是两个独立实现，不能将其中一个的输入限制直接套用到另一个。

测试使用宿主机临时原版/重建版副本、生成的 WAV，不改用户音乐。脚本
`tools/probe_conversion_window.ps1 -Encoder 3 -CliPreset <0..28> -Convert -Complete -Seconds 3 -SampleRate 0 -Tone`
逐项驱动真实转换窗口。`-Tone` 为 44.1 kHz 立体声 440 Hz 测试信号；不以静音文件
或仅显示配置窗口作为成功编码的证明。测试源码/工具仍遵守仓库现有忽略规则。

最终结果：29/29 个当前命令行预设完成有声样本转换，全部测试播放器正常退出；
Release 的 30/30 项 CTest 通过（36.37 秒）。新增主 EXE 导出测试验证按名称查找、
`0x1022` 创建、读写/定位、`Stat` 路径和释放后的独占打开。

输出检查：21 个预设通过通用 PCM 探针的两种读块大小检查；另 8 个（Musepack 四个、
Nero 64K/Q0.28、TTA、TAK）在该探针中出现短读、静音或退出异常，不能称为通过。
这 8 个输出另外使用 `mppdec`、`neroAacDec`、`ttaenc -d`、`Takc -d` 独立解码，
均还原为完整 3.000 秒、44.1 kHz 立体声，归一化 RMS 0.25895–0.25930、峰值小于
0.386 的 PCM，证明转换文件本身有效。通用探针/播放器各解码路径的差异需要单独
排查；本次不把它描述为已修复，也没有通过更改预设编码格式来规避。
本轮也没有恢复另一个私有 `CreateStdContent` 导出，不能把这些 WAV 输入转换测试
作为全部命令行输出标签已与原版一致的证明。

构建前的 30 个运行配置已备份；除明确修复的 Nero 六个参数拼写外，其余配置
保持原值。备份位于 `build/external-encoders/runtime-config-before-build`。
