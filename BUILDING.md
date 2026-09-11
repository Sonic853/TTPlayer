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

使用 `windows-2025`、Visual Studio 2022、Win32/x86；不构建 x64，
因为现有 DLL/AddIn ABI 为 32 位。工作流只有 `contents: read` 权限，
官方 checkout/upload-artifact 动作固定到提交 SHA，不需要额外 secrets。

产物包含 EXE、许可证、本说明、SHA-256 校验值、提交信息，以及生成时的 PDB。
**这不是包含原版运行依赖的安装包**：请把 `ttplayer_rebuild.exe` 放入已有
TTPlayer 目录，与其 `ttpcomm.dll`、`ttpres.dll`、`AddIn`、`Skin` 等一起使用。
不会上传原版 DLL、编码器、歌曲、播放列表或 `TTPlayer.xml` 等个人配置。
普通使用选择 Release；Debug 需要开发环境的调试运行库，不适合分发。

## 干净源码构建

在此仓库根目录（本地即 `rebuild`）执行：

```powershell
cmake -S . -B out/ci -A Win32 -DBUILD_TESTING=OFF -DTTPLAYER_STAGE_RUNTIME=OFF
cmake --build out/ci --config Release --target ttplayer_rebuild --parallel 4
```

使用独立输出目录，不覆盖已有播放器运行目录。

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
