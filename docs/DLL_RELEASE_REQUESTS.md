# 手动请求全部 DLL 仓库发布

在播放器仓库的 **Actions → Request DLL Releases → Run workflow** 手动运行。
入口是 `.github/workflows/release-dlls.yml`，只配置 `workflow_dispatch`，没有 push、PR 或定时触发。

默认向清单中的全部 24 个仓库发送请求。每个仓库使用自己的默认分支和现有发布工作流，携带：

```json
{"release_a_version": true, "only_if_unreleased": true}
```

gettext 另传 `configuration: Release`。各项目仍独立分配北京时间日期版本、编译、检查 XP／Win7 导入、打包和发布 GitHub Release。源码包、许可证和既有附件规则沿用各项目自己的流程。

## 配置与首次启用

1. 将本次两个接收端文件提交并推送到下面 **24 个 DLL 仓库的默认分支**：原有工作流和新增的 `cmake/check_component_release.py`。
2. 将播放器仓库的统一工作流、脚本、清单和文档提交推送到默认分支。文件夹与 GitHub 仓库的映射保存在 `cmake/dll_repositories.json`，无需在 Actions 中手填仓库名。
3. 在播放器仓库的 **Settings → Secrets and variables → Actions** 设置 `DLL_RELEASE_TOKEN`。未设置时沿用已有的 `GH_TOKEN`。
4. 该令牌需要访问全部 24 个目标仓库，并具有 **Actions: write、Contents: read**。组织使用 fine-grained PAT 时选择 `TTPlayerRebuild` 作为 resource owner，并授权这些仓库；也可以使用具有相应权限的 GitHub App installation token。遵循组织自身的令牌授权／SSO 设置。
5. 首次可以勾选 **Preview only (do not send release requests)** 查看只读预览；正式请求时取消勾选。预览默认关闭。

