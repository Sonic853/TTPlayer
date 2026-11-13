# ttp_ofr → OpenFROG 替代可行性分析

日期：2026-11-12。仅分析与隔离测试，**未修改或部署 `ttp_ofr` / OpenFROG 产品代码**。

## 结论

**可以用 OpenFROG 替换当前解码后端并消除临时 DLL 释放，但不能把指定 DLL 原样作为 XP/Win7 通用替换件。**

1. 指定产物是 x86 DLL，具有当前适配层实际使用的全部 SDK 导出；保持 Reader、IStream 和 PCM 适配代码不变，只替换加载器的隔离构建已通过现有样本回归。
2. 当前产物直接导入 `CreateFile2`，Win7 缺少此 API；XP 还有其它缺失导入且 PE 版本为 6.0。必须对后端本身做兼容重建，不能只在外层 `ttp_ofr.dll` 链接兼容补丁。
3. 不需要手写 PE 内存加载器。可选择外置并按绝对路径加载 `OpenFROG.dll`（双 DLL），或者把 OpenFROG 核心源码静态编译进插件（单 DLL）。后者更符合现有单插件运行包的目标，但还须补齐分发授权/许可证声明并验证剩余格式边界。

## 1. 样本身份与二进制证据

指定路径：`D:\Projects\Backup\OpenFROG\OpenFROG\build\Win32\Release\OpenFROG.dll`。

- 文件：**231,936 字节，PE32 / x86**。
- SHA256：`6e89450aad0d873ae8c4761fb126b48c9f2ee9752c2390b102de4f6d1b9342a6`。
- PE OS / Subsystem 版本均为 **6.0**。
- 19 个命名导出；没有导出转发器。
- 标准导入仅有 `KERNEL32.dll`，77 个函数，无延迟导入；没有外置 VC Runtime DLL 或官方 `OptimFROG.dll` 的静态依赖。
- 导出表证据：`ev_54d0ecc11a9aa3a8f4eed08f575fa11404346f34c1046d2c6bed24666571bd32`。
- 导入表证据：`ev_e8afd7e856a3901778afa7c760aeb0d60570ee4fc1c0abf47595d050d4384cb8`。

完整 REA 证据保存在 `out/openfrog-assessment-20261112/rea-evidence.json`，汇总为 `binary-findings.json`。这是静态表证据，不等于所有代码路径已执行；本轮未在 XP/Win7 上运行该 DLL。

## 2. 现有临时目录的来源

`../ttp_ofr/src/embedded_library.cpp:9-50` 从资源 101 取出官方 DLL 原字节，在 `%TEMP%` 中创建当前用户专有目录，写入 `OptimFROG.dll`、校验，再 `LoadLibraryExW`；关闭时卸载并删除。严格说这里是**释放内嵌 DLL，不是解压音频文件，也不是压缩的可执行载荷**。

`../ttp_ofr/src/decoder.cpp:7-16` 每个 Decoder::Impl 拥有一个 EmbeddedLibrary；`reader.cpp:20-27` 的重开/定位也会销毁并重建 Decoder。因此这项文件系统依赖位于后端加载层，不是 Reader ABI 或 OFR 格式的必然要求。

只把 `OpenFROG.dll` 放到 AddIn、改名为 `OptimFROG.dll`，或替换资源中的 DLL 都不是正确方案：现实现不搜索外部 DLL，资源还有固定长度/哈希校验；即使换了资源，原加载器仍会落地临时文件。

## 3. 接口兼容与隔离实测

现有 `decoder.cpp` 使用的 8 个导出均存在：

`OptimFROG_createInstance`、`OptimFROG_destroyInstance`、`OptimFROG_openExt`、`OptimFROG_read`、`OptimFROG_getInfo`、`OptimFROG_readHead`、`OptimFROG_seekPoint`、`OptimFROG_recoverableErrors`。

原适配层保留官方 ABI 的 `ReadInterface=28`、`OptimFROG_Info=68`、`condition_t=1` 字节断言；实际回调、信息读取、解码、销毁及定位已通过调用验证，不仅凭同名导出推断。

私有评估构建 `tests/openfrog_assessment_20261112/` 直接编译当前插件源码，只以 `external_library.cpp` 替代原加载器，不链接内嵌 DLL 资源。它只从测试环境指定的绝对路径加载选定 DLL 的字节快照。这个环境变量是**测试设施，不是建议的正式插件搜索策略**。

Windows 11 下共 **55 次插件驱动调用**：

- 10 组有效样本：8/16/24/32-bit × 单/双声道、WAVE EXTENSIBLE valid20、DualStream OFS；完整输出均与原始/参考 PCM 逐字节一致。
- 每组另测小缓冲、浮点请求标志、重复定位及读取取消恢复；实际 10 组均触发了取消注入。重复定位由原驱动执行 201、0、177 ms，再定位到 103 ms，均与顺序 PCM 对照一致。
- 1 组 OFRX 按原插件约定明确拒绝，不作为“支持浮点”计分。
- 2 次不可用临时路径对照：将 TEMP/TMP 指向不存在的目录，原插件在 Open 失败；外置 OpenFROG 版本成功，PCM 完全正确，也没有创建该目录。
- 2 次仅针对副本的标签/封面保存及重开：Unicode 标题、ReplayGain、PNG 封面保留，32-bit PCM 不变。

报告为 `out/openfrog-assessment-20261112/assessment.json`，日志/PCM 在其 `decoded/`。原音频样本未写入；正式插件及原构建产物没有被覆盖。

