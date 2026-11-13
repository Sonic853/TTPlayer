# ttp_ofr：OpenFROG c37926c 更新与验证

## 更新结果

将用户指定的 `D:\Projects\Backup\OpenFROG\OpenFROG` 解码源码更新到 `ttp_ofr`。检查时上游已从之前的未提交状态变为干净提交 **`c37926cf6fc612cb2e9a0a2371964e0b4ff919d4`**；没有修改上游，也没有要求用户另行提交。

- `third_party/openfrog` 保存 **24 个与上游工作树逐字节相同的源码/头/许可证文件**，比旧版增加 `float_details.cpp/.hpp`。
- `SOURCE_MANIFEST.json` 记录提交、导入时工作树状态、逐文件 SHA256 和整体快照 SHA256；配置和源码打包核验实际清单、文件内容及整体摘要。修改源码、漏掉浮点依赖、额外加入源码或错误身份均会失败，9 项私有测试通过。
- 快照 SHA256：`3006513145ee448c247e2b16f814d241d8ccd78021f37510af059c35b798a51c`。
- `cmake/codec.cmake` 增加浮点细节解码目标；保持 `/fp:strict`、SSE2、异常、安全边界及最终插件唯一兼容 CRT/YY-Thunks 入口。
- 移除 `src/decoder.cpp` 的旧 OFRX 无条件拒绝。沿用已存在的 IEEE_FLOAT 格式协商和“不翻转浮点符号位”路径，`max16bit=false` 返回原始 IEEE32 字节，不做 host float 计算、幅度归一化或有损降位。
- 只接入解码依赖闭包。上游的 OFF 编码器、range encoder、WAVE、MD5 和独立 verify/check CLI 没有 SDK 解码引用，不编译、不打包，不新增 TTPlayer 编码器或校验接口。
- Reader/Metadata/Thumbnail/Commit/Probe ABI 及 `.ofr` / `.ofs` 过滤不变；OFF/OFRX 是文件内容格式，官方生成的测试文件本身仍使用 `.ofr` 后缀。

仍为私有静态后端及**单 DLL**部署，不释放临时解码库、不加载外部 OpenFROG/OptimFROG DLL。默认解码沿用上游不验证存储 CRC/MD5 的策略；结构/浮点细节损坏致命失败，不用静音掩盖。OFS 仍为有损 DualStream，LPC 阶数 >128、未证明参数域、普通块 64 MiB 等原有限制不放宽。

## 产物身份

`ttp_ofr.dll`：**179,200 字节**，SHA256：

```text
93d96f9a9dcddd77d187c35e4a0c2c86bc3877a3797ea571c1474769ac688550
```

比上一轮 174,592 字节增加 4,608 字节。最终导出仅 `ttpGetSoundAddIn`，序号 1；资源类型只有 VERSIONINFO（16）和 manifest（24），没有 RCDATA/解码 PE 或 SDK API 导出。XP/Win7 静态导入审核通过，仍只依赖 ole32、shlwapi、kernel32、msvcrt，共 80 个导入。

## 本轮插件级验证

私有测试：`rebuild/tests/ofr_openfrog_update_20261113/`；输出：`rebuild/out/ofr-openfrog-update-20261113/`。旧版测试和交付目录没有被覆盖；不将 OpenFROG 上游 SDK/CLI 的历史测试结果当作本轮插件验收。

88 个有效样本包括：

- 原有 48 个有效整数/OFS 样本，覆盖 8/16/24/32-bit、有/无符号、mono/stereo、20 有效位、压缩模式及 DualStream。
- 之前被插件拒绝的 1 个真实浮点 OFRX 样本，现与原源 PCM 一致。
- 上游保留的 35 份真实 OFF 编码文件及已知 `.f32le`，核验其原清单身份；覆盖 FLOAT32 类型 8/9/10、±0、正负 subnormal、Inf、qNaN/sNaN payload、多种压缩模式和多块流。
- 4 个已知整数样本只改变 OFRX 主签名的合法布局变体，保留原 PCM 参考；不声称它们是官方 OFF 编码样本。

