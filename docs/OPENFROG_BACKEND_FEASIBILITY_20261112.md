# ttp_ofr 改用 OpenFROG 的可行性分析

## 结论

**可以用指定 OpenFROG 取代官方解码后端，并消除播放时将解码 DLL 释放到用户临时目录的步骤。** 但需要改接后端，不能仅替换内嵌资源或覆盖一个文件就完成：

| 方案 | 播放时释放临时 DLL | 部署文件 | 评价 |
|---|---|---|---|
| 当前：内嵌官方 OptimFROG.dll | 有 | ttp_ofr.dll | 保留官方原字节，但依赖可写、可执行的临时目录 |
| 同目录部署 OpenFROG.dll，绝对路径动态加载 | 无 | ttp_ofr.dll + OpenFROG.dll | 改动较少，可直接使用本次指定二进制 |
| 将 OpenFROG 源码静态合入 ttp_ofr.dll | 无 | ttp_ofr.dll | **推荐**，保留现有单 DLL 安装方式 |

现有 `OpenFROG.lib` 随 `add_library(OpenFROG SHARED ...)` 生成，是 DLL 的导入库，不是能独立包含解码器的静态库。实现第三种方案需要编译源码/静态目标；不能把现有导入库当作静态后端。

本次只做静态核对和独立 SDK 实测，**没有修改或重建 ttp_ofr，也没有替换已部署产物**。

## 1. 指定产物身份及 ABI

目标：`D:\Projects\Backup\OpenFROG\OpenFROG\build\Win32\Release\OpenFROG.dll`。

- PE32 / x86，128,512 字节。
- SHA256：`f8c438cc5363778efe9ca760939a478908c970da6b53d90736033f8c5d94132c`。
- 导出 19 个原 SDK 名称及序号，没有 PE 转发导出。
- `ttp_ofr/src/decoder.cpp:9–16` 使用的 8 个函数全部存在：`createInstance`、`destroyInstance`、`openExt`、`read`、`getInfo`、`readHead`、`seekPoint`、`recoverableErrors`（均带 `OptimFROG_` 前缀）。
- 官方头与 OpenFROG 头的 x86 `__cdecl`、单字节 `condition_t`、`pack(1)` 一致；`ReadInterface=28`、`OptimFROG_Info=68` 字节。
- 静态导入仅 `kernel32.dll`、`msvcrt.dll`，75 个函数，无 delay imports。重新执行插件的 XP/Win7 导入审核通过；不是要求安装现代 VC/UCRT 的产物。

REA 的二进制事实：导出 Evidence `ev_e8ff0a8d2ec7625860b1f1ddfd8f4b0aafa9c22b3076a7329988c83fdfad779c`；导入 Evidence `ev_656a798e56ddca733509403d30c8d306ebf0821b5dc47adeaa71e8bdef5eed91`。完整两条记录保存在 `out/openfrog-backend-analysis-20261112/rea-evidence.json`。这些静态表本身不证明运行行为；下面另列普通进程实测。

## 2. 当前为什么需要临时路径

`ttp_ofr/src/embedded_library.cpp:11–50` 从 RCDATA 读取官方 DLL，创建用户专属临时目录，写出原名 `OptimFROG.dll`，核对字节并锁住文件，再调用 `LoadLibraryExW`。析构卸载后删除 DLL 和目录。`Decoder::Impl` 持有该对象，因此每次创建/重新打开 Decoder 都可能重复这套流程。

`cmake/codec.cmake` 校验官方发布包及 DLL 的固定 SHA256；运行时还固定检查 176,640 字节。指定 OpenFROG 为 128,512 字节，不能直接替换资源。即使修改这些校验，若仍保留 `EmbeddedLibrary`，仍会写临时文件。

Windows 的正常 `LoadLibrary` 接口接收磁盘文件，不会因为 DLL 的实现变成 MIT 就能直接加载资源字节。没有必要为此引入自定义 PE 内存映射器：已有源码时静态链接更直接，也避免手工处理 TLS、CRT、重定位和卸载。

## 3. 本轮实际对照

诊断脚本：`tests/openfrog_backend_analysis_20261112/verify_backend.py`；输出：`out/openfrog-backend-analysis-20261112/backend-comparison.json`。

