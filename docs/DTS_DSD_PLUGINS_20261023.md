# WAV 杂音修复与 DTS / DSD 插件

## 结论

没有取得网友原始 WAV，因此不能断言其文件一定是 DTS-WAV。生成样本已证明两个真实问题：PCM 头包装的 DTS 被内置 WAVE Reader 当作 PCM 输出；WAVEFORMATEXTENSIBLE 的声道掩码和有效位数在宿主中丢失。两条路径均已修正。

## 改动

1. 新建独立 `ttp_dts`、`ttp_dsd` 仓库，按音频插件开发指南保留原版接口，提供固定 FFmpeg 后端、XP/Win7 构建、日期版本、独立 Action、源码附件和仓库许可证。
2. `ttpcomm` 格式 SDK 增加 DSD 家族和 DTS-WAV 内部帧识别；连续两帧及一致的采样率/声道信息才作为识别依据。同步到宿主及现有插件的头文件副本，不需要新增 ttpcomm DLL 导出。
3. DTS-WAV 保留 WAVE 容器身份，不提示错误改成 `.dts`。Reader 查找、错误后缀、长 JUNK 块路径已验证。没有可用解码器时停止，避免继续交给 PCM/系统回退路径输出压缩字节。
4. WAVE Reader 保留完整扩展格式，支持 24 有效位装在 32 位容器中。PCM 转换新增完整输出格式，DirectSound/WaveOut/WASAPI/KS 传递声道掩码；ASIO 接受规范扩展 PCM 描述。多声道 DirectSound 不请求不适用的声像控制标志。
5. 新增 `.dsf`、`.dff`、`.dsdiff`、`.dtshd` 音频扩展名。DSD 转成 176.4/192 kHz PCM，接入现有均衡器与输出；没有修改 Winamp 插件的 16-bit 接口限制。

## 验证与限制

- 24 种普通 WAV（2/6 声道，16/24/32 位、24-in-32、float32/64，44.1/96 kHz）解码数值与源数据一致。
- DTS 16/14-bit、BE/LE、WAV/raw、重命名、长 JUNK、缺失插件、偶然同步字；DSF DSD64/128/256、DSDIFF、中文标签/PNG、定位/EOF/取消及损坏输入。
- XP / Win7 上实际执行两个 DLL，DTS-WAV 与六声道 DSF 输出均和 Win11 逐字节一致。Win11 代现代 Windows 验证，未声称实际运行 Win10。
- 本机原版和重建版隔离启动/插件加载 smoke；输出相关 4 组回归通过。没有真实六声道设备的听音验收，也没有网友原文件。
- DTS-HD、DST、DSD 48 kHz 系列还需真实样本验证；DSD 原生直通/DoP、SACD ISO、标签写回不在本次实现范围。向后定位目前重放历史，后续可优化索引。

详细实现见 `ttp_dts/docs/IMPLEMENTATION.md` 和 `ttp_dsd/docs/IMPLEMENTATION.md`。所有测试源码、样本及报告留在 `rebuild/tests/wav_feedback_20261023/`；Action 不运行测试。插件尚未发布远程仓库/Release，未向现有在线 CodecAddIn 聚合填入无法验证的仓库地址。

发行输出：`rebuild/build/Release/`，播放器和插件分开打包；手动关闭播放器后覆盖 EXE，并将插件 DLL 放进 `AddIn/`。
