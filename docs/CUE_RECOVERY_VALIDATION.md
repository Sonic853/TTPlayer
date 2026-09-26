# CUE 修复与验证记录

日期：2026-09-27。

## 修复结果

按 [原版分析](CUE_IMPLEMENTATION_ANALYSIS.md) 补齐了导入、分段播放、信息读取、
文件属性编辑和 CUE 文本回写，并接通原先拒绝 CUE 的 ReplayGain 扫描路径。

### 解析与编号

- 子曲目统一为解析后的顺序序号；源 TRACK 数字单独保留。
- 保存 INDEX 00、INDEX 01、FILE 原文、单轨 REM 和完整原始文本行。
- 无 INDEX 的轨道不加入数组；仅 INDEX 00 可保留，但不会错误地从零开始播放。
- 支持多 FILE；相邻 FILE 按原文字串不区分大小写比较。相等／倒序起点不会
  建立零长或负长的下一轨边界，而使用实际音频总长度后备。
- TITLE、Artist、Album、Tracknumber、REM 组成独立标签集合，不继承整张底层
  音频的曲名、轨号或 ReplayGain。
- TTBL/XML 读取尝试迁移旧重建版的源 TRACK 编号：只接受缓存标题明确匹配，
  或原编号已超出顺序范围且源轨号唯一的情况。重复编号或缺少区分信息时无法
  无歧义地推断旧条目，保留合法顺序编号；这类旧列表应重新导入 CUE。

### 播放、定位与文件查找

- 播放和后台信息读取共享 `AudioCandidates()`：FILE 原路径、CUE 同名原扩展名、
  同名 APE、同名 TAK；直接引用其它格式仍由普通解码路径处理。
- 起止位置分别对齐到完整 PCM 帧后相减，避免低采样率下分段长度取整丢失样本。
- Seek 使用毫秒到采样帧的转换，不再先量化到 75 Hz；拒绝负值及结尾／越界定位。
- 旧插件实际 Seek 返回值通过源接口保留，用于计算需要补读的字节数；返回位置晚于
  目标时尝试从零补读，无法到达目标前方则报错。
- 缺 INDEX 01、起点超出已知音频长度、部分 PCM 帧和空的非终止缓冲都有错误处理。

### 文件属性与保存

- 文件属性子进程增加 `cue-read`、`cue-write` 模式，传递目标子曲目。
- 标准及高级标签编辑限制不可写字段；Album 保存后刷新同 CUE 的其它列表条目。
- 修改 Title、Artist、Genre、Date、Comment、ReplayGain、自定义合法 REM 等字段时，
  只合并修改项，不把子曲目的标签写入底层音频文件。
- 保存保留 FILE、源 TRACK 文本、INDEX、FLAGS 等未知指令和换行。原编码可表示
  修改内容时保持编码；ANSI 无法表示新增字符时改用 UTF-8 BOM，避免静默丢字。
- 只读及包内文件禁止写入。写入使用同目录临时文件、刷新、重新解析校验、原内容
  比较和 `ReplaceFileW`。本程序的多进程写入以同一路径互斥；过期快照返回冲突。
- 针对短暂共享／删除占用进行有限替换重试；不退回直接截断原文件。
- 原版拒绝修改 Tracknumber 和 Lyrics 的约束保留；不提供 CUE 内嵌封面写入。

### ReplayGain

- 手动扫描解码所选子曲目，结果写入该轨的 REM 标签，支持跳过已有增益。
- 播放结束后的后台提交携带子曲目序号；不同子曲目的待提交任务不会互相去重。
- 手动扫描沿用既有清除只读属性策略；自动扫描保存仍保留只读属性并拒绝写入。

## 验证

测试源码、样本、构建项目和日志均只放在本地 `tests/`，未加入发行包；未修改
GitHub Actions 或打开发行构建的 BUILD_TESTING。现有 `audio_recovery_tests`
使用原安装目录运行，退出码为 0。

同一 CUE 测试程序在本机 Windows、Windows XP 和 Windows 7 SP1 中均通过：

| 验证项 | 内容 |
| --- | --- |
| 解析与编号 | 非连续／重复 TRACK、INDEX 00/01、缺 INDEX、REM 作用域、多 FILE |
| 分段 PCM | 8/44.1/48/96 kHz，各 3 轨，共 12 个区段，与生成的参考数据逐字节比较 |
| 定位 | 12 次非 CUE 帧整数位置定位后的剩余数据逐字节比较；结尾定位拒绝 |
| 异常边界 | 相等、倒序及 FILE 切换使用底层总长；仅 INDEX 00 不播放 |
| 后备 | 引用文件不存在时，同名原扩展名及 APE 候选成功打开 |
| Unicode | UTF-8 无 BOM／有 BOM、UTF-16 LE／BE 的编辑、保存、重新读取 |
| ANSI | 系统 ANSI 保留及无法表示新增 Unicode 字符时无损升级 |
| 保存保护 | 多轨连续编辑不丢前轨修改；过期快照、只读文件、禁止字段拒绝写入 |
| 网上 CUE | 11 份公开文本解析、编辑、重新读取后，轨道数、INDEX 00/01、FILE 不变 |
| 实际 FLAC | 网上一份 10 轨 CUE 配合本地生成的同名 FLAC，逐轨打开、定位和读取 |
| 增益扫描 | 所选区段实际样本数、标签提交、其它轨不受影响、跳过已有增益 |
| 属性进程 | 独立子进程读取 CUE 属性、播放列表后备路径、编辑并保存第二轨 |
| 旧列表 | 旧 TTBL 源 TRACK 编号迁移为顺序编号 |

本地日志：`tests/cue-test-results.log`、`tests/cue-win7-results.log`、
`tests/cue-xp-results.log`、`tests/cue-existing-results.log`。

Release 构建通过；静态导入检查通过（20 个 DLL、667 个导入）。生成文件：
`build/Release/TTPlayerRebuild.exe`。本轮校验 SHA256：
`FE29EFF2034EDCE0C92CA7E09BF45B1A7DFF64AC4BEA129B9B318D49B676F9AF`。

## 网上样本来源与使用方式

来源为 [justlaputa/cue-parser 的 CUE 测试目录](https://github.com/justlaputa/cue-parser/tree/master/test/cue)，
覆盖 EAC、XLD、LF／CRLF 和多文件样式。下载的原始文本保留在 `tests/cue-online/`，
下载地址及各文件 SHA256 记录在该目录的 `provenance.json`。编辑验证使用副本。

10 轨 FLAC 验证使用该目录的 `Michael Kiwanuka - Love And Hate.cue`。配套 FLAC 是
本地 FFmpeg 生成的 44.1 kHz、双声道、16 位静音音频，长度 3020 秒；没有下载或使用
该专辑录音。此项验证真实 CUE 时间表与 FLAC 解码组合，其定位内容精度另由非静音
确定性 PCM 样本验证。

## 保留的改进与验证边界

保留正确的无 BOM UTF-8 处理、UTF-16、采样帧边界及安全保存，不复制原版首行丢字、
ANSI 丢字和直接覆盖写入的缺陷。保存也保留源 TRACK 文本，而不是像原版序列化器
那样强制重写源轨号；公开 subtrack 始终是顺序编号。

上述测试验证的是恢复后的实现。没有宣称所有第三方插件都能逐样本精确定位，也没有
把分段源的字节连续性等同于输出设备切换曲目时必然无缝。本轮没有新增原版 EXE 的
音频逐样本对照；压缩包 CUE 使用既有音频恢复测试覆盖，嵌入音频标签的 CUESHEET
自动展开不属于此次已确认的原版外部 CUE 调用链。