使用现有 x86 `ofr_probe.exe`，分别加载官方 5.100 SDK DLL 与指定 OpenFROG，走 `openExt` 回调而不是文件名接口。共 **11 个样本 × 3 场景 = 33 组比较，0 失败**（两库共 66 次诊断进程调用）：

- 8/16/24/32-bit，各 mono/stereo，另含 20 有效位 OFR 和 DualStream OFS，共 10 个有效样本。
- 三个场景：完整读取、7 字节短读与 17 帧小批量读取、定位到 17 ms 后读取。
- 30 组有效解码的原生 PCM **逐字节一致**；按照插件现有规则适配 PCM 符号位后，也全部与已知源 PCM/既有 OFS 参考一致。
- SDK 信息、HEAD、位置、EOF、错误标志和关闭回调结果一致；正常样本没有恢复错误。
- OFRX 浮点样本在两库中均拒绝打开，三个场景均符合既有插件的不支持约定。
- 没有修改输入音频；不执行标签写入。

OpenFROG 的既有 `docs/windows-compatibility.md` 记录了**同一 SHA256** 的 XP SP3 / Win7 SP1 运行验收及来宾输出回验；本轮重新验证了二进制身份和静态导入，但没有把该历史验收冒充为本轮替换后的 TTPlayer 插件验收。

## 4. 实施建议

### A. 若直接复用现成 DLL

将 `OpenFROG.dll` 与 `ttp_ofr.dll` 一同部署在 AddIn 目录。改用由插件自身模块路径构造的绝对路径加载，不从工作目录或 PATH 搜索。保留 SDK 函数表、实例销毁早于卸载及失败 HRESULT 传播；删除官方资源释放路径及其大小/哈希检查。运行包需增加第二个 DLL 及相应许可证/校验清单。

这可满足“不解压到临时目录”，但改变现有单 DLL 交付形式。OpenFROG 本身不导出 `ttpGetSoundAddIn`，仍需保留 TTPlayer 的 Reader 适配层。

### B. 若继续只交付 ttp_ofr.dll（推荐）

1. 将 OpenFROG 的 container/primitives/codec 与 SDK API 编译为私有静态目标或对象库，替换原官方 SDK 依赖。
2. 为头文件提供静态调用声明；不能沿用 `__declspec(dllimport)`。SDK API 无需额外暴露给宿主，保留原 `ttpGetSoundAddIn` 和 Reader ABI。
3. `decoder.cpp` 改为直接调用 SDK API，保留 IStream 回调、符号位适配、真实 HEAD 有效位、取消/错误及定位重开逻辑。
4. 移除产品构建中的 `embedded_library.*` 和 `optimfrog.rc` 接入；最终 DLL 只保留一次兼容 CRT/YY-Thunks 入口初始化。保留 OpenFROG 的 `/fp:strict`、异常及安全拒绝，不用播放器的通用浮点优化替代。
5. 同步源码包白名单、依赖固定身份、许可证、README、CI 和导入审核；不再为生产构建下载/捆绑官方 OptimFROG ZIP。OpenFROG 工程本身为 MIT，兼容组件仍保留各自许可证，不能把所有依赖统称为 MIT。

这一方案不要求宿主增加新接口，也不要求把解码器内置到播放器 EXE。

## 5. 边界与替换前的剩余验收

- OpenFROG 有明确的安全/资源拒绝范围，如 LPC 阶数超过 128、未证明的预测参数域、普通块 64 MiB 预算；不承诺所有官方库可处理的未知比特流都等价。
- OFRX/FLOAT32 仍不支持；OFS 仍是有损 DualStream，不因更换库而恢复为无损。
- 尚需对实际改造后的插件验证 Reader ABI、取消/重试、反复定位、并发实例、标签/封面 Commit、只读/无效 TEMP 环境、宿主集成及 XP/Win7 实测；当前后端对照不能替代这些集成测试。
- **免掉的是播放/读取时释放解码 DLL 到用户 TEMP。** `ttp_ofr/src/metadata.h:86–121` 的标签安全提交仍会在音频所在目录创建替换/备份文件；这是另一类临时文件，不能为了“无临时路径”而删除事务保护。
