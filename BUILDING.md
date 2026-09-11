# 手动构建

## GitHub Actions

本地 `rebuild` 是独立 Git 仓库；它在 GitHub 上就是仓库根目录。
工作流位于 `.github/workflows/manual-build.yml`，不配置 push/PR 自动触发，
也不创建 Release 或推送代码。

1. 将工作流及配套构建文件提交到远程仓库的默认分支。
2. 打开 **Actions → Manual Windows Build → Run workflow**。
3. 选择分支及配置：`Release`（默认）、`RelWithDebInfo` 或 `Debug`。
4. 构建完成后，从该次运行的 **Artifacts** 下载
   `TTPlayer-Windows-x86-配置-运行编号`，产物保留 14 天。

工作流必须先存在于默认分支，手动运行入口才会显示，见
[GitHub 手动运行工作流说明](https://docs.github.com/en/actions/how-tos/manage-workflow-runs/manually-run-a-workflow)。

使用 `windows-2025-vs2026`、Visual Studio 2026、Win32/x86；不构建 x64，
因为现有 DLL/AddIn ABI 为 32 位。工作流只有 `contents: read` 权限，
官方 checkout/upload-artifact 动作固定到提交 SHA，不需要额外 secrets。

`windows-2025` 已迁移到 VS 2026 镜像，因此不能再配合写死的
`Visual Studio 17 2022` 生成器。工作流明确选择 VS 2026 镜像和
`Visual Studio 18 2026` 生成器，并使用 `vswhere` 查找带 x86/x64 C++ 工具的
18.x 安装实例，显式传给 CMake。配置前检查 CMake 的生成器支持，
避免版本或 PATH 不匹配时仅出现笼统的找不到 Visual Studio 报错。
VS 2026 生成器要求 CMake 4.2 或更新版本，Runner 已提供相应工具。
参见 [GitHub 镜像迁移公告](https://github.com/actions/runner-images/issues/14017)
及 [CMake VS 2026 生成器说明](https://cmake.org/cmake/help/latest/generator/Visual%20Studio%2018%202026.html)。

修改工作流后，请提交并在 **Run workflow** 中选择含修复的分支发起新运行；
直接 **Re-run jobs** 重跑旧失败记录仍使用旧提交中的工作流。

产物包含 EXE、许可证、本说明、SHA-256 校验值、提交信息，以及生成时的 PDB。
**这不是包含原版运行依赖的安装包**：请把 `ttplayer_rebuild.exe` 放入已有
TTPlayer 目录，与其 `ttpcomm.dll`、`ttpres.dll`、`AddIn`、`Skin` 等一起使用。
不会上传原版 DLL、编码器、歌曲、播放列表或 `TTPlayer.xml` 等个人配置。
普通使用选择 Release；Debug 需要开发环境的调试运行库，不适合分发。

“选项 → 关于”的完成日期由每次构建开始时的北京时间（UTC+08:00）生成，
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
GitHub 工作流只验证编译，不冒充已经完成依赖原版资源的 UI/播放运行测试。
