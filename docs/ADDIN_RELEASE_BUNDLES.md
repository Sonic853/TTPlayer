# Release 插件包

更新日期：2026-10-06。

## 下载与安装

在 **Actions → Manual Windows Build** 勾选 **Release a Version (GitHub)** 或
**Publish Release to Gitee** 时，播放器构建成功后会下载下面 9 个仓库的最新正式版，
分成两组插件包。两端发布同一组文件；同时选中两个平台时，插件只下载和打包一次。

最终 Release 有四个附件：

```text
TTPlayerRebuild-yyyy.MM.dd[pN].zip
CodecAddIn-Rebuild-yyyy.MM.dd[pN].zip
ExtraAddIn-yyyy.MM.dd[pN].zip
SHA256SUMS.txt
```

两个插件包的文件名使用播放器最终发行版本，包括同日 `pN`。
DLL 保留各自上游版本和原始字节。仅构建、不发布的运行仍只生成原有播放器包。

退出播放器后解压所需插件包到 `TTPlayerRebuild.exe` 所在目录，合并 `AddIn`。
ExtraAddIn 的 `i18n` 目录也放在 EXE 旁，用于多语言插件读取翻译。
播放器自动更新器继续只更新播放器／更新器，插件包由用户单独下载安装。

## CodecAddIn-Rebuild

| 仓库 | 包内 DLL |
| --- | --- |
| [TTPlayerWebM](https://github.com/TTPlayerRebuild/TTPlayerWebM) | `AddIn/ttp_webm.dll` |
| [TTPlayerAPE](https://github.com/TTPlayerRebuild/TTPlayerAPE) | `AddIn/ttp_ape.dll` |
| [TTPlayerFLAC](https://github.com/TTPlayerRebuild/TTPlayerFLAC) | `AddIn/ttp_flac.dll` |
| [TTPlayerEnc](https://github.com/TTPlayerRebuild/TTPlayerEnc) | `AddIn/ttp_enc.dll` |
| [TTPlayerOGG](https://github.com/TTPlayerRebuild/TTPlayerOGG) | `AddIn/ttp_ogg.dll` |
| [TTPlayerAAC](https://github.com/TTPlayerRebuild/TTPlayerAAC) | `AddIn/ttp_aac.dll` |

包内仅有以上 6 个 DLL 和 `SHA256SUMS.txt`。这是编解码插件合集，不包含第三方 DSP 滤镜。

## ExtraAddIn

| 仓库 | 包内内容 |
| --- | --- |
| [TTPlayerWaskin](https://github.com/TTPlayerRebuild/TTPlayerWaskin) | `AddIn/ttp_waskin.dll` |
| [TTPlayerMaki](https://github.com/TTPlayerRebuild/TTPlayerMaki) | `AddIn/ttp_maki.dll` |
| [TTPlayerI18n](https://github.com/TTPlayerRebuild/TTPlayerI18n) | `AddIn/ttp_i18n.dll` 和 `i18n` 翻译资源 |

保留上游已校验的 `i18n/README.md`、`i18n/ttplayer.pot`、
`i18n/<语言>/ttplayer.po`／`.mo`，并要求简体中文、繁体中文和英文目录均有翻译。
不复制本机的语言文件、播放器设置或皮肤包。包内另有覆盖所有运行文件的 `SHA256SUMS.txt`。

## 下载和校验

实现入口：[package_addin_bundles.py](../cmake/package_addin_bundles.py)。仅使用 Python 标准库，
不需要检出插件源码、安装额外 Python 包或构建这些 DLL。

1. 每个仓库只请求一次 GitHub `/releases/latest`，接受正式发布的有效日期版本，拒绝草稿／预发行。
2. 精确选择 `ttp_名称-上游版本.zip`；I18n 为 `ttp_i18n-x86-上游版本.zip`。
   AAC 的 `-source.zip` 和 GitHub 自动源码附件不会被选中。
3. 保存本次选中的标签、Release ID、附件 ID、地址及摘要。后续使用该标签的下载地址，
   并验证 GitHub API 提供的 ZIP SHA-256 和长度；同一任务中不再次切换至后来发布的版本。
4. 检查 ZIP 路径、重复条目、文件类型、大小上限、CRC 及包内 SHA-256。
   只读取约定的 DLL、翻译资源及清单，拒绝目录越界、未知 DLL、符号链接和加密 ZIP。
5. 静态检查每个 DLL 为 x86 PE32、XP 可加载的系统／子系统版本；复用 YY-Thunks
   的 XP 5.1.2600 和 Win7 6.1.7600 导出表审计导入，不执行下载的 DLL。
6. 9 个组件全部验证成功后才输出两个完整插件 ZIP。下载或校验失败会使发布准备失败，
   不发布缺少插件的合集，也不从已有本地 DLL 回退。
7. 为两个 ZIP 内的文件重新生成 SHA-256；外部 Release 清单同时校验播放器及两个插件 ZIP。
   准备任务和两个发布任务均在上传前复核三个 ZIP。

网络错误和临时服务错误最多重试 3 次。Actions 通过 `${{ secrets.GH_TOKEN }}` 注入 `GH_TOKEN` 环境变量，认证读取 GitHub API，
Token 只发给 GitHub API，不进入 ZIP、日志、来源记录或附件下载请求，跨站重定向时移除认证头。

来源保存在 Actions Artifact 的 `addin-bundles.json`，同时嵌入 `build-info.json`。
Release 说明自动列出每个插件的实际版本及对应上游 Release 链接。
这些记录不额外混入插件运行包。两组 ZIP 使用固定顺序和时间戳；相同的上游文件生成相同 ZIP。

## 本地命令

在 `rebuild` 目录运行，先准备好播放器构建使用的 YY-Thunks 导出表：

```powershell
python cmake/package_addin_bundles.py --version 2026.10.06 --output build/Release `
  --exports build/_deps/ttplayer_yy_thunks-src/Config/x86/5.1.2600.txt `
  --exports build/_deps/ttplayer_yy_thunks-src/Config/x86/6.1.7600.txt
```

使用本地依赖缓存时将两个 `--exports` 路径换成缓存的对应文件即可。
该命令输出两个插件 ZIP 和 `addin-bundles.json`。外部覆盖三个 ZIP 的 `SHA256SUMS.txt`
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
