# DSP DLL 打包方式静态检查

检查日期：2026-09-28。对象：`D:\Projects\Backup\TTPlayer\Plugins` 下现有 17 个 DLL。

本次暂停了通用注册表接入工作，仅静态读取文件，未加载、解压写回或运行插件，也未修改任何 DSP DLL。这里的“常规 DLL”指未见压缩/保护壳、常规编译链接形成的 PE 文件；不等于对原始来源、完整性、安全性或全部运行行为的认证。

## 1. 结论

- **5 个常规编译链接 DLL**：`Dsp_Dfx.dll`、`dsp_Dolby.dll`、`dsp_izOzone.dll`、`dsp_skywalker.dll`、`DSP_VAPXP.dll`。
- **5 个明确 UPX 压缩 DLL**：`dsp_compwide.dll`、`dsp_DEE.DLL`、`dsp_iZVinyl.dll`、`dsp_jammix.dll`、`dsp_neq.dll`。
- **6 个明确 PECompact 2 压缩 DLL**：`dsp_DeFX.dll`、`dsp_enh.dll`、`dsp_OctiMax.dll`、`dsp_so3d.dll`、`dsp_sps.dll`、`dsp_superequ.dll`。
- **1 个改名/去标识的 UPX 风格压缩 DLL**：`DSP_ViPER’s Audio.dll`。可以确认有解压层；UPX 家族归属依据入口代码与结构比对，未据此确认精确工具版本。

全部文件均可解析为 x86 PE32 DLL，且导出 `winampDSPGetHeader2`。加壳文件仍是 Windows DLL，不能将“有壳”直接等同于文件异常或无法运行。

## 2. 逐文件结果

“导入数”为磁盘 PE 静态导入条目数，不代表解压后的全部业务依赖。入口地址使用 RVA，即相对模块基址的偏移。

| 文件 | 文件大小（字节） | 分类 | 静态导入数 | DLL 入口 RVA |
| --- | ---: | --- | ---: | --- |
| `dsp_compwide.dll` | 8,192 | UPX，标识明确 | 6 | `0xb990` |
| `dsp_DEE.DLL` | 86,016 | UPX，标识明确 | 6 | `0x9e360` |
| `dsp_DeFX.dll` | 30,720 | PECompact 2，标识明确 | 5 | `0x2aa0` |
| `Dsp_Dfx.dll` | 1,015,808 | 常规编译链接，未见压缩壳 | 238 | `0x8f4e3` |
| `dsp_Dolby.dll` | 110,080 | 常规编译链接，未见压缩壳 | 82 | `0x4724` |
| `dsp_enh.dll` | 45,568 | PECompact 2，标识明确 | 9 | `0x7a36` |
| `dsp_izOzone.dll` | 3,022,923 | 常规编译链接，未见压缩壳 | 184 | `0x3fa8b` |
| `dsp_iZVinyl.dll` | 1,110,528 | UPX，标识明确 | 8 | `0x21cc30` |
| `dsp_jammix.dll` | 242,176 | UPX，标识明确 | 13 | `0x3638c0` |
| `dsp_neq.dll` | 69,120 | UPX，标识明确 | 6 | `0x64890` |
| `dsp_OctiMax.dll` | 203,264 | PECompact 2，标识明确 | 13 | `0x282f9` |
| `dsp_skywalker.dll` | 73,728 | 常规编译链接，未见压缩壳 | 74 | `0x3d33` |
| `dsp_so3d.dll` | 463,872 | PECompact 2，标识明确 | 9 | `0x1000` |
| `dsp_sps.dll` | 22,016 | PECompact 2，标识明确 | 7 | `0x29221` |
| `dsp_superequ.dll` | 85,504 | PECompact 2，标识明确 | 10 | `0xb587` |
| `DSP_VAPXP.dll` | 241,664 | 常规编译链接，未见压缩壳 | 86 | `0xdccc` |
| `DSP_ViPER’s Audio.dll` | 243,200 | UPX 风格压缩，区段改名/标识缺失 | 7 | `0x2c37d0` |

## 3. 判定证据

### 3.1 常规编译链接的 5 个 DLL

这五个文件同时具备以下特征：

