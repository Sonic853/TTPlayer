# WAV 杂音修复与 DTS / DSD 插件

## 后续整合（2026-10-23）

按用户确认，将现代 DTS 后端整合入 `ttp_ac3dts.dll`，保留四工厂和 AC3，移除 libdca 链接。随后按进一步要求完成边界 3～5：追加 E-AC3 Reader、DTS Core 稀疏准确定位、WAVE 标签／封面写入，并修复 XP DLL 初始化入口和混合标签删除回退。独立 `ttp_dts` 已从工作区根目录及 Release 移入私有可恢复归档；当前只分发 `ttp_ac3dts`。下文中两份 DTS 插件同时安装的冲突结论属于历史状态。

整合后在 XP／Win7／Windows 11 的原版 5.7.9 与重建播放器中验证 AC3、原始 DTS 和 DTS-WAV 插件路径，解码／定位／小缓冲及标签生命周期检查通过；修正旧 14 位 DTS 帧边界和载体时长估算。原版 EXE 对 `.wav` 的抢先选路限制仍保留，完整 DTS-HD 扩展层仍待真实样本验证。详见相邻 `ttp_ac3dts/docs/DTS_INTEGRATION_20261023.md`。

最新补全及验收记录：[边界 3～5](../../ttp_ac3dts/docs/BOUNDARIES_3_5_20261023.md)。测试保留在 `rebuild/tests/ac3dts_complete_20261023`，不提交、不进入 Action 或发行包。

## 结论

没有取得网友原始 WAV，因此不能断言其文件一定是 DTS-WAV。生成样本已证明两个真实问题：PCM 头包装的 DTS 被内置 WAVE Reader 当作 PCM 输出；WAVEFORMATEXTENSIBLE 的声道掩码和有效位数在宿主中丢失。两条路径均已在重建版修正；原版 EXE 的内置 WAVE 选路不会随插件安装自动改变。

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

## 原版是否能通过此链路播放：补验结果

原版 5.7.9 的旧 Reader 接口可直接接收插件解码的 PCM。`004CBC1F` 选择 Reader，`004B0947 / 004B0966 / 004B0999` 识别 PCM 或 IEEE_FLOAT（含 EXTENSIBLE），`004B0D1A / 004B1375` 取得并读取音频。不存在“必须先有原版 DSD/DTS Decoder，才能使用直接输出 PCM 的 Reader”的限制。

本轮在原 EXE、原 ttpcomm / ttpres 和隔离配置中，用私有转发探针记录发行插件实际 Open / Format / Read。测试静音，未改原 EXE 或发行 DLL。结果：

| 输入及组合 | 原版的实际行为 |
| --- | --- |
| 裸 DTS，仅新 DTS 插件 | Read 返回 44.1 kHz / 六声道 / float64，播放推进超过 3 秒 |
| DSF / DFF | Read 返回 176.4 kHz / 双声道 / float64，播放推进超过 3 秒 |
| 常规 PCM 头包装的 DTS `.wav` | 内置 WAVE Reader 先接受；新插件只被枚举，Reader 没有 Create / Open / Read。进度推进不能证明正确解码 |
| 同内容的 `.dtswav` 测试副本，仅新 DTS 插件 | 新插件被调用并返回正确解码的六声道 PCM |
| 同时安装原版或重建 `ttp_ac3dts.dll` | 本次组合中 `.dts` / `.dtswav` 先走既有插件，新 DTS Reader 未被调用 |

原版 WAVE 函数 `004E82CA` 的反编译存在错误删去分支的现象；已核对保留 REA 汇编中 `004E83AD` 开始的数据接受路径，并与运行证据交叉验证。不能据伪代码中单独的失败返回判断原版不支持 WAVE。

因此，**新 DLL 的 ABI 与原版兼容，但“所有扩展名均自动由新插件正确解码”尚不成立**。要在原版保留 DTS-WAV 的 `.wav` 并自动正确解码，需要处理宿主的内容识别和选路；还应处理既有 DTS Reader 的优先级，同时保留 AC3 功能。重建版的 Probe 路由、缺失插件保护和新 PCM 模式不会通过替换插件或 ttpcomm 自动移植进原版。

原版默认输出设置仍是 16 位，Winamp 链路（`004AC166`）也仍经过 PCM16；Reader 返回 float64 不等于声卡端保留同等精度。实际多声道布线、高采样率设备及原版 XP/Win7 完整 GUI 输出尚需另验。此前 XP/Win7 的 DLL / 旧 ABI 解码通过与本机原版宿主补验分别记录，不混称为同一项验证。

详细实现见 `ttp_dts/docs/IMPLEMENTATION.md` 和 `ttp_dsd/docs/IMPLEMENTATION.md`。所有测试源码、样本及报告留在 `rebuild/tests/wav_feedback_20261023/`（补验为 `original-chain-results.json`、`original-coexist-results.json`）；Action 不运行测试。插件尚未发布远程仓库/Release，未向现有在线 CodecAddIn 聚合填入无法验证的仓库地址。

发行输出：`rebuild/build/Release/`，播放器和插件分开打包；手动关闭播放器后覆盖 EXE，并将插件 DLL 放进 `AddIn/`。
