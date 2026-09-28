# PECompact 2 DSP DLL 还原记录

日期：2026-09-28。

## 1. 本次恢复范围

| 文件 | 恢复大小（字节） | 原始入口 RVA | 导入函数 | HIGHLOW 重定位 | 资源 |
| --- | ---: | ---: | ---: | ---: | ---: |
| `dsp_DeFX.dll` | 67,072 | `0x2aa0` | 41 | 1,163 | 5 |
| `dsp_enh.dll` | 117,248 | `0x7a36` | 100 | 2,669 | 5 |
| `dsp_OctiMax.dll` | 1,373,696 | `0x282f9` | 425 | 9,243 | 36 |
| `dsp_so3d.dll` | 993,792 | `0x1000` | 61 | 4,433 | 76 |
| `dsp_sps.dll` | 41,984 | `0x5b91` | 72 | 743 | 2 |
| `dsp_superequ.dll` | 216,064 | `0xb587` | 243 | 4,728 | 21 |

上述文件与此前 6 个 UPX 系文件共同构成本轮 12 个恢复对象。`Dsp_Dfx.dll` 是另一个本来就无压缩壳的文件，与 `dsp_DeFX.dll` 不同。

## 2. 获取初始化前的数据

参考 [PECompact 官方加载器说明](https://bitsum.com/pec2av.htm)及[官方结构资料](https://bitsum.com/pecompact2-av-info.shtml)，再与这 6 个 DLL 的实际反汇编逐项核对：外层解码器通过异常处理转入加载器，主加载器恢复原始映像，再返回原始入口。恢复所需的信息位于加载器前方的宿主结构及其块描述符中。

采集工具仅处理原始文件的进程内映像：先以 `DONT_RESOLVE_DLL_REFERENCES` 映射，解析外壳自身的系统导入，再在解码器调用主加载器的前后设置断点。主加载器恢复业务映像后、解析第一个业务导入前，采集尚未被系统函数地址覆盖的导入名称及初始数据；主加载器返回时，采集资源与完成恢复后的映像，随即结束进程，**不进入原始 DLL 入口及其 CRT 初始化**。

这样保留的是可重复初始化的数据。此前在 `LoadLibrary` 完成后取得的 `loaded.bin` 可能包含堆指针、句柄、TLS 状态，不能直接作为文件输出；本轮没有采用这种方式。

每个原始文件分别在首选基址和强制换址状态下独立采集。按恢复出的真实重定位表消除地址差后，从 RVA `0x1000` 到原始重定位区之前的全部业务数据逐字节比较：**6 个文件的差异均为 0**。

## 3. 标准 PE 结构重建

- 从宿主结构恢复真实入口；SPS 的真实入口为 `0x5b91`，不能继续沿用外壳入口。
- 从初始化前的数据解析完整业务导入；重建独立名称表 `OriginalFirstThunk`，保留业务 IAT 的 RVA。
- 原重定位记录的 WORD 项采用差分编码。对块内记录依次累加，恢复标准 `HIGHLOW`/填充项；根据真实记录修复地址，不用地址范围猜测指针。
- 保留业务节的 RVA、初始数据和零填充大小；重建导出表，保持名称、序号、函数 RVA。
- 逐项复制资源树及资源数据，重建数据项 RVA。
- 将新的导入、导出、资源和重定位放入 `.meta`，移除外壳解码器、压缩数据尾部及外壳导入。

业务代码节暂时保留原壳解压后的可读、可写、可执行属性，以免改变旧插件的自修改行为。这可能触发静态工具的 RWX 启发式提示，不能据此判定仍有压缩壳。本次没有额外修改音效算法、授权逻辑或插件界面。

重建节布局不保证与厂商加壳前文件逐字节相同，也不等于恢复了 C/C++ 源码。恢复结果是标准、可换址加载的 PE32 DLL。

## 4. 本机验证

6 个原始文件及 6 个恢复文件分别普通加载、强制换址加载，共 24 次独立加载：

- `LoadLibrary`、导出调用、模块枚举和卸载全部通过。
- 接口版本、描述文本、模块数量以及 Config/Init/ModifySamples/Quit 回调 RVA 全部一致。
- 消除实际基址差后的业务代码共 743,424 字节，两种加载模式下逐字节一致。
- 145 个资源的标识、语言、代码页及内容在两种加载模式下全部一致。

### OctiMax 报错的测试宿主原因

用户看到的 `loader_probe.exe — abnormal program termination` 在原始与恢复版 OctiMax 中均可重现。静态初始化路径为：CRT 初始化表 `0x72000..0x724cc` → `0x3e180` → `0x3e160` → `WSAStartup(0x0202)`；返回非零后转入 `0x35175` 的 CRT `abort`。

旧测试宿主在 DLL 加载前把 HKCU/HKLM 整体重定向到空白临时分支，连 Winsock 的系统目录配置也被隐藏。修正为先完成系统 Winsock 初始化、再建立插件测试注册表隔离。修正后原始与恢复 OctiMax 的普通加载、换址、代码比较均通过，不需要修改插件机器码。

测试程序另行记录致命 MessageBox 的文本和调用位置，以非零退出结束子进程，避免无人值守时阻塞。错误不会被计作通过。

## 5. 本地证据与复现入口

所有工具、源码和中间文件仅位于 `rebuild/tests`，不加入 Actions 和发行包。

- `tests/pec_capture.cpp`、`build_pec_capture.ps1`：初始化前采集工具。
- `tests/capture_pec_samples.py`：两种基址的采集编排。
- `tests/restore_pec_dsp.py`：标准 PE 重建。
- `tests/verify_restored_dsp.py pec`：本机加载、回调、代码对照。
- `tests/compare_restored_resources.py pec`：资源对照。
- `tests/artifacts/dsp_restore/pec/captures.json`：原始文件散列与捕获位置。
- `tests/artifacts/dsp_restore/pec/restoration.json`：重建元数据、散列与导入清单。
- `tests/artifacts/dsp_restore/pec/validation.json`、`resource-validation.json`：测试结果。

虚拟机结果与最终交付范围见 [DSP DLL 恢复总报告](DSP_DLL_RESTORATION_RESULTS.md)。