1. `.text`、`.rdata`、`.data`、`.rsrc`、`.reloc` 的常规区段布局；Ozone 另有 `.data1`。
2. `.text` 为可读、可执行，未设置可写；代码熵约 6.52–6.74 bit/byte，属于本组可直接反汇编的机器码。
3. 各导入描述符均有非零、与 FirstThunk 分离的 OriginalFirstThunk，磁盘上的函数名称表可读取。
4. DLL 入口能看到普通运行库初始化代码；`winampDSPGetHeader2` 在磁盘上就是清晰的 `mov eax, ...; ret`，能继续定位插件头和回调。
5. 未发现本次检查的 UPX/PECompact 等压缩器标识，也未见本组有壳 DLL 的入口解压流程。

| 文件 | `.text` 熵 | 磁盘中的注册表静态导入 |
| --- | ---: | --- |
| `Dsp_Dfx.dll` | 6.594 | `RegOpenKeyExW`, `RegCreateKeyExW`, `RegOpenKeyExA`, `RegQueryValueExA`, `RegCloseKey`, `RegQueryValueExW`, `RegSetValueExW` |
| `dsp_Dolby.dll` | 6.718 | 未见；仍有动态解析入口，不能据此断定从不访问注册表 |
| `dsp_izOzone.dll` | 6.736 | `RegCloseKey`, `RegQueryValueExA`, `RegOpenKeyExA`, `RegSetValueExA`, `RegCreateKeyExA` |
| `dsp_skywalker.dll` | 6.523 | `RegSetValueExA`, `RegCreateKeyExA`, `RegQueryValueExA`, `RegCloseKey`, `RegOpenKeyExA` |
| `DSP_VAPXP.dll` | 6.719 | 未见；仍有动态解析入口，不能据此断定从不访问注册表 |

DFX 的 `.data` 虚拟大小明显大于文件大小，符合未初始化数据占空间的常见情况，不能单凭这一点判定有壳。Ozone 的 `.data1` 和 75 字节文件尾附加数据也不是压缩壳的充分证据；它的入口、代码和导入表均属于常规布局。

### 3.2 明确 UPX 的 5 个 DLL

五者均有 `UPX0`、`UPX1`、`.rsrc` 区段和 `UPX!` 标识。`UPX0` 的文件大小为 0，但保留较大的虚拟空间；DLL 入口位于 `UPX1`，先执行解压代码。`winampDSPGetHeader2` 则指向磁盘无原始代码的 `UPX0`，必须等解压后才能读取其真实实现。

导入名称表 OriginalFirstThunk 为 0，磁盘可见导入缩减到 6–13 项。仅凭这些壳导入，无法判断插件完整使用了哪些 Reg* API。

### 3.3 明确 PECompact 2 的 6 个 DLL

六者均在入口附近包含 `PECompact2` 字符串，并有 `PEC2` 标记。入口先安装异常处理结构，再进入解压流程；`.text` 具有可写与可执行属性，原始内容熵约 7.85–8.00。多数业务代码在磁盘上不能按导出 RVA 直接解释。

虽然区段也叫 `.text/.rsrc/.reloc`，这些名称不能证明文件未加壳。尤其 `dsp_sps.dll` 的 DLL 入口实际位于 `.rsrc` 内的解压桩。

已有 Enhancer 运行时取证还确认：解压后的壳导入表仍不能作为普通名称表遍历，业务 IAT 与磁盘壳 IAT 不同。其他 PECompact 文件是否采用完全相同的恢复位置和表结构，不能直接套用 Enhancer 的固定 RVA。

### 3.4 ViPER：没有 UPX 字样，仍不是常规裸 DLL

`DSP_ViPER’s Audio.dll` 的区段名为 `SEGX/SEGY/.SEGZ`，未发现 `UPX!`。但它有多项独立证据：

- `SEGX` 的文件大小为 0、虚拟空间约 2.53 MiB；两个导出函数均指向这个待恢复区段。
- DLL 入口位于 `SEGY`；先检查 DLL 调用原因，保存寄存器，然后设置源/目标地址并进入位流解压循环。
- 将绝对地址常量归一化后，入口前 24 条指令与本目录明确 UPX 的 `dsp_compwide.dll` 一致。
- 静态导入仅 7 项，OriginalFirstThunk 全部为 0，符合本组 UPX 壳的缩减布局。

因此，可以确定它带有加载时恢复代码的压缩层；“改名/去标识 UPX 风格”是有代码比对支持的家族判断，不是仅凭区段名猜测。其 `SEGY` 熵约 6.73，与常规代码区相近，也说明不能仅凭熵判断是否加壳。

## 4. 两个容易混淆的文件

