# 音频格式识别接口与错误后缀纠正实施记录

日期：2026-10-20。

## 1. 本次范围

接入播放器、ttpcomm SDK，以及 12 个独立插件：AAC、OGG、FLAC、APE、WebM、AC3/DTS、MOD、MPC、ASF、RM、TAK、VQF。

新建插件的完整工程说明见 [音频插件开发指南](AUDIO_PLUGIN_DEVELOPMENT_GUIDE.md)。协议规范在相邻 `ttpcomm/sdk/AUDIO_FORMAT_PROBE.md`，声明和共用实现以该 SDK 的头文件为准。

## 2. 原有问题与改动

旧宿主的 `RecoverAudioFormatHint` 复现 5.7.9 的有限提示规则。它没有统一识别 FLAC、Ogg 等全部格式；“MP3 内容配 `.flac` 后缀”也不属于其 MPEG 提示恢复范围。之后工厂主要按后缀筛选，而更名模块又单独识别一次，容易出现读取器和建议文件名依据不一致。

本轮保留旧 `RecoverAudioFormatHint` 及其历史测试，增加可选接口和宿主路由：

| 部位 | 改动 |
|---|---|
| Reader 工厂 | QI 获取 Probe；接收宿主提供的有界只读头部，返回候选格式及置信级别 |
| Reader | QI 获取 SourceInfo；真正 Open 成功后提供外层容器、编码、建议后缀和别名 |
| PluginManager | 不先按后缀排除新工厂；Strong、Tentative、旧后缀候选依次尝试，同级保持原顺序 |
| 播放/信息读取 | 共用 OpenReaderWithFlags 选择逻辑；保存成功读取器的结构化格式结果 |
| 内置文件路径 | MPEG、WAVE、AIFF、AU 等有明确内容证据时改善路由；成功打开后记录结果 |
| 播放时自动增益检查 | 从仅 HasReaderForPath 改为有界 HasReaderForContent，避免未知后缀提前排除插件 |
| 更名提示 | 优先使用成功读取器的结果，支持旧插件时仍保留已有识别后备 |
| 文件变化检查 | 保存打开前文件版本；提示前拒绝陈旧结果，实际更名继续核对源/目标身份、大小和修改时间 |

选择读取器时，宿主给工厂共用同一份快照；成功候选的 Reader 辅助实现也会单独保存自己的识别结果，不是宣称整条播放链只读取一次头部。

`Probe` 只有进程内头文件协议，没有新增 DLL 导出或注册表操作，也没有要求独立插件链接到 ttpcomm.dll。

## 3. 用户看到的纠正流程

### 普通音频

1. 添加文件时不弹更名窗口。显式添加的未知后缀文件仍可进入列表；读取信息时可按内容选择插件。
2. 请求播放后，保留实际文件名，通过内容识别排列工厂候选，再执行真正的 Open 和格式检查。
3. 成功播放后检查“选项 → 常规 → 选项 → 识别到错误后缀时提示更改”。关闭时照常播放，不更名。
4. 后缀为规范值或合法别名时不提示。MP4 接受 `.m4a`，WebM 接受 `.weba`，Opus 接受对应 Ogg 别名。
5. 后缀错误时显示识别格式、现有文件名、建议文件名和大小。取消不更名；“不再提示更改”关闭并保存总开关。
6. 若目标已存在，展示目标大小，选择覆盖或加编号。编号依次尝试 `(1)`、`(2)`……，继续跳过被占用名称；不会静默覆盖。
7. 确认后，必要时停止当前音频并释放句柄，再更名。同步更新各播放列表、媒体库和歌词关联中的引用。
8. 重新打开后尝试恢复确认时的播放位置与暂停状态。失败则显示具体错误，不把失败当作成功。

更名只改文件名，不转码、不重编码、不把封装内部的音频提取出来。单个假同步字、未成功打开或无法可靠确认的格式不会仅凭 Probe 结果提示更名。

同一会话内的已提示记录仍按原有来源/文件版本去重。若在弹窗期间目标文件出现，会重新显示冲突确认；若源文件发生变化，旧建议失效。

### 压缩包

归档更名保持已有独立入口和外层格式判断，不把成员的 FLAC/MP3 等结果用于外层包。包内文件继续只读，不写回。OSZ 作为 ZIP 衍生的谱面包保留 `.osz`，不会建议改 `.zip`。

