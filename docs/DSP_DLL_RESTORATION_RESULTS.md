# DSP DLL 实际恢复结果

日期：2026-09-28。

已将 12 个验证通过的常规 x86 DLL 输出到 `D:\Projects\Backup\TTPlayer\Plugins-out`：6 个 UPX 系、6 个 PECompact 2。原始 `Plugins` 下的 17 个 DLL 均重新核对 SHA-256，内容未改动。

两批文件均已验证加载、换址、接口、代码与资源一致性。XP／Win7 最终各完成 74 个用例，共 148 个全部通过，覆盖原始／恢复文件的加载、换址、默认音频处理、退出及同时加载／卸载。**配置界面、全部预设及播放器内长时间播放尚未完整回归，不能据此宣称全部行为已完全验收。**

## 1. 已交付文件

| 文件 | 原大小（字节） | 恢复大小（字节） | 恢复方法 |
| --- | ---: | ---: | --- |
| `dsp_compwide.dll` | 8,192 | 17,408 | UPX 解压及布局修复 |
| `dsp_DEE.DLL` | 86,016 | 487,424 | UPX 解压及布局修复 |
| `dsp_DeFX.dll` | 30,720 | 67,072 | PECompact 初始化前捕获及 PE 重建 |
| `dsp_enh.dll` | 45,568 | 117,248 | PECompact 初始化前捕获及 PE 重建 |
| `dsp_iZVinyl.dll` | 1,110,528 | 2,199,552 | UPX 解压及布局修复 |
| `dsp_jammix.dll` | 242,176 | 730,112 | UPX 解压及布局修复 |
| `dsp_neq.dll` | 69,120 | 330,752 | UPX 解压及布局修复 |
| `dsp_OctiMax.dll` | 203,264 | 1,373,696 | PECompact 初始化前捕获及 PE 重建 |
| `dsp_so3d.dll` | 463,872 | 993,792 | PECompact 初始化前捕获及 PE 重建 |
| `dsp_sps.dll` | 22,016 | 41,984 | PECompact 初始化前捕获及 PE 重建 |
| `dsp_superequ.dll` | 85,504 | 216,064 | PECompact 初始化前捕获及 PE 重建 |
| `DSP_ViPER’s Audio.dll` | 243,200 | 663,552 | UPX 解压及布局修复 |

同目录包括全部 12 个 DLL 的 `SHA256SUMS.txt`、`restoration-manifest.json` 和 `README.md`。另外 5 个本来就是常规格式的 DLL 保留在原目录。

## 2. 具体恢复方法及额外修正

### 2.1 官方 UPX 工具

采用固定版本 UPX 5.2.1，其官方下载归档 SHA-256 为：

```text
eabc6792a347d45e945be7748423e7868fd01b0d2bcaa2f4b1031fd71ff69bda
```