跨仓库工作流分发需要 Actions 写权限，播放器仓库自带的 `GITHUB_TOKEN` 不能替代目标仓库的跨仓库授权，见 [GitHub workflow dispatch API](https://docs.github.com/en/rest/actions/workflows#create-a-workflow-dispatch-event)。接收端使用自身 `${{ github.token }}` 读取提交；原发布任务继续使用自身已有的发布凭据和 `contents: write` 权限，不传递播放器的跨仓库令牌。

**必须先部署接收端。** 尚未增加 `only_if_unreleased` 输入的工作流会拒绝该请求；控制器不会退回无条件发布，以免失去“有新提交才打包”的约束。本次本地实现和验证没有替用户提交、推送或触发远端构建／发布。

## 如何判断需要发布

检查发生在目标仓库的原有**工作流级发布并发组**内，位于 Windows 构建和版本分配之前。重复请求会在获得同一发布名额之后重新查询 Release，不依赖控制器较早查询的快照。

取 GitHub 最新正式 Release，排除草稿和预发布，再将 Release 标签解析为实际 Git commit。兼容轻量标签和带注释标签，不把 `target_commitish: master` 误当成发布时的提交。查询规则见 [latest release API](https://docs.github.com/en/rest/releases/releases#get-the-latest-release)。

比较的是这次工作流绑定的 `GITHUB_SHA` 与发布提交的祖先关系，使用 [compare commits API](https://docs.github.com/en/rest/commits/commits#compare-two-commits)，不比较文件修改时间、提交日期或 Release 创建时间。

| 情况 | 处理 |
|---|---|
| 没有任何正式 Release | 构建并创建首次 Release |
| 本次提交与 Release 标签提交相同 | 跳过构建、版本分配、打包和发布 |
| 本次提交是 Release 提交的后代，有新增提交 | 运行既有 Release 流程 |
| 排队期间已有更晚的后代提交发布 | 跳过这个较旧的排队请求 |
| 历史分叉、标签缺失、仓库身份变化、权限／API 错误 | 明确失败，不自动当成“首次发布” |

所有已提交的变动都算新提交，包括文档、工作流和构建脚本。未推送的本地修改不参与判定。条件发布限定在各仓库默认分支；GitHub 事件绑定的 SHA 同时用于检出和创建 Release 标签，构建期间默认分支继续前进不会改变本次源代码。

接收端新增的 `only_if_unreleased` 默认关闭，保留原有单仓库手动构建／发布方式；统一入口始终显式开启它。普通不发布的构建照常可用。

## 查看结果与重试

统一 Action 的 Summary 列出各仓库的请求结果和运行链接，同时保存 `dll-release-requests.json` Artifact。

- `requested` 表示请求已被 GitHub 接收，**不表示 DLL 已经编译或 Release 已发布**。打开对应运行，查看 `Check for unpublished commits`、构建和发布任务的结果。
- `would-build`／`would-skip` 是勾选 Preview 后的只读预测，不会发送请求。
- 一个仓库请求失败后继续处理其他仓库，最后统一 Action 报告失败，保留所有结果。
- POST 请求不自动重试。连接中断可能发生在请求已被接收之后，应先查看目标 Actions，再决定重跑。再次请求已成功发布的提交会由接收端跳过。
- 网络或权限错误不会回退匿名请求，也不会忽略条件直接发布。令牌只通过环境变量使用，不写入 Artifact。

该入口负责请求 DLL 发布，不自动启动播放器构建。需要将这些新版 DLL 打入播放器发行包时，等目标仓库发布完成，再手动运行原有 **Manual Windows Build**。

## 仓库映射

| 本地文件夹 | GitHub 仓库 | 接收工作流 |
|---|---|---|
| gettext | TTPlayerRebuild/TTPlayerI18n | manual-build.yml |
| makivm | TTPlayerRebuild/TTPlayerMaki | build.yml |
| mbedtlsmin | TTPlayerRebuild/TTPlayerHttps | build.yml |
| ttp_aac | TTPlayerRebuild/TTPlayerAAC | build.yml |
| ttp_ac3dts | TTPlayerRebuild/TTPlayer_AC_130_dts | build.yml |
| ttp_ape | TTPlayerRebuild/TTPlayerAPE | build.yml |
| ttp_asf | TTPlayerRebuild/TTPlayerAsf | build.yml |
| ttp_clienc | TTPlayerRebuild/TTPlayerClienc | build.yml |
| ttp_dsd | TTPlayerRebuild/TTPlayerDSD | build.yml |
| ttp_enc | TTPlayerRebuild/TTPlayerEnc | build.yml |
| ttp_flac | TTPlayerRebuild/TTPlayerFLAC | build.yml |
| ttp_lrcsh | TTPlayerRebuild/TTPlayerLrcsh | build.yml |
| ttp_mod | TTPlayerRebuild/TTPlayerMod | build.yml |
| ttp_mpc | TTPlayerRebuild/TTPlayerMpc | build.yml |
| ttp_ogg | TTPlayerRebuild/TTPlayerOGG | build.yml |
| ttp_osu | TTPlayerRebuild/TTPlayerOSU | build.yml |
| ttp_rm | TTPlayerRebuild/TTPlayerRm | build.yml |
| ttp_tak | TTPlayerRebuild/TTPlayerTak | build.yml |
| ttp_vqf | TTPlayerRebuild/TTPlayerVqf | build.yml |
| ttp_webm | TTPlayerRebuild/TTPlayerWebM | build.yml |
| ttpbrowser | TTPlayerRebuild/TTPlayerBrowser | build.yml |
| ttpcomm | TTPlayerRebuild/TTPlayerComm | build.yml |
| ttpres | TTPlayerRebuild/TTPlayerRes | build.yml |
| waskin | TTPlayerRebuild/TTPlayerWaskin | manual-build.yml |

## 维护和本地验证

`cmake/check_component_release.py` 的维护源在播放器仓库，各 DLL 仓库携带字节相同的副本，以便独立构建；运行时不下载另一个仓库中可变的脚本。修改条件检查后，同步该文件到清单中各仓库，并一起提交对应变动。

本地验证材料位于 `rebuild/tests/dll_release_dispatch_20261105`。已覆盖相同／领先／落后／分叉提交、首次发布、注释标签、缺失标签和权限错误、重复请求、POST 不重试、预览不写入、24 个仓库的映射及接线、原有构建和发布任务保持原样等情况。35 项检查通过，25 份变更后的工作流通过 Actionlint 1.7.12。

2026-11-05 对全部 24 个远端仓库完成只读预览：12 个需要构建、12 个跳过，0 个查询失败。验证入口明确禁止 POST，未触发任何远端构建或发布。该结果只代表查询时的已推送提交；部署本次工作流修改后，新增提交会参与下一次条件检查。

测试代码和测试依赖只留在本地 tests，未加入仓库交付文件、发行包或 Actions。本次只调整工作流调度，没有修改 DLL 源码、版本规则或 XP／Win7 构建选项。
