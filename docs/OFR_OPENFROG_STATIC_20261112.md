# ttp_ofr 静态 OpenFROG 实现与验证

## 交付与架构

按用户选择，将 `D:\Projects\Backup\OpenFROG\OpenFROG` 的生产源码静态合入 `ttp_ofr.dll`。从干净提交 `5dc0c80cf0c357e8cadbbae9b593f1b3e62d917d` 导入 **22 个未修改的生产源码/头/许可证文件**，保存在 `ttp_ofr/third_party/openfrog`；逐文件 SHA256 在配置和源码打包时核验。没有修改原 OpenFROG 工程，不依赖它在运行或独立源码包重建时继续存在。

- `cmake/codec.cmake` 创建私有静态后端，保留 `/fp:strict`、异常、SSE2 和原安全拒绝。
- `src/decoder.cpp` 直接调用 SDK C API，原 x86 pack(1)、IStream 回调、符号位适配、HEAD 有效位、错误传播及 Reader 接口不变。
- 移除 `embedded_library.cpp/.h`、官方 DLL 的 RCDATA 及 `optimfrog.rc` 构建接入；生产构建不再获取官方 SDK ZIP。
- 不引入 `OpenFROG.dll`、`OptimFROG.dll`、编码器 EXE、手写 PE 映射或运行时后备释放路径。CRT/YY-Thunks 仅由最终插件初始化一次。
- SDK 函数保持私有，仅导出原 `ttpGetSoundAddIn`，序号 1；不增加宿主静态依赖或修改播放器 EXE。
- 源码包、依赖来源、许可证、发布说明和开发指南同步调整；运行 ZIP 仍只有 `AddIn/ttp_ofr.dll` 和 SHA256。

当前产物 **174,592 字节**，较官方 DLL 内嵌版 **280,064 字节**减少 105,472 字节（约 37.7%）。SHA256：`b908114a009b557f85ed1daf0d082f614dd7117fd93ada3b8350a516e4c69f81`。

## 无用户 TEMP 依赖的实测

所有套件进程的 `TEMP` / `TMP` 均指向测试目录中的**普通文件**，不是可写目录。新插件能完成读取、定位、宿主解码和标签/封面提交；原有内嵌版在同一无效 TEMP 下于 Open 返回 `0x8007010B`。这不是仅观察结束时临时目录是否被清空。

额外 native contract 检查：

- DLL 加载前已存在的两条线程，并发创建/销毁 8 个实例。
- IStream 每次最多 13 字节短读、40 次定位与完整 PCM 对照。
- 调用者预设向下舍入/x87 扩展精度，检查读/定位后 FPU/MXCSR 控制环境恢复。
- 加载模块中没有外部 OpenFROG/OptimFROG DLL；没有资源 101 或 SDK API 导出。

上述 contract 在 Windows 11、XP、Win7 均执行并通过。

## 格式、宿主和虚拟机

私有脚本：`tests/ofr_openfrog_static_20261112/`；输出：`out/ofr-openfrog-static-20261112/`。

**49 个样本**包括：此前 10 个有效 OFR/OFS 和 1 个预期拒绝的 OFRX，以及 OpenFROG 的 38 个已知源 PCM 编码样本。覆盖有/无符号整数 8/16/24/32-bit、mono/stereo、20 有效位、DualStream OFS、多种新旧压缩模式、跨块和非静音数据。测试使用复制品，不编辑原音频。

| 环境 | 套件调用 | 宿主日志补跑 | 有效样本 | 最终校验失败 |
|---|---:|---:|---:|---:|
| Windows 11，x86 插件 | 297 | 0 | 48 | 0 |
| XP SP3 x86，5.1.2600 | 297 | 0 | 48 | 0 |
| Win7 SP1 AMD64，6.1.7601，运行 x86 产物 | 297 | 2 | 48 | 0 |

297 次套件调用包括：48 ×（完整 PCM、小缓冲、flags=3、反复定位后读取、读取中途取消/重试、实际宿主解码），以及 OFRX、非 seek、截断拒绝、ABI、native contract、两组标签/封面保存与重读。每个环境 **48 组均实际注入读取取消**；五种读取模式的 PCM/定位后缀与已知源或既有 OFS 参考逐字节比较；48 组宿主完整 PCM/count/hash 与对应原始输出核对。

XP/Win7 使用 MCP 和新目录 `C:\TTPlayer_OfrStatic_20261112_v2`，不覆盖既有安装、不操作虚拟机电源/快照。实际输出已取回至 `vm-results/xp-v2` / `win7-v2`，不是仅采信 MCP `(ok)` 或进程退出码。

**保留的测试诊断：** 初次来宾测试误复制了宿主已写入封面的编辑样本，重复添加封面按约定被拒绝；已修正套件在每次标签测试前从未编辑样本创建新副本，初次结果仍保留。Win7 第二轮两份宿主日志为空（批处理记录退出码 0），未计作证据；对这两项做只读补跑，实际 PCM/count/hash 日志验证通过，原空日志与补跑日志均保留。没有为消除测试诊断而修改解码实现。

环境报告：`windows11-tests.json`、`xp-tests.json`、`win7-tests.json`。静态导入审计通过，最终依赖仅 ole32、shlwapi、kernel32、msvcrt，共 80 个导入；仍有 `GetTempFileNameW` 用于下述标签事务，不能据此推断还在释放解码 DLL。

## 包与源码独立性

运行/源码附件及其 SHA256 汇总到 `out/ofr-openfrog-static-20261112/delivery/`。源码包包含所有实际使用的 OpenFROG 生产源码及 MIT 许可证、插件适配层、构建脚本和兼容组件许可证，不含官方 SDK/DLL、编码器、私有样本或测试工程。

最终源码包独立提取到 `source-verify-v2`，以兼容工具链缓存冷构建到 `source-build-v2`；不使用相邻 OpenFROG / ttp_ofr 源目录。复验 32-bit PCM、20 有效位、OFS、跨块四组完整 PCM，以及无效 TEMP/并发/FPU native contract；具体身份和结果记录在 `packages.json`。

二进制 REA Evidence：导出 `ev_ddeafd483e05e2fc1435d505c9e542ac1ecf28f2fd733a54469d63e15ab8c5fe`，导入 `ev_fa8f207a042a554a8f618b32147cd5e7e3404cdaa8ed8665f5b42a9e2e74e6a2`，资源 `ev_0095f4696328459b254f1af821c18f1d7ebf1ee8449ec8b18f0181bd3dd10128`。资源表完整检查仅有 VERSIONINFO 和 manifest，不含内嵌解码 PE。Evidence 包在输出目录 `rea-evidence.json`；这些是静态观察，运行结论来自另列测试。

## 保留边界

- OFRX / FLOAT32 仍明确拒绝；OFS 仍是有损 DualStream。
- OpenFROG 的 LPC 阶数 >128、未证明参数域、普通块 64 MiB 等安全/资源边界不放宽，不声明穷尽全部 OptimFROG 流。
- **免除的是播放/读取时将解码 DLL 写入用户 TEMP。** 标签/封面写回仍在音频目录创建安全替换/备份文件并清理，不能删除其事务保护。
- 虚拟机样本使用 ASCII 文件名；未据此声称所有 Unicode 路径、全部 CPU/补丁、原版 GUI、Win10 或实际音频设备播放均已验收。
- 未替换工作区根目录或虚拟机已有播放器/DLL，未执行提交、推送或远端 Release。
