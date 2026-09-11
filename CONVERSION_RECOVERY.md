# 播放列表“转换格式”恢复记录

2026-09-11。入口：歌曲列表右键 → 转换格式（命令 `0x7EF7`）。
本次恢复可执行的转换流程，不把反编译 C 文本等同于可逐字重编译的原始源码。

后续更新：外部编码器已补齐，当前 29 个命令行预设均完成有声样本转换。
同时补上 `%s` 临时 WAV 所需的主 EXE `CreateStreamOnFile` 导出，修正 Nero CLI
六处 `-ignorelenth` 拼写。Nero 包来自原厂链接的 Wayback 快照且匹配历史 SHA-256。
来源、部署、许可证和输出解码验证边界见 [EXTERNAL_ENCODERS.md](EXTERNAL_ENCODERS.md)。

## 伪代码对照

| 原程序地址 | 行为 | 重建位置 |
| --- | --- | --- |
| `004CA48E` / `004CA3BE` | 先注册内置 Wave，再枚举 AddIn 编码器 | `playlist_transforms.cpp` 的编码器目录；`file_encoder.cpp` 的 Wave 输出 |
| `0047D682` | 资源 220：编码器、位深、13 档采样率、线程优先级、文件存在策略及处理开关 | `PopulateConvertConfiguration` |
| `0047D9C7` / `0047D9A4` | 只有 Wave 可选位深；配置按钮取 creator 能力并调用其 Configure | `UpdateEncoderConfigurationButton`、`ConvertConfigProc` |
| `0047DA15` / `0047DA59` | 采样率勾选联动、保存 Convert 配置 | `ConvertConfigProc`、`CommitConvertConfiguration` |
| `004122CD` / `004B0D1A` | 解码、重采样、ReplayGain/EQ/环绕；非 Wave 使用 double PCM；Wave 按 reader 报告的原始位深量化 | `ConvertPlaylistTrack`、`PcmOutputTransform` |
| `004CD30C` | 优先 stream encoder 接口，缺失时回退路径 Open | `LegacyEncoderSession::Open` |
| `004122CD` | encoder 实例 slot 7 决定实际输出扩展名，不能只使用 creator 的格式声明 | `LegacyEncoderSession::FileExtension` |
| `004121C4` | 源/目标都有 metadata 接口时复制非空字段，排除 `replaygain_*` | `LegacyEncoderSession::SetMetadata` |
| `00412723` / `004128D4` | STA 工作线程依次 Start、Write(buffer)、Finalize；按 PCM 字节计算进度 | `RunConversion`、`ConvertPlaylistTrack` |
| `00412A8D` / `00412AB8` | 进程内单例、非模态进度窗口，再次打开激活原窗口 | `ShowPlaylistConverter` |
| `00412B48` / `004AE7FD` | 资源 221、四列、播放列表格式化标题、图标及暂停文字 | `PopulateConvertProgress`；主窗传入 `PlaylistDisplayText` |
| `004132B8` / `00411ECD` | 状态列蓝白进度及反色百分比 | `DrawConversionProgress` |
| `00412E61` | 每首成功后按选项添加到播放列表；全部成功关闭，失败保留结果 | `ConvertProgressProc` |
| `00413253` | 暂停/继续；终止结束本批工作 | `ConvertProgressState::Checkpoint/Cancel` |
| `004E8420` / `004E8500` | PCM RIFF/WAVE 头及最终文件长度回填 | `FileEncoder` |

窗口、标签、列名、状态文字及原有按钮位图来自运行目录下 `ttpres.dll` 的
220/221 模板、字符串表和位图 1、2、1023、352、356。Wave 名称用字符串 `0x811A`。
原有六项的排列为 Wave、Nero HE-AAC、APE、命令行、LAME 3.90.3、WMA。
优先级保存真实 Win32 值 `2 / 0 / -15`，不是下拉列表索引。

支持原目录/指定目录输出、`%03d.` 自动编号、跳过/询问/覆盖、转换后添加、
多选顺序转换、CUE 子曲目及现有解码源。采样率转换之后才量化给整数编码器，
不会先降为 16 位再交给需要 float64 的旧插件。

## 用户提供的 LAME 4.0 压缩包

现有 `ttp_enc.dll` 内含 LAME 3.90.3，不会自动使用旁边的 `lame_enc.dll`。
所以保留其原条目，另加 **MP3 (LAME DLL)**，不冒充原有编码器。