文件夹递归导入和压缩包成员筛选仍按现有音频类型/插件扩展名规则，不为了探测未知成员而无条件解压整包。

## 4. 兼容性与保守边界

- 原版工厂、Reader、Decoder 的槽位和原导出不变；新接口通过独立 IID 获取。
- 原版播放器不会调用新 IID，因此更换插件本身不会给原版 EXE 自动增加新的错误后缀路由或更名弹窗。
- 新宿主遇到旧插件的新 IID 不支持时保留旧路径。没有结构化结果时，更名仍可使用已有受限检测。
- 共用采样通常 8 KiB，Ogg 标识页按需最多 64 KiB；合法 ID3 前导最多跳过 1 MiB。巨大标签、无签名旧 MOD、APL 等不强行推断。
- 不可定位或未知长度的流不执行新的前缀采样；保留现有网络流式路径。本轮没有声称增加所有直播流错误后缀识别。
- 原版 `CodecName`、压缩 Decoder 支持 GUID 和解码后的 PCM Format 不作为文件扩展名判断依据。

## 5. 构建与测试

### 构建

12 个独立插件 Release 构建全部通过；播放器 `ttplayer_rebuild` 和 3 个测试目标构建通过。XP/Win7 静态导入检查通过。

本地 CMake 构建缓存保留既有固定版本值：本轮插件大部分为 `2026.10.19`，WebM 为 `2026.10.06`。这是本地资源版本缓存，不表示代码未更新；各产物 SHA-256 在 `out/format-probe-20261020/final-binaries.json`。Action 仍在编译前分配其日期版本；本轮没有触发发布。

### 同一测试矩阵

| 环境 | 结果 | 内容 |
|---|---|---|
| Windows 11 本机 | 99/99 通过 | 自测 1、插件 ABI 12、真实音频用例 84、既有回归 2 |
| XP SP3 虚拟机 | 99/99 通过，FAILURES=0 | 同一产物、同一矩阵 |
| Win7 SP1 虚拟机 | 99/99 通过，FAILURES=0 | 同一产物、同一矩阵 |

84 个真实音频用例为 28 个样本 × 3 种后缀（原后缀、`.wrong`、`.mp3`）：AAC、MP4/AAC、Vorbis、Opus、FLAC、TTA 整数/浮点、APE、WebM/Opus、WebM/Vorbis、AC3、DTS 三种字节打包、MOD/XM/S3M/IT/MTM/STM/FAR/UMX、MPC SV7/SV8、ASF、RM、TAK、VQF。

每个真实文件检查：成功选择 Reader、返回 Opened 外层格式、读出非空 PCM、信息模式使用同一选择路径、合法后缀无建议、错误后缀的建议正确、建议不改文件、陈旧来源版本被拒绝。

ABI 自测包含新增/旧接口的规范 IUnknown 身份、原工厂 Create 槽、新接口的无效参数/版本、Open 前或 Open 失败后不能报告 Opened。纯识别自测覆盖短读、取消、位置恢复、恢复失败、未知长度、不可 seek、ID3 和连续 MPEG 帧。

额外验证 5 个**原版 DLL**（AAC/OGG/FLAC/APE/MPC）拒绝新 IID，保留旧工厂枚举。该检查属于原版 ABI 兼容检查，**不是本轮重新完成原版播放器 GUI 全流程验证**。

Windows 10 没有独立实机测试；沿用用户指定的 Windows 11 替代环境，不能据此声称在 Win10 实机执行过。

### 可复查证据

- 源码：`tests/audio/audio_format_probe_tests.cpp`、`format_probe_matrix.py`。
- Windows 11：`out/format-probe-20261020/bundle/results.json`。
- XP、Win7：`out/format-probe-20261020/XP-result.txt`、`Win7-result.txt`。
- 原版接口：`out/format-probe-20261020/old-abi-results.json`。
- 最终宿主构建：`out/format-probe-20261020/build-host-final-verified.log`。
- 各插件构建：同目录 `build-*-verified.log`；产物摘要：`final-binaries.json`。

测试仍在独立 `tests` 仓库；播放器测试目标受 `BUILD_TESTING` 和文件存在性保护。没有修改 Action 为运行测试，也没有提交或推送本轮测试。

## 6. 产物和部署状态

