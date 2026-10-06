# DLL 日期版本与体积优先构建（2026-10-06）

## 完成范围

本次覆盖 makivm、mbedtlsmin、gettext、ttp_aac、ttp_ape、ttp_enc、ttp_flac、ttp_ogg、ttp_webm、waskin 共 10 个独立 DLL 项目。

补齐 MAKI、gettext、waskin 缺失的 PE 版本资源；其余 7 个项目保留已有的日期版本。所有项目的 Release 构建现在明确以文件体积为优先目标。

## 日期版本与 Actions

- 新增 3 个项目的版本生成、验证脚本及资源模板；文件版本／产品版本使用北京时间 `yyyy.MM.dd[pN]`，固定数字版本使用 `年.月.日.补丁号`。
- MAKI 新增独立手动 Actions 和 `Release a Version` 流程；无需检出播放器或其他插件仓库。
- gettext 将最终补丁号分配提前至编译前。之前仅在发布准备阶段改 ZIP 名称，现在 DLL 版本、ZIP 和标签一致。
- waskin 将现有日期／补丁号传入 CMake，打包前验证实际 DLL 版本。
- 日期分配扫描已有标签和 Release（含草稿），按当天最大补丁号递增，发布流程串行避免本工作流重复分配。
- 每个项目继续独立构建、独立发布。Actions 不编译、不运行测试；gettext 的 Actions 测试步骤已移除。
- 运行 ZIP 保持 `AddIn/插件.dll` 与 `SHA256SUMS.txt`；gettext 另外保留原有的翻译目录。许可证保留在各自仓库。

## 体积优先配置

每个项目拥有独立的 `cmake/size_release.cmake`，在创建插件和静态编解码库之前引入：

- `/O1 /Os`：优先生成较小代码。
- `/Gy /Gw /GF`：允许函数、数据和相同字符串参与链接优化。
- Release 跨模块优化（IPO／LTCG），`/OPT:REF /OPT:ICF /INCREMENTAL:NO`：删除未引用代码、合并相同实现。
- Release `/GR- /DEBUG:NONE`：本范围内无运行时类型识别依赖，关闭 RTTI；不生成 Release PDB。
- 按同一份源码实测 `/Ob0`、`/Ob1`、`/Ob2`，选取体积最小的配置；同体积时保留 `/Ob2`。
- 保留浮点精度、异常处理、XP 兼容初始化、现有导出接口，以及 VC-LTL 5.3.1／YY-Thunks 1.2.2 配置。未引入压缩壳或删减格式支持。

下表为同一源码、同一工具链的内联策略对比，单位为字节。工具链为本地 Visual Studio 18 2026，x86 Release。

| 项目 | /Ob0 | /Ob1 | /Ob2 | 默认 |
|---|---:|---:|---:|---|
| makivm | 78,336 | 65,536 | 65,536 | /Ob2 |
| mbedtlsmin | 430,592 | 421,888 | 422,400 | /Ob1 |
| gettext | 130,048 | 116,224 | 115,712 | /Ob2 |
| ttp_aac | 380,416 | 369,152 | 369,664 | /Ob1 |
| ttp_ape | 252,416 | 240,640 | 239,616 | /Ob2 |
| ttp_enc | 318,464 | 309,248 | 310,272 | /Ob1 |
| ttp_flac | 166,912 | 156,672 | 156,160 | /Ob2 |
| ttp_ogg | 349,184 | 341,504 | 340,480 | /Ob2 |
| ttp_webm | 399,872 | 379,904 | 378,880 | /Ob2 |
| waskin | 608,768 | 562,688 | 562,176 | /Ob2 |

这些选择仅表示当前源码和工具链下三种策略中最小；升级编译器或编解码库后可重新测量。

## 本地产物前后对比

“修改前”列为开始本任务时已有的本地 DLL；其中 waskin 等产物可能早于当前源码，因此此表是现有文件对比，不应把全部差值都归因于编译参数。上节才是同源码的受控对比。

| 项目 / DLL | 修改前字节 | 当前字节 | 减少字节 |
|---|---:|---:|---:|
| makivm / `ttp_maki.dll` | 77,824 | 65,536 | 12,288 |
| mbedtlsmin / `ttp_https.dll` | 423,424 | 421,888 | 1,536 |
| gettext / `ttp_i18n.dll` | 130,048 | 115,712 | 14,336 |
| ttp_aac / `ttp_aac.dll` | 369,664 | 369,152 | 512 |
| ttp_ape / `ttp_ape.dll` | 239,616 | 239,616 | 0 |
| ttp_enc / `ttp_enc.dll` | 310,272 | 309,248 | 1,024 |
| ttp_flac / `ttp_flac.dll` | 156,160 | 156,160 | 0 |
| ttp_ogg / `ttp_ogg.dll` | 340,480 | 340,480 | 0 |
| ttp_webm / `ttp_webm.dll` | 378,880 | 378,880 | 0 |
| waskin / `ttp_waskin.dll` | 731,648 | 562,176 | 169,472 |

合计比任务开始时的现有 DLL 减少 **199,168 字节（194.5 KiB）**。已充分优化的 APE、FLAC、OGG、WebM 体积保持不变。

## 已完成验证

