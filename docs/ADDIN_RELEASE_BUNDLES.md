# Release 插件包

更新日期：2026-11-10。

## 下载与安装

在 **Actions → Manual Windows Build** 中，每次播放器构建成功后都会下载插件仓库的最新正式版，
生成 `CodecAddIn-Rebuild` 和 `ExtraAddIn` 两组插件包，连同播放器包上传到本次运行的 **Artifacts**，
保留 14 天。勾选 **Release a Version (GitHub)** 或 **Publish Release to Gitee** 时，
还会向相应平台发布同一组文件；同时选中两个平台时，插件只下载和打包一次。

最终 Release 有五个附件：

```text
TTPlayerRebuild-yyyy.MM.dd[pN].zip
TTPlayer-yyyy.MM.dd[pN].zip
CodecAddIn-Rebuild-yyyy.MM.dd[pN].zip
ExtraAddIn-yyyy.MM.dd[pN].zip
SHA256SUMS.txt
```

两个插件包的文件名使用播放器最终发行版本，包括同日 `pN`。
DLL 保留各自上游版本和原始字节。仅构建的运行同样生成两种命名的播放器包、两个插件包和外部校验清单。

退出播放器后解压所需插件包到所选播放器 EXE 所在目录，合并 `AddIn`。
ExtraAddIn 的 `i18n` 目录也放在 EXE 旁，用于多语言插件读取翻译。
ExtraAddIn 根目录的 `ttpbrowser.dll` 同样放在 EXE 旁，
用于音乐浏览器与下载窗口。
播放器自动更新器按发起程序名称选择对应主包，更新包内全部运行文件；两个可选插件包由用户单独下载安装。

## CodecAddIn-Rebuild