| 环境 | 最终套件调用 | 独立宿主补跑 | 实际读取取消 | 失败 |
|---|---:|---:|---:|---:|
| Windows 11，x86 插件 | 551 | 0 | 88 | 0 |
| XP SP3 x86，5.1.2600 | 551 | 0 | 88 | 0 |
| Win7 SP1 AMD64/WOW64，6.1.7601 | 551 | 1 | 88 | 0 |

每个有效样本执行：完整 PCM、65 字节输出缓冲、flags=3、反复定位后读取、实际 IStream 读取取消/恢复、实际宿主 Reader 选择/解码。前三种与取消后的全部 PCM 逐字节匹配参考；定位按 Reader 毫秒接口计算精确帧后缀。OFF 多块边界为第 220,500 帧，另验证边界附近三次定位；宿主完整 PCM 的计数/hash 与参考核对。

全套进程的 TEMP/TMP 指向**普通文件**，而非可写目录。整数、浮点特殊值和浮点多块三组 native contract 各自检查：DLL 加载前已存在的两条线程、8 个并发生命周期实例、每次最多 13 字节 IStream 短读、40 次定位/完整 PCM、调用者 x87/FPU/MXCSR 控制环境保留、缓冲区边界及无外部解码模块。标签/封面在三类新副本上保存和重读，音频字节不变；保留音频目录中的安全替换/备份事务，不混同于解码 DLL 释放。

额外检查非定位流拒绝，以及缺失 DETA、DETA 短长度/超预算/非法 range tag、COMP 类型不匹配、非法主类型、非声道对齐 COMP 及截断音频。损坏数据在 Open/Read 返回错误，驱动断言错误读取长度为零。另从上一轮已核验目录取得固定旧静态 DLL，确认它对同一 OFF 仍返回 `ERROR_NOT_SUPPORTED`，不使用可能已变化的默认构建作为基线。

### 保留的分发/日志诊断

VM 使用新目录 `C:\TTPlayer_OfrUpdate_20261113`，未改电源、配置、快照或已有安装。首次 MCP 目录传输只复制顶层文件，漏掉测试宿主所需的 `AddIn` 子目录，造成 88 项宿主选择失败；直接插件测试成功。已在批处理入口显式从本次 DLL 创建本地 AddIn，完整复跑到独立 `out-r2`；最初失败日志保留在 `xp-results` / `win7-results`，没有通过修改插件来掩盖测试分发问题。

Win7 第二轮 `case-084-host` 日志为空、退出码为 0，未据此判定通过；另保存只读补跑的非空日志和退出码，实际 PCM hash 核对成功。其余最终日志均非空。取回 XP/Win7 的直接插件与 AddIn DLL 四份实际文件，SHA256 均与本轮产物一致；取回的日志/PCM 经本地验证，不只采信 MCP `(ok)`。

## 包与独立重建

运行 ZIP 仅含 `AddIn/ttp_ofr.dll`、`SHA256SUMS.txt`，原始 ZIP 条目均为正斜杠，实际通过现有 CodecAddIn 严格读取器；测试使用明确标注的离线 Release 元数据，没有远端发布。

源码 ZIP 为 64 个文件，包含完整适配层、24 个 OpenFROG 文件及三方许可证、兼容构建/打包脚本；无外部 SDK/DLL、编码器、测试媒体或原工程绝对依赖。提取到 `source-verify`、冷构建到 `source-build`，只复用固定兼容工具链缓存，不依赖相邻 OpenFROG/插件源码。冷构建后在无效 TEMP 下复验 9 组 PCM（整数32、有效20、OFS、整数参考、FLOAT32 类型8/9/10、特殊值、多块）及三组 native contract，均通过；导出和资源边界再次核验。冷构建 DLL 单独记录身份，不声称不同目录构建的 PE 字节必然相同。

最终附件与报告位于 `out/ofr-openfrog-update-20261113/delivery/`。完整身份见 `SOURCE_MANIFEST.json`、`packages.json`、`deployment.json` 和三个环境的 `*-tests.json`。

本次未覆盖已部署播放器/插件，未执行 git 提交、推送、workflow dispatch 或远端 Release。有限语料验收不代表全部比特流/CPU/系统、Win10、原版 GUI、全部 Unicode 路径或真实音频设备播放均已测试。