- `dsp_DeFX.dll`：30,720 字节，名称含额外的字母 **e**，PECompact 2 压缩，磁盘导入 5 项。
- `Dsp_Dfx.dll`：1,015,808 字节，常规编译链接，磁盘导入 238 项。
两者不是单纯的大小写区别，不能复用同一套固定地址或二进制识别结论。

## 5. 对后续通用 registry.json 接入的影响

1. 五个常规 DLL 可作为标准 PE 导入表适配的第一组，其中 Skywalker 已有清晰的 Reg* 静态导入。
2. Dolby、VAPXP 没有静态 Reg* 导入，不意味着需要制造注册表调用；应兼容它们通过 GetProcAddress 等方式动态取得接口的情况。
3. 其余 12 个应在加载解压后识别业务导入和调用位置，不能只扫描磁盘导入表，也不能把 OriginalFirstThunk 为 0 直接判成无效 DLL。
4. Enhancer 已有固定版本的业务槽适配；这不意味着其他 PECompact 或 UPX 插件可以使用同一组偏移。
5. 接入后还需要分别验证查询、创建、写入、枚举、退出保存和 DLL 卸载，不应只以“Attach 没报错”作为成功标准。

## 6. 本次边界和解析器提示

本次未调用 LoadLibrary，未实际解压或运行插件，也未继续修改通用接入代码。分类结论适用于本目录当前文件；文件哈希与此前静态清单一致。

解析器对 Ozone 的大量零字节和 ViPER 的资源目录重叠给出了启发式警告。它们不是“文件已损坏”的结论，本次不根据这些警告认定插件不能运行。

完整的区段属性、熵、导入描述符、入口反汇编和 SHA-256 已记录在本地 `tests/artifacts/dsp_static/packaging/inspection.json`，分析脚本位于 `tests/inspect_dsp_packaging.py`。本地测试材料不加入 Actions 或发行包。

## 7. 文件指纹

| 文件 | SHA-256 |
| --- | --- |
| `dsp_compwide.dll` | `9d09da800e74d5c37612cdbe1f4191b605c6cc089f77bacdf0c931367717f237` |
| `dsp_DEE.DLL` | `e42858c3246894f4aeb3a410cfbfdd68b8acff121a0a4c0e7b823bf3115137e1` |
| `dsp_DeFX.dll` | `813e4ba53c8f606584b695546d827190857a5b2b3dcd86740c650c122964f7e3` |
| `Dsp_Dfx.dll` | `987e7d531b92df9a582c1ab803f92856e948cc87f0a6b1ed900d3a7898a0eb03` |
| `dsp_Dolby.dll` | `99087a56b1c36ca77f541c9ac0733e1496b59c5eabd9f2f8445523b999e3d338` |
| `dsp_enh.dll` | `55eb2f2dece655a491141376916f4488a6714e221298fab26032d647fff2287f` |
| `dsp_izOzone.dll` | `e3bb0eef979ea8016fb1278b373c7c70ae4507719524bfefe802b5bc3c800e59` |
| `dsp_iZVinyl.dll` | `4607559be14a199e5685566df7b70361ee6aed28bcf84bb2da1b0475b3b02c99` |
| `dsp_jammix.dll` | `25c0c820e2a18c4a2fc6e3afd4619d716dc950525ded0a451c302687ee6a5b7b` |
| `dsp_neq.dll` | `787f559a9412f23dc17255103de0b8a1120521465eee8d0e7691a3891b10df2b` |
| `dsp_OctiMax.dll` | `7af9afab242bd21f2a54664eeaa8fac7166c80b0469eed65b7d01795270a16a4` |
| `dsp_skywalker.dll` | `e67ade4132aa90e0c7b7b4efc5a9a34da05623f14db4517368b3fa7d1f5afc02` |
| `dsp_so3d.dll` | `38e04cec35e8338af82a83cc566698f38882172f2af0e075219c961efa1360b7` |
| `dsp_sps.dll` | `27db79b2076679eb273fb68cc7a067f752258a2fdc95b0e3cc188523babc0025` |
| `dsp_superequ.dll` | `25a410aa8946d7e68e560c4f2ab4ac1dede1d2996ad3d5ee52efd445ab0684ea` |
| `DSP_VAPXP.dll` | `24d974ed1d9f1561bc1eef8cd55c8ceeb377a704974a538af29f9e0107c6ea95` |
| `DSP_ViPER’s Audio.dll` | `bf1fa41fa44658c12754f978bc9bb9c65ca1bf3ce6f578890dd5cda288359aae` |
