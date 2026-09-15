# 手动构建

## GitHub Actions

本地 `rebuild` 是独立 Git 仓库；它在 GitHub 上就是仓库根目录。
工作流位于 `.github/workflows/manual-build.yml`，不配置 push/PR 自动触发，
默认只构建；可勾选 **Release a Version** 在构建成功后发布版本，不推送源码。

1. 将工作流及配套构建文件提交到远程仓库的默认分支。
2. 打开 **Actions → Manual Windows Build → Run workflow**。
3. 选择分支及配置：`Release`（默认）、`RelWithDebInfo` 或 `Debug`。
4. 构建完成后，从该次运行的 **Artifacts** 下载
   `TTPlayer-Windows-x86-配置-运行编号`，产物保留 14 天。
   其中包含 `TTPlayerRebuild-yyyy.MM.dd.zip` 和
   `TTPlayerRebuild-XP-Win7-yyyy.MM.dd.zip`，日期取构建开始时的北京时间。

如需发布，选择 `Release` 配置并勾选 **Release a Version**，不需要输入版本号。
在构建任务开始时记录北京时间（UTC+08:00）的日期，版本号和 Release 标题使用
`yyyy.MM.dd`（例如 `2026.09.05`，不加 `v`）。同一天已有 tag 或 Release 时，按当天
最大补丁号加一：`2026.09.05` → `2026.09.05p1` → `2026.09.05p2`，不复用中间空号。
草稿 Release 也占用版本号。发布任务串行执行，在获得发布名额后重新分页读取全部
tag/Release；新标签始终指向本次构建提交，不移动旧标签、不覆盖已有 Release。
Debug / RelWithDebInfo 仍可只构建，不用于发布；每次工作流也会额外构建旧系统 Release 版。
Release 附件为现代版 `TTPlayerRebuild-版本号.zip`、旧系统版
`TTPlayerRebuild-XP-Win7-版本号.zip` 与包含两个 ZIP 校验值的 `SHA256SUMS.txt`。
例如发布 `2026.09.05p1` 时，两个附件分别为 `TTPlayerRebuild-2026.09.05p1.zip`、
`TTPlayerRebuild-XP-Win7-2026.09.05p1.zip`。发布步骤在分配最终版本号后重命名 ZIP，
同步更新校验文件和正文下载说明；原 Actions 构建产物仍保留构建日期名称。
两种 ZIP 解压后的程序名均为 `TTPlayerRebuild.exe`。
**XP / Win7 请使用名称中带 XP-Win7 的包**；详见 [兼容版说明](LEGACY_WINDOWS.md)。
完整更新日志链接自动使用仓库中最近的版本 tag 与本次新 tag 对比，例如
`compare/2026.09.05...2026.09.05p1`。历史版本 tag 必须符合 `yyyy.MM.dd` 或
`yyyy.MM.ddpN`，按日期和数值补丁号排序（`p10` 晚于 `p2`），不依赖 API 返回顺序。
若还没有符合规则的历史 tag，则链接到 `commits/本次版本`，不生成无效对比链接。
正文安装说明明确区分现代版与旧系统版，不自动追加 GitHub 生成的说明。

工作流会先运行 `cmake/test_manual_release.ps1`，离线检查北京时间边界、同日补丁号、
动态对比链接、安装说明和模拟发布保护逻辑，包括两个附件的 SHA-256 和 Release 配置。
同时用隔离目录实际打包、解压两个版本，检查 ZIP 内容、版本化文件名及内外校验文件。
本地也可运行该脚本；测试不调用远程 API，不创建标签或 Release。