| 仓库 | 包内 DLL |
| --- | --- |
| [TTPlayerWebM](https://github.com/TTPlayerRebuild/TTPlayerWebM) | `AddIn/ttp_webm.dll` |
| [TTPlayerAPE](https://github.com/TTPlayerRebuild/TTPlayerAPE) | `AddIn/ttp_ape.dll` |
| [TTPlayerFLAC](https://github.com/TTPlayerRebuild/TTPlayerFLAC) | `AddIn/ttp_flac.dll` |
| [TTPlayerEnc](https://github.com/TTPlayerRebuild/TTPlayerEnc) | `AddIn/ttp_enc.dll` |
| [TTPlayerOGG](https://github.com/TTPlayerRebuild/TTPlayerOGG) | `AddIn/ttp_ogg.dll` |
| [TTPlayerAAC](https://github.com/TTPlayerRebuild/TTPlayerAAC) | `AddIn/ttp_aac.dll` |
| [TTPlayerMpc](https://github.com/TTPlayerRebuild/TTPlayerMpc) | `AddIn/ttp_mpc.dll` |
| [TTPlayer_AC_130_dts](https://github.com/TTPlayerRebuild/TTPlayer_AC_130_dts) | `AddIn/ttp_ac3dts.dll` |
| [TTPlayerMod](https://github.com/TTPlayerRebuild/TTPlayerMod) | `AddIn/ttp_mod.dll` |
| [TTPlayerClienc](https://github.com/TTPlayerRebuild/TTPlayerClienc) | `AddIn/ttp_clienc.dll` |
| [TTPlayerAsf](https://github.com/TTPlayerRebuild/TTPlayerAsf) | `AddIn/ttp_asf.dll` |
| [TTPlayerTak](https://github.com/TTPlayerRebuild/TTPlayerTak) | `AddIn/ttp_tak.dll` |
| [TTPlayerRm](https://github.com/TTPlayerRebuild/TTPlayerRm) | `AddIn/ttp_rm.dll` |
| [TTPlayerVqf](https://github.com/TTPlayerRebuild/TTPlayerVqf) | `AddIn/ttp_vqf.dll` |
| [TTPlayerDSD](https://github.com/TTPlayerRebuild/TTPlayerDSD) | `AddIn/ttp_dsd.dll` |

包内仅有以上 15 个 DLL 和 `SHA256SUMS.txt`。这是编解码插件合集，不包含第三方 DSP 滤镜。
新增八项的仓库名取自各工程 Git origin；特别是 AC3/DTS 的仓库名为 `TTPlayer_AC_130_dts`，
其运行附件仍使用 `ttp_ac3dts-上游版本.zip`。

## ExtraAddIn

| 仓库 | 包内内容 |
| --- | --- |
| [TTPlayerWaskin](https://github.com/TTPlayerRebuild/TTPlayerWaskin) | `AddIn/ttp_waskin.dll` |
| [TTPlayerMaki](https://github.com/TTPlayerRebuild/TTPlayerMaki) | `AddIn/ttp_maki.dll` |
| [TTPlayerI18n](https://github.com/TTPlayerRebuild/TTPlayerI18n) | `AddIn/ttp_i18n.dll` 和 `i18n` 翻译资源 |
| [TTPlayerOSU](https://github.com/TTPlayerRebuild/TTPlayerOSU) | `AddIn/ttp_osu.dll` |
| [TTPlayerBrowser](https://github.com/TTPlayerRebuild/TTPlayerBrowser) | 根目录 `ttpbrowser.dll` |

保留上游已校验的 `i18n/README.md`、`i18n/ttplayer.pot`、
`i18n/<语言>/ttplayer.po`／`.mo`，并要求简体中文、繁体中文和英文目录均有翻译。
不复制本机的语言文件、播放器设置或皮肤包。包内另有覆盖所有运行文件的 `SHA256SUMS.txt`。

## 下载和校验

实现入口：[package_addin_bundles.py](../cmake/package_addin_bundles.py)。仅使用 Python 标准库，
不需要检出插件源码、安装额外 Python 包或构建这些 DLL。

1. 每个仓库只请求一次 GitHub `/releases/latest`，接受正式发布的有效日期版本，拒绝草稿／预发行。
2. 精确选择 `ttp_名称-上游版本.zip`；I18n 为 `ttp_i18n-x86-上游版本.zip`。
   Browser 为 `ttpbrowser-上游版本.zip`，提取根目录 `ttpbrowser.dll`；第三方声明保存在浏览器仓库。
   AAC、AC3/DTS、TAK、RM、VQF、DSD 等组件的 `-source.zip` 和 GitHub 自动源码附件不会被选中。
3. 保存本次选中的标签、Release ID、附件 ID、地址及摘要。后续使用该标签的下载地址，
   并验证 GitHub API 提供的 ZIP SHA-256 和长度；同一任务中不再次切换至后来发布的版本。
4. 检查 ZIP 路径、重复条目、文件类型、大小上限、CRC 及包内 SHA-256。
   打包约定的 DLL、翻译资源及清单。既有浏览器运行 ZIP 中的声明先参与完整源包校验，再从合集及其文件清单中移除。
   拒绝目录越界、未知文件、符号链接和加密 ZIP。
5. 静态检查每个 DLL 为 x86 PE32、XP 可加载的系统／子系统版本；复用 YY-Thunks
   的 XP 5.1.2600 和 Win7 6.1.7600 导出表审计导入，不执行下载的 DLL。
   浏览器另外检查 `ttp_browser_get_api` 导出入口。
6. 全部组件验证成功后才输出两个完整插件 ZIP。下载或校验失败会使本次打包失败，
   不发布缺少插件的合集，也不从已有本地 DLL 回退。
   上游返回 404 时明确显示仓库名及“先发布日期版本运行 ZIP”的提示。
7. 为两个 ZIP 内的文件重新生成 SHA-256；外部 Release 清单同时校验播放器及两个插件 ZIP。
   准备任务和两个发布任务均在上传前复核四个 ZIP。

网络错误和临时服务错误最多重试 3 次。Actions 通过 `${{ secrets.GH_TOKEN }}` 注入 `GH_TOKEN` 环境变量，认证读取 GitHub API，
Token 只发给 GitHub API，不进入 ZIP、日志、来源记录或附件下载请求，跨站重定向时移除认证头。

来源保存在 Actions Artifact 的 `addin-bundles.json`，同时嵌入 `build-info.json`。
Release 说明自动列出每个插件的实际版本及对应上游 Release 链接。
这些记录不额外混入插件运行包。两组 ZIP 使用固定顺序和时间戳；相同的上游文件生成相同 ZIP。

### 2026-11-10 浏览器组件接入与验证

ExtraAddIn 现有五个组件。浏览器使用 `TTPlayerRebuild/TTPlayerBrowser` 的最新正式 Release，
选取 `ttpbrowser-日期版本.zip`，收录根目录 `ttpbrowser.dll`。
第三方声明保存在浏览器仓库的 `THIRD_PARTY_NOTICES.md`。新运行包包含 DLL 和校验清单，
旧运行包附带的声明仍参与源包 SHA256 校验，ExtraAddIn 及其文件清单仅收录浏览器 DLL。
浏览器仍接受 x86 PE32、XP 系统／子系统版本和 XP／Win7 导入审计，并要求导出 `ttp_browser_get_api`。

首次接入时查询该公开仓库的 Release 列表为空，`/releases/latest` 返回 404。
需要先在浏览器仓库运行 **Build TTPlayer music window**，勾选 **Release a Version (GitHub)**，
发布首个含上述运行 ZIP 的正式版。播放器打包遇到此情况会明确显示 `TTPlayerBrowser` 缺少正式版。

本地验证使用浏览器工程现有的 `ttpbrowser-2026.10.29.zip`，测试用 Release 元数据明确标为模拟数据。
该 ZIP 中 DLL 的 SHA256 为
`dce223916b8dd4968d2d6fca2a94d948dd7c7ae5f1eb6c9d5e528007df60f0df`。
20 项回归全部通过，覆盖完整 20 组件合集、确定性打包、来源只解析一次、根目录安装布局、
新旧声明布局、声明排除及源包校验、两文件浏览器运行包验证、错误路径、摘要错误、缺少导出、x64／高版本子系统／不兼容导入，
以及浏览器下载失败时保留上一份完整输出。

从测试生成的 ExtraAddIn 根目录重新取出的同一 DLL，在本机 Windows 11 和 VirtualBox MCP 的
XP、Win7 上均通过动态加载、API 1 前缀与必要回调查询、卸载检查；两个虚拟机退出码均为 0。
本次运行验证范围为 DLL 加载与接口兼容性。Actionlint、14 段工作流 PowerShell 语法和四种发布开关组合均通过。
源码、夹具和日志保存在 `tests/extra_browser_20261110`，测试继续仅在本地运行。

声明范围调整的追加日志保存在 `tests/browser_notices_20261110`。
已通过浏览器 `build.ps1 -Package` 实际构建两文件运行 ZIP，完成 16 DLL／381 imports 的 XP／Win7 审计，
再用该 ZIP 生成完整合集，核对 ExtraAddIn 内容、SHA256 清单及来源记录中的浏览器载荷均为 DLL。

## 本地命令

在 `rebuild` 目录运行，先准备好播放器构建使用的 YY-Thunks 导出表：

```powershell
python cmake/package_addin_bundles.py --version 2026.10.20 --output build/Release `
  --exports build/_deps/ttplayer_yy_thunks-src/Config/x86/5.1.2600.txt `
  --exports build/_deps/ttplayer_yy_thunks-src/Config/x86/6.1.7600.txt
```

使用本地依赖缓存时将两个 `--exports` 路径换成缓存的对应文件即可。
该命令输出两个插件 ZIP 和 `addin-bundles.json`。外部覆盖四个 ZIP 的 `SHA256SUMS.txt`
由 Actions 在播放器和两个插件包均生成后统一写入，避免覆盖本地已有播放器的校验记录。

## 2026-10-06 验证

已实际请求 9 个公开仓库并下载、校验、审计和打包。所选版本：

| 组件 | 实际上游版本 |
| --- | --- |
| WebM、APE、FLAC、Enc、Waskin、Maki、I18n | `2026.10.06` |
| OGG | `2026.10.03` |
| AAC | `2026.09.23` |

因此合集日期相同不意味着插件自己的日期也相同；每次新 Release 会重新查询这些仓库。

本地输出：

| 文件 | 字节数 | SHA-256 |
| --- | ---: | --- |
| [CodecAddIn-Rebuild-2026.10.06.zip](../build/Release/CodecAddIn-Rebuild-2026.10.06.zip) | 1,054,142 | `fc4edd31818e314bf096568b7ed7e59131f40302e1164fa47b342daa1b0107ee` |
| [ExtraAddIn-2026.10.06.zip](../build/Release/ExtraAddIn-2026.10.06.zip) | 548,618 | `53bf4a99ee8683a4a7e0322e386536dd7ae63c2326f64a3e5a3d6e6176121308` |

已完成以下检查：

- Actionlint 通过；12 段工作流 PowerShell 语法通过。
- 12 项本地打包回归覆盖真实 9 个发行包、源码附件排除、日期／补丁边界、重复／越界路径、
  摘要错误、翻译缺失、架构／导入不兼容、相同输入的重复打包，以及最后一个组件失败时不输出半成品。
- 离线执行实际发布脚本，模拟 GitHub／Gitee CLI：两端均上传三个 ZIP 和一份外部清单，
  Release 说明包含 9 个插件；校验清单损坏、缺项、重复时在发布调用前拒绝。
- 直接编译生产更新解析代码，确认 GitHub／Gitee 含四附件及三条校验记录时，仍精确选择播放器包。

新增测试及下载样本仅位于本地 `tests/addin_bundles`，不进入 Actions 和发行包。
本次验证未创建远程 Release，也未重新执行这些已发布插件的完整播放／界面测试。

## 2026-10-20 新插件接入与验证

新增 MPC、AC3/DTS、MOD、CLI Encoder、ASF、TAK、RM、VQF 八项，编解码合集从 6 个扩为
14 个 DLL；ExtraAddIn 保持 3 个组件。GitHub／Gitee 发行说明和来源表随新清单更新。
构建仍从上游下载已发布 DLL，不编译这些独立插件，也不运行私有测试。

实际通过 GitHub API 核对了所有仓库：八个新仓库均存在，但其 `/releases/latest` 均返回 404，
尚无可供合集下载的正式版。因此**需要先在各新插件工程发布首个正式 Release，再执行播放器发布**。
每个 Release 使用日期标签和对应的 `ttp_名称-标签.zip`；仅上传源码或只创建 Git tag 不够。
播放器 Action 会按仓库名给出失败原因，不会跳过这些组件。此次未创建远程 Release。

本地验证将两种来源明确分开：

- 原有 9 个组件：实际下载当前正式 Release 并校验。Waskin 为 `2026.10.13`，其余版本同上节表格。
- 新增 8 个组件：使用 2026-10-19 已构建的真实运行 ZIP，配合仅供测试的模拟 Release 元数据；
  不把这些元数据或生成的测试合集作为远程已发布产物。
- 17 个 DLL 的包内外 SHA-256、x86 PE32、XP／Win7 导入审计通过。
- 13 项本地打包回归通过，覆盖源码附件排除、重复打包一致性、缺失上游保留既有输出、
  最后一个组件损坏时不输出不完整合集，以及原有路径、摘要和翻译检查。
- Actionlint 与 14 段 PowerShell 语法检查通过；以模拟发布命令执行工作流的准备与发布步骤，
  确认 GitHub／Gitee 均保留三个 ZIP 和一份外部校验清单，发行说明列出 17 个插件。

本轮测试、缓存与模拟附件仅在 `rebuild/tests/addin_bundles`，没有加入 Actions 或发行包。

## 2026-10-23 DSD 接入与验证

新增 `TTPlayerDSD → AddIn/ttp_dsd.dll`，编解码合集为 15 个 DLL，加上 ExtraAddIn 共 18 个组件。
DSD 支持 DSF／DSDIFF，解码为 PCM 后进入播放器现有处理链。发行说明同步列出该插件。
运行附件精确匹配 `ttp_dsd-上游日期版本.zip`，不选取 `-source.zip`，不需要在播放器 Action 中编译 DSD。

已核对本地 Git origin 与 GitHub 仓库 `TTPlayerRebuild/TTPlayerDSD`，仓库公开且存在；
本次核对时 `/releases/latest` 返回 404。因此需先在 DSD 工程执行 **Release a Version**，
发布带日期标签的运行 ZIP，再执行播放器发布。缺失上游时仍明确报错，不生成遗漏 DSD 的合集。

本地使用已构建的 `ttp_dsd-2026.10.23.zip` 和明确标注为模拟的 Release 元数据，
沿用其余组件的本地验证缓存完成 13 项打包回归：15／3 项分包、运行附件选择、
包内外 SHA-256、XP／Win7 静态导入审计、重复打包一致性及失败时保留原输出均通过。
测试、缓存和模拟合集留在 `rebuild/tests/addin_bundles`，不提交、不进入 Action 或发行包。
本次没有发布远程 Release，也没有重新执行插件播放测试。