来源为 [UPX 官方 v5.2.1 发行页](https://github.com/upx/upx/releases/tag/v5.2.1)。该值是本地下载后计算的记录；GitHub API 当时达到访问限额，未取得资产 API 的 digest 做独立比对。

普通样本在副本上经过 `-t` 校验、`-d -o` 解压。NEQ、ViPER 需要先修复副本元数据，之后同样通过 UPX 原有数据校验；没有关闭压缩/解压数据校验，也没有修改其音效算法。

### 2.2 NEQ：修正与实际解压代码不符的算法字段

原始 `dsp_neq.dll` 的 UPX 头算法字段是 `0x05`，但入口解压循环与 NRV2B 样本一致，与 NRV2D 的偏移编码循环不一致。

最终只在输入副本做了一项修复：

```text
文件偏移 0x3e6：05 → 02
```

这一处改正后：

- 原有头校验字节 `0xca` 原样成立，不需要修改。
- 原有压缩数据与解压数据校验全部通过。
- 导出 `___CPPdebugHook`、`winampDSPGetHeader2` 均保留。
- 与原文件实际解压代码逐字节匹配。

调查过程曾尝试仅重算头校验值，以及改变 MRU 字段，这些都未通过数据校验，**未进入最终输入副本和交付文件**。可复现脚本从原文件重新开始，仅实施上面的一字节修复。

### 2.3 ViPER：恢复标识与节名

压缩头其余字段与原校验值完全一致。最终修复输入副本中的：

| 文件偏移 | 原值 | 修复值 |
| --- | --- | --- |
| `0x3e0` | `ess!`（`Compress!` 的最后四字节） | `UPX!` |
| `0x1e8` | `SEGX` | `UPX0` |
| `0x210` | `SEGY` | `UPX1` |
| `0x238` | `.SEGZ` | `.rsrc` |

随后官方解压和数据校验均通过。恢复文件保留 `ResetDSP` 和 `winampDSPGetHeader2` 两个导出。对 5 项资源逐项比较一致，原静态资源范围警告未变成丢失资源的恢复结果。

### 2.4 compwide：恢复节文件间隙

UPX 虽报告解压成功，最初输出的 16,896 字节文件仍有结构问题：节表指定 `.reloc` 位于文件偏移 `0x3e00`、`.edata` 位于 `0x4200`，实际数据却分别连续写在 `0x3c00`、`0x4000`。

对照固定版本 `src/pefile.cpp` 的写出逻辑，确认其按节顺序连续写入，未保留本样本的这一文件间隙。恢复处理按节表的 `PointerToRawData` 重新摆放已有节数据，补齐 512 字节间隙，保留全部业务 RVA 和内容。

最终大小为 17,408 字节，等于压缩头所记录的原文件大小。导出表、重定位表解析及实际换址加载均恢复正常。

## 3. 第一批 UPX 文件的本机验证

### 3.1 可复现性

从 6 个原始 DLL 重新运行固定版本解压及最小修复，得到与候选恢复文件完全相同的 SHA-256。此复现过程不使用加载后的内存状态生成输出 DLL。

最终本地脚本：`tests/reproduce_dsp_restoration.py`。

### 3.2 本机 x86 加载验证

共完成 24 次独立加载：6 个文件 × 原始/恢复 × 普通/强制换址。

全部通过：

- `LoadLibraryW` 和 `FreeLibrary`。
- `winampDSPGetHeader2`、模块枚举直至结束。
- 插件版本、描述文本哈希、模块数量一致。
- 配置、初始化、音频处理、退出的回调 RVA 一致。
- 强制换址时记录实际模块基址，确认确实离开首选地址。

另外分别把 6 个原始插件、6 个恢复插件在同一进程全部加载，随后逆序卸载，两组均通过。

探针是独立 32 位进程，注册表 HKCU/HKLM 访问被重定向到本次唯一的临时测试分支。调用 DLL 加载入口并读取插件接口，**未调用 DSP Init、Config 或 ModifySamples**。

### 3.3 业务代码逐字节比较

从上述独立进程采集原文件解压后的映像和恢复文件映像，依据恢复文件的业务重定位表消除基址差异。

对恢复文件声明的可执行节比较 `min(VirtualSize, SizeOfRawData)` 范围：共 **871,176 字节**。普通加载与强制换址两组中，各文件差异均为 **0 字节**。

比较范围不包含节尾零填充、壳代码和已被系统运行时修改的其他数据区，不能扩展解释为整个内存映像逐字节相同。

### 3.4 资源比较

按原始及恢复 DLL 各自运行时资源树遍历名称/ID、语言、大小、代码页和资源内容哈希，127 个资源项在两种加载模式下全部一致。

资源匹配能证明恢复没有改变这些资源内容；界面事件、显示效果和配置交互仍需要真实功能测试。

## 4. PECompact 与虚拟机验证

### 4.1 第二批 PECompact 2

`dsp_DeFX.dll`、`dsp_enh.dll`、`dsp_OctiMax.dll`、`dsp_so3d.dll`、`dsp_sps.dll`、`dsp_superequ.dll` 均已恢复原始入口、完整业务导入、业务重定位、资源及初始化前数据，输出到 `Plugins-out`。

第二批完成 24 次本机独立加载对照：归一化基址后的 743,424 字节可执行节内容无差异，145 个资源在普通／换址模式下均一致。两个基址独立取得的完整初始化前业务区也均为零差异。既有 Enhancer `loaded.bin` 不参与构建。

两批共验证 **1,614,600 字节可执行节内容、272 个资源**。详见 [PECompact 还原方法与取证](DSP_PECOMPACT_RESTORATION.md)。

### 4.2 XP / Win7

全部 12 对原始／恢复文件在两台虚拟机共完成 148 个用例，最终全部通过。每份文件都完成 Init、44.1 kHz／48 kHz 默认配置 PCM 处理、Quit 和卸载；同一进程加载 12 个 DLL 后逆序卸载也通过。

用户截图中的 `loader_probe.exe — abnormal program termination` 是测试宿主隔离注册表后影响 OctiMax 的 Winsock 初始化导致，原始 DLL 同样受影响。已修复测试宿主初始化顺序并验证。其它测试环境修正、音频摘要限制见 [虚拟机测试报告](DSP_DLL_RESTORATION_VM_TESTS.md)。

### 4.3 本来就是常规 DLL 的文件

`Dsp_Dfx.dll`、`dsp_Dolby.dll`、`dsp_izOzone.dll`、`dsp_skywalker.dll`、`DSP_VAPXP.dll` 无需脱壳，没有复制到输出目录。

## 5. 与 registry.json 接入的边界

本次未修改播放器的通用注册表接入逻辑。

这批恢复文件在磁盘上已具有完整业务导入名称，但 UPX 输出的部分/全部导入描述符 `OriginalFirstThunk` 为零，加载时 `FirstThunk` 会变成实际函数地址。它是可正常加载的 PE 形式；宿主不能继续假设每个常规 DLL 都保留独立的运行时名称表。

后续通用兼容层仍需从磁盘导入元数据建立对应关系，或使用其他经过验证的识别办法；动态解析的 API 和依赖模块调用也仍需覆盖。

PECompact 恢复结果具有独立 `OriginalFirstThunk`。恢复后 Enhancer 的哈希已改变，现有按原始哈希识别的注册表兼容分支不能自动认作已覆盖。此前暂停的通用 DSP 接入没有在本轮恢复。

## 6. 尚未覆盖的测试

- 原版／重建版播放器内的长时间实际播放、反复启停和所有参数组合。
- PCM 逐字节对照、随机音效完整等价性，以及默认旁路以外的全部预设。
- 配置窗口、预设、参数持久化的交互回归。

所有测试代码、工具和中间映像均保存在本地 `rebuild/tests`，未加入 Actions、发行包或提交。`Plugins-out` 仅含恢复结果与清单，不含测试程序。

## 7. 证据文件

均位于本地 `rebuild/tests/artifacts/dsp_restore`：

- `reproduction.json`：UPX 最小修正及复现记录。
- `validation.json`、`resource-validation.json`、`batch-validation.json`：第一批本机结果。
- `compwide-layout-fix.json`、`images/`：布局修复和映像证据。
- `pec/captures.json`、`pec/restoration.json`：初始化前捕获和 PECompact 重建记录。
- `pec/validation.json`、`pec/resource-validation.json`：第二批本机结果。
- `vm/files.json`、`vm/XP-final.log`、`vm/Win7-final.log`、`vm/validation.json`：最终虚拟机结果。

恢复前的方法分析保留在 [DSP_DLL_UNPACKING_RESTORATION_PLAN.md](DSP_DLL_UNPACKING_RESTORATION_PLAN.md)。