**覆盖边界：** 这是现有短样本回归，不代表所有历史 OFR 版本、全部编码模式、任意损坏流或长文件资源消耗已验收。原插件仍在 `decoder.cpp:60` 拒绝 OFRX；指定 DLL 用直接 SDK 打开同一浮点样本也失败，不能仅因 SDK 有 FLOAT32 类型便宣称新增浮点支持。

### 源码核对与既有验证的边界

已核对同目录的 OpenFROG 源码，而不是将 SDK 同名导出视为全部能力相同：

- `include/OptimFROG.h:6-20,45-120` 明确 `__cdecl`、pack(1) 和单字节 condition；`openExt` 可继续使用现有 IStream，不必转为窄字符文件名。回调 close 的所有权须保持原适配器语义，不可重复释放宿主流。
- `src/container.cpp:260-335` 处理 OFR 整数容器及扩展主头，限制单/双声道并拒绝超出 5100 的最低解码版本；不支持 OFRX 和旧 `*RIFF` 分支。`src/pcm.cpp:17-29` 亦明确拒绝浮点 PCM 打包。
- `analysis/implementation-status.md:22-39` 列出有意拒绝的高阶 LPC、未确认参数域及资源预算；这些是安全实现边界，不能据当前样本宣称全部合法/历史编码穷举兼容。
- `src/api.cpp:176-183` 在兼容 API 中禁用 COMP CRC 不匹配拒绝；`container.cpp:441-445` 仍计算 CRC，但只有 strict parser 会以此拒绝。项目记录这与原 SDK 的兼容策略一致，本轮未独立比较 CRC-only 损坏，**不据此认定发生新退化，也不把 recoverableErrors 当作完整文件校验**。
- 取消仍由适配器事件及 I/O 回调传播。后端没有独立取消 token；已经进入单个块的 CPU 解码不提供任意指令级中断，这一点不因“没有临时 DLL”自动改善。

现有 `test-results/stage2-build-manifest.json` 的 Win32 产物 SHA256 与本次实际 DLL 一致，记录有 993/993 音频场景、40/40 信息/标签差分。源码/语料清单显示这些场景来自约 50 个输入，而非 993 种独立编码；包含官方 5.100 生成模式和部分 4.520 历史样本。**这些是读取到的既有报告，不是本轮重新运行的测试**，不能与本轮 55 次适配器调用混为一谈。

## 4. XP / Win7 阻碍

以现有 YY-Thunks x86 导出清单核对实际静态导入：

| 系统 | 不可用的静态导入 |
|---|---|
| Windows 7 | `CreateFile2` |
| Windows XP | `CreateFile2`、`GetLocaleInfoEx`、`GetFileInformationByHandleEx`、`FlsAlloc/GetValue/SetValue/Free`、`InitializeCriticalSectionEx` |

缺失的静态导入会在加载时阻止绑定，即便所选 IStream 回调路径不主动调用这些函数也不能忽略。建议以当前播放器相同的 VC-LTL / YY-Thunks 目标重编后端，再审计**最终插件及任何外置 DLL**并在 XP/Win7 实测；仅修改 `_WIN32_WINNT` 或 PE 版本号不够。

## 5. 建议的接入边界

- **单 DLL 方案（建议）：** 将 `src/api.cpp` 与现有三个核心静态目标组合成完整 STATIC/OBJECT 后端，直接调用兼容 C API；去掉 EmbeddedLibrary、官方资源以及下载/嵌入官方二进制的构建链。当前 `OpenFROG.lib` 是 DLL 的导入库，不是完整静态解码库，不能只链接它便声称消除外置 DLL。还须处理头文件的导入宏，保留核心 `/fp:strict` 与 API 的 scoped FPU/MXCSR 环境（`src/api.cpp:26-45`）。
- **双 DLL 方案：** 保留动态绑定，但只加载插件目录中确定的 `OpenFROG.dll` 绝对路径，模块生命周期须覆盖所有 decoder；禁止搜索当前目录或修改进程 DLL 搜索路径。该方案改变现有运行包布局。
- 保留现有 Reader/Creator/Probe/SourceInfo、IStream 短读/错误传播、符号位转换、HEAD 有效位和 APEv2/封面写回；无需修改播放器接口。
- 正式切换须同步 `CMakeLists.txt`、`cmake/codec.cmake`、`src/decoder.cpp`、内嵌资源/加载器、源码包与许可证/来源文档，不能只替换一个文件名。
- 元数据原子写回可能仍需在音频所在目录创建替换文件；它与解码库释放到系统临时目录是两件事，不应为“零临时 DLL”删掉数据安全措施。

## 6. 分发授权及最终判断

在所选 OpenFROG 工程中未找到适用的 `LICENSE` / `COPYING` 或独立实现的静态链接、二进制再分发授权声明。保留的官方 SDK 许可不自动等于 OpenFROG 实现的许可；正式分发前应由权利人确认并补齐声明。这里仅记录授权证据缺口，不判断源码来源是否合法。

**建议：采用 OpenFROG 源码静态集成、沿用现有旧系统工具链与 Reader ABI；完成授权说明和 XP/Win7 重建/实测后再切换正式后端。** 若只需 Windows 10/11 且接受双 DLL，隔离结果已证明外置加载方案在当前样本范围可行。当前指定 DLL 不应原样加入跨 XP/Win7 的正式交付。