1. 10 个项目独立 Release 构建与打包成功。最终版本字符串均为 `2026.10.06`，固定数字版本为 `2026.10.6.0`。
2. 导出名称／序号与原有 DLL 一致；所有 DLL 均为 x86、子系统版本 5.01，XP／Win7 导入审计通过，审计中的哈希匹配最终文件。
3. ZIP 内容、包内 DLL、内部／外部 SHA-256 一致；无测试文件或调试符号进入包。
4. 新增版本脚本检查无效日期、`p0`、`p65536`、合法上限 `p65535`；模拟已有 p2 和草稿 p3 时分配 p4，版本资源 p1 映射正确。PowerShell 脚本及工作流内嵌脚本语法检查通过。
5. 本机、XP、Win7 均完成 10 个 DLL 加载与接口检查：音频工厂枚举、MAKI／皮肤 API、gettext Unicode 词条查找、HTTPS API 和取消／释放。XP／Win7 另外通过完整 MAKI 字节码、类型转换、嵌套调用与边界检查。
6. 12 组音频样本（AAC／HE／HEv2／多声道／分片 MP4、APE、FLAC、TTA、Vorbis、Opus、WebM Vorbis／Opus）优化前后 PCM 哈希一致。ENC 13 组编码及回读／标签检查通过。
7. 当前皮肤测试程序通过 WSZ ZIP／BMP、三个窗口、控件命令、畸形输入及 30 次生命周期检查。旧预编译测试工具的头文件与当前 API 不一致，已在本地用当前头文件重编译后验证。
8. 优化后 HTTPS DLL 完成实际本地 TLS 1.2／1.3 握手；有效证书成功，未知 CA 和主机名不匹配均拒绝。临时证书只作为本次请求的测试信任源，未导入系统证书库。

验证范围为上述构建与回归样本；本次没有运行 GitHub Actions／发布 Release，也没有在虚拟机重跑所有音频格式、所有皮肤及完整播放器交互。

本次新增测试源码、日志、样本和临时构建均位于本地 `rebuild/tests/dll_size_versions`，ENC 复用本地 `rebuild/tests/enc_rebuild`。测试不提交、不进入 Actions。

## 发行包位置

所有项目均保留各自独立的 `build/Release` 输出，以下为本次实际构建的 ZIP：

| 项目 | 发行包 | DLL SHA-256 |
|---|---|---|
| makivm | [ttp_maki-2026.10.06.zip](../../makivm/build/Release/ttp_maki-2026.10.06.zip) | `9ad84488c88348e24f080a68dc81f30e4308cf1351f849700b6ed84d7fb1e494` |
| mbedtlsmin | [ttp_https-2026.10.06.zip](../../mbedtlsmin/build/Release/ttp_https-2026.10.06.zip) | `4d00112682029cf2e0030613fbb16e96cc78b70abf23aea786bffee746453584` |
| gettext | [ttp_i18n-x86-2026.10.06.zip](../../gettext/build/Release/ttp_i18n-x86-2026.10.06.zip) | `5280af389287ff941b5653cb4a453f2c3aee6ad89cee880f207e4a64b460d9bc` |
| ttp_aac | [ttp_aac-2026.10.06.zip](../../ttp_aac/build/Release/ttp_aac-2026.10.06.zip) | `670948094e2b79b6aa50368d32164d563622c24a8ee53f2870437cac326428c8` |
| ttp_ape | [ttp_ape-2026.10.06.zip](../../ttp_ape/build/Release/ttp_ape-2026.10.06.zip) | `508de60e0b9d8640872c1edcdb70cf730678416fd1f839306dd2ab292e8cfd80` |
| ttp_enc | [ttp_enc-2026.10.06.zip](../../ttp_enc/build/Release/ttp_enc-2026.10.06.zip) | `293402ebf28662fc54261bf36e9aadbb0334f4aa2717dce9e915787787a9d541` |
| ttp_flac | [ttp_flac-2026.10.06.zip](../../ttp_flac/build/Release/ttp_flac-2026.10.06.zip) | `00154706bb1129241505b81fb05ef73b260460949bd1ad76b0fc3e15dec5bebf` |
| ttp_ogg | [ttp_ogg-2026.10.06.zip](../../ttp_ogg/build/Release/ttp_ogg-2026.10.06.zip) | `38e77bbba7bde365eb99ddc8b0ac755339465fa33d35593f3c96086afa292983` |
| ttp_webm | [ttp_webm-2026.10.06.zip](../../ttp_webm/build/Release/ttp_webm-2026.10.06.zip) | `9d65b32d7153b775f624f72421b228e8f39f8e82bd41f41f124fb520d3a2e8a4` |
| waskin | [ttp_waskin-2026.10.06.zip](../../waskin/build/Release/ttp_waskin-2026.10.06.zip) | `c5ee4af0f35ce62a49de6a59d5380ab97368e06318fb25946c237bbea63387c0` |

新补齐项目的本地构建入口均支持 `./build.ps1 -Package`；固定补丁版本示例：`./build.ps1 -Package -PackageVersion 2026.10.06p1`。直接使用 CMake 时可设置各项目的 `TTP_*_BUILD_VERSION`，并用 `TTP_SIZE_INLINE_LEVEL` 复测内联策略。