- CMake 按目标位数读取 `lame-4.0-x86.zip` / `lame-4.0-x64.zip`。
  当前整个播放器受旧 AddIn ABI 限制仍是 **Win32/x86**，因此实际使用 x86 包；
  没有宣称恢复 x64 播放器。
- DLL 格式使用 `lame_enc.dll` 与包内必需的 `libmpg123-0.dll`，不启动 `lame.exe`。
  后续命令行预设恢复另在 `Encoders` 部署可选 `lame.exe` 和其依赖；不构成播放器
  启动、播放或 DLL 转换的 EXE 依赖，不覆盖已安装的同名命令行程序。
- 只加载运行 EXE 同目录的 DLL；DLL 和所需导出可加载时才出现该条目。
- 支持 CBR、VBR、ABR，码率、VBR 品质、单/双声道、最终 flush/InfoTag 以及 MP3 标签。
  XML 使用 `Convert/@LameMode`、`LameBitrate`、`LameQuality`。
- 使用 LAME 项目的 [BladeMP3EncDLL.h](https://raw.githubusercontent.com/lameproject/lame/master/Dll/BladeMP3EncDLL.h)
  和 [DLL 实现](https://raw.githubusercontent.com/lameproject/lame/master/Dll/BladeMP3EncDLL.c)
  所定义的 packed 331 字节配置与 cdecl 调用约定。
  `beWriteInfoTag` 自行关闭句柄，不能再重复 `beCloseStream`。
- 修复内置 MP3 信息读取未使用 Xing/Info/VBRI 帧数的问题，避免 VBR 输出按首帧码率估算出错误时长。

## 2026-09-11：编码器配置窗口“卡死”修复

原版 `0047D9A4` 读取当前编码器索引，调用 `004CD21E`；后者取得 creator，
同步调用 vtable `+0x1C`（slot 7），传入转换设置窗口 HWND，返回后 Release。
重建版 `ConfigureEncoder` 的接口槽位和父窗口已经对应这一流程，无需把插件
配置挪到工作线程，也不能把正常的模态消息循环误判为编码阻塞。

实际故障发生在原 DLL 创建配置窗口之后：`ttp_ape.dll!60101C36` 和
`ttp_enc.dll!60251FA8` 构造旧 ATL 的 13 字节窗口 thunk
（`C7 44 24 04 <this> E9 <relative-target>`），存放在普通读写堆内存中。
宿主机调试捕获到 APE 的执行访问异常 `C0000005`、访问类型 8、页面保护
`PAGE_READWRITE`，随后 `C000041D` 并进入 Windows 错误报告等待。此时对话框
已创建但尚未显示，父窗口被模态禁用，所以表现为整个转换设置“卡死”。

原 EXE 未声明 NX compatibility；现代工具链默认给重建 EXE 添加了该标记，
从而禁用了 Windows 的旧 ATL thunk 仿真。修复配套执行：

- 仅播放器目标链接 `/NXCOMPAT:NO`，保留 ASLR 等其它选项；
- `wWinMain` 在载入插件、分派私有 worker 前调用
  `SetProcessDEPPolicy(PROCESS_DEP_ENABLE)`，显式保持 DEP 并允许系统 ATL 仿真；
- 不修改原 DLL、不把堆改成可执行、不修改系统/注册表 DEP 策略。

机制参见微软的 [DEP 与 ATL 仿真说明](https://www.microsoft.com/en-us/msrc/blog/2009/06/understanding-dep-as-a-mitigation-technology-part-1/)
和 [SetProcessDEPPolicy 文档](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setprocessdeppolicy)。
系统 AlwaysOn、AlwaysOff 或外部强制的进程策略优先，不能宣称覆盖这些策略下的兼容性。

宿主机临时副本验证：原有五个插件配置及新增 LAME 配置均能显示、响应、正常关闭；
Wave 配置按钮仍按原版禁用。重建版在同一进程做三轮共 18 次配置打开，分别用关闭、
确定、取消返回；每次均恢复转换设置父窗口，最终正常退出，无强制终止。
运行时核验 `GetProcessDEPPolicy` 为 flags=1、permanent=TRUE（DEP 开启，ATL 仿真未禁用）。
脚本：`tools/probe_conversion_window.ps1 -ConfigureAll -ConfigurePasses 3 -Seconds 1 -SampleRate 0`。
新版 Release 的全部 30 项 CTest 通过（37.67 秒）。这验证配置窗口，不代表所有预设均可实际编码。

## 2026-09-11：Nero 实际编码与全部输出格式复查

### 已修复的两个独立故障

1. `ttp_aac.dll!60003CC7` 按名称加载 `Aac.dll`，后者依赖 `aacenc32.dll`，
   编码启动时再延迟加载 `NeroIPP.dll`。现有文件都在 AddIn，但旧 Nero 注册表/
   搜索路径逻辑仍会返回模块错误 126。现在在创建 Nero encoder 或打开配置前，
   按该插件所在 AddIn 目录的绝对路径依次加载三份现有组件；不查版本、不更改
   注册表、不依赖当前工作目录或另一份安装。后台 library snapshot 共享组件
   生命周期，释放 encoder/creator/AddIn 后才释放所持组件引用。缺少组件时返回
   具体路径与 HRESULT，配置窗口不会再无声失败；原 Nero 取消配置返回的失败值
   仍按 `0047D9A4` 忽略，不能误弹错误框。
2. `00412575` 在编码 Open 前删除旧目标；重建版却预建空 `.ttconvert-*.m4a`。
   跟踪实际 `CreateFileA/CloseHandle` 证实 Aac+`5F37B -> 61D3C` 先读这个空文件，
   解析失败后遗留一个共享模式 3、不允许删除的读句柄。随后 writer 正常写入、
   Finalize 正常关闭，但该读句柄使提交返回 `80070020`。现在以独占临时目录
   预留名称，交给编码器的是不存在的 `output.<ext>`，恢复原版的文件前置条件；
   编码成功后同卷替换真实目标，失败/取消清理临时文件及目录。不遍历强关未知
   句柄，不卸载正在使用的 DLL，不删除已有目标来掩盖错误。

### 宿主机转换对照

以下均使用临时程序副本及生成的 3 秒 WAV，不操作用户音频。原版 Nero 对照
仅在临时副本给 EXE 旁补一份已有 NeroIPP（原程序自身也受旧搜索问题影响）；
重建版全部三份组件始终只放在 AddIn。命令行对照只使用用户已有的 LAME ZIP。

| 输出格式 | 原版 | 修复后的重建版 |
| --- | --- | --- |
| Wave | 成功 | 成功；本样本与原版文件字节一致 |
| Nero HE-AAC | 补齐本地搜索后成功 | 成功；无需旁置 DLL；两首批量转换及加入列表通过 |
| APE | 成功 | 成功；本样本与原版文件字节一致 |
| 命令行 LAME 五个预设 | 全部成功 | 全部成功：CBR 128/320、standard/extreme/insane |
| 内置 LAME 3.90.3 | 成功 | 成功；本样本与原版文件字节一致 |
| WMA | 成功 | 成功；输出解码及标签检查通过 |
| 新增 LAME DLL | 无此新增条目 | 窗口转换及 CBR/VBR/ABR 音频测试通过 |

`conversion_recovery_tests` 已将 Nero 纳入真正编码/解码测试（不再跳过）：
正弦 PCM 的时长/音量、Title/Artist/Album、输出的独占读写删除权限、重复覆盖、
取消保护、后台 snapshot、中文路径、48 kHz 转换、192 kHz 拒绝及临时目录清理。
缺少本地 NeroIPP 的独立 AddIn 副本必须失败，即使进程里已加载另一份 NeroIPP。

最终 Release 回归：全部 30 项 CTest 通过（32.43 秒），六种编码器配置窗口的
关闭/确定/取消三轮共 18 次打开均通过，DEP 保持开启。构建后已恢复测试前备份的
30 个运行配置文件并校验哈希一致，临时宿主机播放器进程均已退出。

### 本轮复查时缺少的外部命令行组件（后续已补齐）

以下是补齐组件之前的历史结果；当前状态见 EXTERNAL_ENCODERS.md。

XML 共 29 个预设，已提供的 LAME ZIP 可补齐其中 5 个。其余 24 个依赖以下
10 份尚不存在的可选 EXE：`qaac.exe`、`mppenc.exe`、`faac.exe`、`oggenc.exe`、
`mac.exe`、`flac.exe`、`neroAacEnc.exe`、`ttaenc.exe`、`Takc.exe`、`opusenc.exe`。
已对照缺少 QAAC 的原版/重建版：原 DLL 显示执行失败消息，确认后状态列为“出错”，
进程均能正常关闭；不是未释放文件句柄导致的提交错误。没有下载这些组件，也没有
把 QAAC/AAC 等预设静默替换成 MP3。可在命令行编码器的配置中选择已可用的 LAME。

Nero 大于 48 kHz 输入的 `8BDA0602` 是原 `ttp_aac` 的明确拒绝，两版本窗口测试
均为“出错”；需要用户勾选重采样至 44.1/48 kHz，不能把这一支持范围说成已扩展。

## 已验证与边界

宿主机 Release 构建，30 项 CTest。新增 `conversion_recovery_tests` 验证：

- Wave 原始/8/16/24/32 位输出、SSRC 重采样、进度、CUE 子曲目；
- Wave/APE 解码后 PCM 与输入一致；MP3/WMA 检查解码时长及 RMS，避免静音或倍增/减半音量；
- 原有 APE、LAME 3.90.3、WMA 实际编码；新增 LAME CBR/VBR/ABR、单声道、中文路径、标签；
- 同文件/硬链接/CUE 源文件保护；取消不破坏已有文件；覆盖询问的接受与拒绝；临时文件清理。

`tools/probe_conversion_window.ps1` 在临时目录各自复制原版/重建版，在宿主机使用真实
窗口消息操作，验证原有编码器顺序和启用状态、暂停/继续、终止、主窗口可操作、
单例窗口和两首歌曲编号/完成后加入列表（2 首变 4 首）。所有媒体为生成的临时测试文件。
测试脚本和测试源在当前仓库规则下被忽略，没有擅自改变 `.gitignore`。

不能将下列事项表述成“全部逐字/逐像素一致”：

1. Nero 依赖加载及输出句柄占用已按上节修复，但未穷举 Nero 的全部质量/容器预设。
   `ttp_aac.dll` 明确拒绝大于 48 kHz 的输入，192 kHz 失败不代表组件缺失。
2. 命令行编码器仍调用原 AddIn 的配置/执行接口。当前本机部署已补齐 QAAC 等
   外部组件并验证 29 个预设；独立 EXE/私有运行库不是源码的一部分，也不代表可
   随社区版统一再分发。新的 `%s` 路径依赖已恢复的主 EXE 导出，须使用新版 EXE。
3. 所附 APE 与 LAME 3.90.3 **encoder** 的 metadata QI 返回 `E_NOINTERFACE`。
   因而和原伪代码一样不强制补写标签。WMA 支持该接口，已验证 Title/Artist/Album；
   新 LAME 条目则使用内置 MP3 写标签实现。没有加入原代码未复制的专辑封面。
4. 文件夹选择保留此前要求的现代文件对话框。按钮使用原位图配合系统控件，未重建
   原私有按钮类的全部主题绘制/布局，因此不能保证所有 DPI 下逐像素一致。
5. 原版用 SuspendThread/ResumeThread，重建版使用 PCM 块边界的协作暂停，避免冻结
   持锁线程。原生 DLL 内部永久阻塞仍无法通过协作取消强行打断。
6. 原版覆盖前直接删除目标；重建版先编码到同目录下的独占临时目录，成功后替换目标，
   避免失败/取消毁掉已有文件，并禁止覆盖输入及其硬链接。这是刻意的安全差异。
7. Blade InfoTag 是 ANSI 路径接口；已使用可逆短路径和系统代码页转换，不做有损 `?`
   替换。若卷禁用短文件名且路径无法由系统代码页表示，返回明确失败，不提交残缺文件。
8. 物理 CD 抓轨、各编码器的全部参数组合及全部输入格式没有逐项实机验证。
   本次 29 个预设的短样本转换通过，不代表通用解码探针的全部路径通过；具体
   短读/HE-AAC/TTA/TAK 探针异常及独立解码结果见 EXTERNAL_ENCODERS.md。

部署时使用新的 `build/Release/ttplayer_rebuild.exe`，保留同目录 `ttpres.dll`、
`ttpcomm.dll`、`AddIn`；若使用新 LAME 条目，同时携带两份已部署的 LAME 依赖 DLL。