- 播放器已生成：`D:\Projects\Backup\TTPlayer\rebuild\build\Release\TTPlayerRebuild.exe`。
- 各插件已生成：`D:\Projects\Backup\TTPlayer\ttp_<名称>\build\Release\ttp_<名称>.dll`。
- 完整已测试插件集合：`D:\Projects\Backup\TTPlayer\rebuild\out\format-probe-20261020\bundle\AddIn`。

**已完成部署（2026-10-20 后续验证）：**此前自动审批拦截过覆盖命令，返回 `blocked by policy`。用户明确授权覆盖后，现已将 12 个带新识别接口的插件，部署到 `build\Release\AddIn`。13 个 DLL 的部署 SHA-256 均与对应独立构建产物一致。

旧 DLL 备份目录：`out/format-deployment-20261020/backup-20261020-050750`。逐文件源路径、目标路径、原摘要、新摘要及大小保存在 `out/format-deployment-20261020/deployment.json`。没有发布 GitHub Release，也没有替换原版目录的 DLL。

## 7. 后续扩展顺序

1. 新插件优先自行实现本协议，用真正解析器的确认结果补齐 SourceInfo，不继续把所有新格式判断堆进播放器。
2. 需要时为更多无签名/大前导格式设计专用有界 Probe；不能用更大的无限扫描代替预算和取消机制。
3. 网络直播若要按内容选取任意插件，应在现有可回放前缀机制上显式传递快照；不要直接消耗不可恢复的 IStream。
4. 再单独评估目录/归档未知后缀导入策略的性能和交互；它与播放时更名是两件事。
5. 独立插件 release 发布后，现有播放器 Action 的最新稳定插件下载流程才会取得新 DLL；本地源码完成不等于远端发行已更新。

## 8. 部署后的实际播放和更名提示验证

### Windows 11 实际播放器界面：10/10

使用与发行目录摘要一致的 EXE、核心 DLL 和完整 AddIn 副本，在独立目录运行；设置和音乐均为测试副本，音量静音。通过实际窗口消息选择按钮，记录对话框文字、截图、文件摘要及退出时保存的播放路径/进度。

| 用例 | 验证结果 |
|---|---|
| FLAC 内容、`.mp3` 后缀 | `/a` 仅添加时不提示；点击播放才提示；确认改为 `.flac`，恢复播放 |
| MP3 内容、`.flac` 后缀 | 正确建议 `.mp3`；取消后原路径继续播放 |
| Opus 内容、`.mp3` 后缀 | 正确建议 `.opus`；确认更名并继续播放 |
| 重名加编号 | 展示源/目标大小；已有目标及 `(1)` 时使用 `(2)`，已有文件摘要不变 |
| 重名覆盖 | 点击“覆盖”后目标内容与源音频一致，原错误文件名消失 |
| 不再提示更改 | 当前文件不改名，仍播放，配置持久化为 `PromptExtensionCorrection="0"` |
| 选项关闭 | 错误后缀照常播放且不弹提示、不更名 |

10 项退出时记录的播放位置均大于 1.3 秒；更名项保存的正在播放路径已切换到目标路径。所有样本文件字节 SHA-256 保持一致，更名没有修改音频内容。这些实际弹窗测试在 Windows 11 执行，XP/Win7 本轮执行的是解码/接口/更名逻辑回归，未重复 GUI 点击流程。

### 复查及复现

- 自动测试：`out/format-deployment-20261020/deployed-final/results.json`。
- XP/Win7：同目录上一级 `XP-final-result.txt`、`Win7-final-result.txt`。仅使用 `*-final-result.txt`，早期覆盖复制日志可能残留旧尾部，不作为最终证据。
- GUI：`out/format-deployment-20261020/ui-verified/results.json`；各用例目录有 `prompt.png`（有提示时）、`player.png` 和 `result.json`。
- 汇总：`out/format-deployment-20261020/verification.json`。
- 测试脚本：`tests/audio/format_probe_matrix.py`、`tests/audio/format_deployment_ui.py`。留在 tests，本轮未提交或推送，Action 不运行。

```powershell
python tests/audio/format_probe_matrix.py --prepare --deployed --bundle out/format-deployment-retest
python tests/audio/format_probe_matrix.py --bundle out/format-deployment-retest
python tests/audio/format_deployment_ui.py --output out/format-deployment-ui-retest
```

GUI 脚本需要没有正在运行的播放器，使用新的输出目录防止覆盖上次测试证据。Windows 10 仍没有独立实机验证，Windows 11 按用户要求作为替代环境。