工作流必须先存在于默认分支，手动运行入口才会显示，见
[GitHub 手动运行工作流说明](https://docs.github.com/en/actions/how-tos/manage-workflow-runs/manually-run-a-workflow)。

使用 `windows-2025-vs2026`、Visual Studio 2026、Win32/x86；不构建 x64，
因为现有 DLL/AddIn ABI 为 32 位。构建任务只有 `contents: read` 权限，
仅勾选发布时运行的独立发布任务使用 `contents: write`，通过内置 `GITHUB_TOKEN` 发布。
官方 checkout/upload-artifact/download-artifact 动作固定到提交 SHA，不需要额外 secrets。

`windows-2025` 已迁移到 VS 2026 镜像，因此不能再配合写死的
`Visual Studio 17 2022` 生成器。工作流明确选择 VS 2026 镜像和
`Visual Studio 18 2026` 生成器，并使用 `vswhere` 查找带 x86/x64 C++ 工具的
18.x 安装实例，显式传给 CMake。配置前检查 CMake 的生成器支持，
避免版本或 PATH 不匹配时仅出现笼统的找不到 Visual Studio 报错。
VS 2026 生成器要求 CMake 4.2 或更新版本，Runner 已提供相应工具。
参见 [GitHub 镜像迁移公告](https://github.com/actions/runner-images/issues/14017)
及 [CMake VS 2026 生成器说明](https://cmake.org/cmake/help/latest/generator/Visual%20Studio%2018%202026.html)。

旧系统版在 `build-legacy` 独立构建，固定使用 YY-Thunks 1.2.2 和 VC-LTL 5.3.1，
首次配置自动下载并校验 SHA-256；Python 3 检查 XP / Win7 导入表后才允许打包。
旧系统包附带依赖许可和导入报告。构建时不改动系统 DLL，也无需安装旧 Visual Studio。

修改工作流后，请提交并在 **Run workflow** 中选择含修复的分支发起新运行；
直接 **Re-run jobs** 重跑旧失败记录仍使用旧提交中的工作流。

现代版 ZIP 包含 EXE、许可证、本说明和 EXE 的 SHA-256 校验值；兼容版 ZIP
另附兼容说明、导入报告及依赖许可。Actions 产物还包含两个 ZIP 的校验文件、
提交和构建信息，以及生成时的 PDB。
**这不是包含原版运行依赖的安装包**：请把 `TTPlayerRebuild.exe` 放入已有
TTPlayer 目录，与其 `ttpcomm.dll`、`ttpres.dll`、`AddIn`、`Skin` 等一起使用。
不会上传原版 DLL、编码器、歌曲、播放列表或 `TTPlayerRebuild.xml` 等个人配置。
普通使用选择 Release；Debug 需要开发环境的调试运行库，不适合分发。

程序从 EXE 同目录读写 `TTPlayerRebuild.xml`；仅当新文件不存在时，
自动导入同目录旧 `TTPlayer.xml`，保留旧文件且不再向其写入。
皮肤同时扫描 `Skin`、`Skin/new`（后者用于 PNG 皮肤），不递归扫描其它目录。
例如 `Skin/new/Example.skn` 的配置是 `Skin/new/Example.skn.xml`，
主配置中的皮肤标识保存为 `new\Example.skn`。内置皮肤仍使用 `Skin/Default.xml`。

“选项 → 关于”的构建日期由每次构建开始时的北京时间（UTC+08:00）生成，
格式为 `yyyy-M-d`，不使用构建机器本地时区。该日期编入 EXE，启动播放器时
不会变化。本地和 GitHub Actions 共用同一生成步骤；跨日增量构建会更新日期，
同日重复构建不重复写入生成头文件，也不需要手工改源码或重新配置 CMake。

## 干净源码构建

在此仓库根目录（本地即 `rebuild`）执行：

```powershell
cmake -S . -B out/ci -G "Visual Studio 18 2026" -A Win32 -DBUILD_TESTING=OFF -DTTPLAYER_STAGE_RUNTIME=OFF
cmake --build out/ci --config Release --target ttplayer_rebuild --parallel 4
```

使用独立输出目录，不覆盖已有播放器运行目录。
构建目标名仍为 `ttplayer_rebuild`，生成文件改为 `Release/TTPlayerRebuild.exe`
（以及有调试信息时的 `TTPlayerRebuild.pdb`）。本地运行文件复制只在目标
`TTPlayerRebuild.xml` 不存在时初始化它，增量构建不会覆盖该配置。
本地同样需要 VS 2026 的 C++ 工具和 CMake 4.2+。如果某个构建目录曾用
VS 2022 配置，请改用新的空构建目录，不要复用旧的生成器缓存。

- `BUILD_TESTING=OFF`：跳过未提交的 `tests/`、`tools/` 及外部反编译测试输入；
  这不关闭播放器内嵌的隔离工作进程功能。
- `TTPLAYER_STAGE_RUNTIME=OFF`：跳过资源 DLL 重建与原版运行文件、LAME、
  mp3PRO 和配置文件的复制，仍然完整编译播放器 EXE。
- 编译使用仓库内 `include/ttpcomm_api.h`、`src/app/ttpcomm_api.c` 以及
  `src/app/assets/TTPlayer.ico`。接口源码来自现有 `reverse` 中的恢复代码，
  导出序号、ABI、调用和原版图标内容保持不变，不需要反编译伪代码参与构建。

本地完整恢复工作区的原有两个选项默认仍为 ON，保留测试和运行文件复制流程；
它需要仓库旁的 `reverse/`、原版 DLL、皮肤等数据以及本地测试/工具源码。
GitHub 工作流在现代版和旧系统版构建后运行媒体库核心回归；依赖私有原版资源
的完整窗口测试只在本地资源可用时运行，不等同于 XP／Win7 实机验收。

### 独立媒体库回归

无需未提交的 `tests/`、`tools/` 即可构建：

```powershell
cmake -S . -B build-library-tests -G "Visual Studio 18 2026" -A Win32 -DBUILD_TESTING=OFF -DTTPLAYER_STAGE_RUNTIME=OFF -DTTPLAYER_BUILD_LIBRARY_TESTS=ON
cmake --build build-library-tests --config Release --target media_library_tests --parallel 4
ctest --test-dir build-library-tests -C Release -R '^media_library_tests$' --output-on-failure
```

测试在独立临时目录运行。原版 DLL 可用时追加完整窗口的启动、关闭期间保存和
重新打开验证；资源不可用时明确报告跳过该部分，核心数据／文件监视测试仍执行。
