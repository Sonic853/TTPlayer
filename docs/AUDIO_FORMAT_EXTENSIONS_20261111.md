# WV / OFR / ALAC / Speex 实施记录

日期：2026-11-11。工作区源码构建与 Windows 11 x86 ABI 实测；不是原版 GUI 或旧系统实机验收。

## 交付

| 工程 | 实现 |
|---|---|
| `ttp_wv` | 新的本地独立 Git 工程；WavPack 5.9.0 静态解码，原整数/float32、有效位、真实 mask、64-bit 流回调、本地 WVC、APEv2/封面/Commit |
| `ttp_ofr` | 新的本地独立 Git 工程；官方 5.100 SSE2 DLL 原字节 RCDATA、SDK 1.600 回调、OFR/OFS 整数输出与定位、APEv2/封面/Commit |
| `ttp_aac` | MP4 新增 ALAC sample entry、精简 FFmpeg 9.0.2 整数解码、16/20/24/32 有效位、edits/fragment/Seek；`.alac/.mp4a` 为 MP4 合法别名 |
| `ttp_ogg` | BOS 分流 Speex 1.2.1，NB/WB/UWB、mono/stereo、多帧包、granule 裁剪、重放式 Seek、独立注释包装与写回 |

开发指南新增高精度 PCM 规范。正式 SDK 增加 WavPack/OptimFrog 家族、Speex 识别及 MP4 别名；同步工具同时维护 `pcm_format.h`，其他仓库的相应变化仅为协议头副本同步。
PCM helper 保留非默认 mono/stereo mask，拒绝与容器位宽不符的浮点 valid bits。旧虚表、IID、结构尺寸及 ABI 版本均不变。

新插件没有 Musepack 的音频 ReplayGain 区块补丁。四个工程均配置 YY-Thunks 的 DLL CRT 入口/别名，并执行 XP/Win7 导入审计；内嵌 OptimFROG.dll 另做导入审计。

## 内嵌 OFR

库来自官方 `OptimFROG_Win_x86_SSE2_5100.zip`，SHA-256 `8d2c910cd7c1b730a16ed9357cd23708efa4312021c22814172b67af41f14b34`。
`OptimFROG.dll` 为 176,640 字节，SHA-256 `83e664a4c4f9403e0292892efdbcb800cdd6705ff3db476d7d21fa91f4f33ab1`，与工作区已有 DLL 相同；已有文件未改动。

DLL 不做修改或可执行压缩。每个实例在 owner-only ACL 私有临时目录用原名生成文件，锁定读取句柄，复核写入字节，再按绝对路径加载；关闭实例后卸载、清理。不搜索或覆盖播放器目录中的同名库，不改变进程搜索路径。
因此运行环境必须允许临时目录写入和加载；不是纯内存 PE 映射器。source 附件保留官方完整 ZIP 的原名和原字节。

## 验证与产物

测试代码：`rebuild/tests/audio_format_extensions_20261111/`。详情、命令及控制版本见该目录 README。
数据及报告：`rebuild/out/audio-format-extensions-20261111/`，包括 `matrix-all.json`、`package-checks.json`。
最终结果：49 组解码/回归、7 组预期拒绝、0 失败；43 组实际触发了带部分字节的读取取消。协议测试 `FAILURES=0`、四工程打包与 SDK 副本校验通过。

已验证完整 PCM、低有效位、多声道顺序、float 位模式、WVC/OFS、ALAC edits/分片、Speex granule/短读、取消恢复、重复定位、错误后缀、截断报错，以及 Unicode/ReplayGain/封面写回后音频不变。
Opus 既有定位差异用本轮修改前的独立构建逐字节控制；不以放宽误差阈值掩盖回归。

四份运行包版本为 `2026.11.11`，仅含 `AddIn/对应插件.dll` 与 SHA256；WV/OFR/AAC 另有 source 附件。运行包、源码附件、许可证、固定依赖字节和 SHA256 均经独立脚本检查。
七份 ZIP 已汇总至 `rebuild/out/audio-format-extensions-20261111/delivery/`，附聚合 SHA256；它们与各工程经验证的 Release 产物逐字节一致。

## 边界与尚未执行

- OFRX / OFF IEEE float 分支：官方 SDK 1.600 库本身无法打开，插件明确返回 `ERROR_NOT_SUPPORTED`；不是成功解码项。OFS 不因整数位宽而成为无损。
- WV 不含 DSD 或 4.0 之前版本。Speex 当前只处理一个选定 logical stream；本地 chained Speex 明确拒绝，Seek 从 BOS 重放，长文件可能较慢。
- `.alac/.mp4a` 是 MP4 别名，不支持缺失容器配置的任意裸 ALAC。
- 没有本轮 XP/Win7/Win10 实机、原版 GUI、全部坏流 fuzz 和标签 I/O 故障注入的验收。导入检查不等于系统兼容性实测。
- 没有创建 GitHub 远端、提交、推送或发布。新工程的 workflow/校验/source 打包已准备；远端真实名称及稳定 release 尚未建立，故未把猜测名称加入播放器 `BUNDLES`，避免破坏现有合集流程。
