# XP / Windows 7 与通用构建

2026-09-25 起，普通版和旧系统版合并为 **同一份 x86 Release EXE**。
下载 `TTPlayerRebuild-版本号.zip`，不再选择单独的 XP-Win7 包。
解压后把 `TTPlayerRebuild.exe` 放入已安装的千千静听 5.7.9 目录，与 `TTPlayer.exe`、
`ttpcomm.dll`、`ttpres.dll`、`AddIn`、`Skin` 放在一起。
压缩包仅包含 EXE 和 `SHA256SUMS.txt`；不包含原版插件或个人设置。

全部调整、功能矩阵及本次验证见 [通用构建调整记录](UNIFIED_WINDOWS_BUILD.md)。

## 运行库与系统差异

- 主程序统一使用 VC-LTL 5.3.1、YY-Thunks 1.2.2，依赖系统 `msvcrt.dll`；
  不需要现代 VC++ Redistributable，不要求用户单独安装 VC-LTL 或 YY-Thunks。
- 原版 AC3/DTS、ASF、MOD、MPC、OGG、RM 等插件仍可能需要 VC++ 2012 x86。
  已重建的 `ttp_aac.dll` 使用自己的兼容构建，插件独立构建及分发。
- EXE 启动时导入同目录 `ttpcomm.dll!#3`，让 XP 正确分配原版 DLL 的 TLS；
  不可只复制 EXE 到没有原版运行文件的空目录。
- XP 使用经典文件打开、保存和目录选择窗口；Win7 及新系统优先使用现代 COM 对话框。
  XP 目录选择的“包含子目录”选项在选定目录后单独询问。
- XP／Win7 不创建 SMTC/WinRT 对象；Win10／Win11 自动尝试启用，失败不影响播放。
- XP／Win7 的随机播放始终后台生成一份随机索引并首尾循环，轮尾不重新洗牌。
  Win8+ 在不超过 5000 首时使用三轮滚动索引，超过时仍为单份随机索引循环。
- XP／Win7 歌曲信息缓存为 128 项／4 MiB；新系统为 512 项／16 MiB。
- 缺少 Media Foundation 时继续使用内置 PCM、Windows Media Format 和 AddIn 路径；
  解码能力取决于安装的系统组件与插件。
- XP 不提供 DWM 预览／任务栏缩略图按钮。Win7 预览需 Aero 合成可用。
  封面使用 WIC，失败时回退 GDI+；PNG 等格式仍走已有兼容路径。
- WASAPI 共享／独占使用实际设备与系统接口探测；XP 无 WASAPI。
- XP／Win7 文件关联沿用当前用户直接关联；Win8+ 交给系统默认程序流程。

## 构建和审计

需要 MSVC x86、ATL、CMake 4.2+（VS 2026 生成器）及 Python 3。
首次配置下载固定版本依赖并校验 SHA-256；无需旧 Visual Studio 或 v141_xp。

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A Win32 -DBUILD_TESTING=OFF -DTTPLAYER_STAGE_RUNTIME=OFF
cmake --build build --config Release --target ttplayer_rebuild --parallel 4
powershell -NoProfile -ExecutionPolicy Bypass -File cmake/package_player.ps1 -BuildDirectory build -Destination build/Release/TTPlayerRebuild-2026.09.25.zip
```

示例 ZIP 日期须与实际 EXE 版本一致。Actions 自动分配版本、验证资源并命名。
原 `TTPLAYER_LEGACY_WINDOWS=ON/OFF` 参数仅提示迁移，均构建通用版；
`package_legacy.ps1` 保留为通用打包脚本的转发入口。

链接使用 `/MT`、VC-LTL XP CRT 适配、YY-Thunks XP 对象及子系统 5.01。
每次链接后，`cmake/check_legacy_imports.py` 对照 XP／Win7 导出清单核对全部静态导入，
拒绝过高的 PE 版本、未审核的延迟导入和缺失的 `ttpcomm.dll!#3`。
打包再核对报告中的 EXE SHA-256，防止审计后文件被替换。
审计覆盖主 EXE；第三方插件仍须满足自身系统和运行库要求。

## 历史验证

- [XP 启动与 TLS 修复](XP_STARTUP_TLS_FIX.md)
- [XP／Win7 虚拟机验证](XP_WIN7_VM_VALIDATION.md)
- [旧系统专辑封面](LEGACY_ALBUM_COVER_FIX.md)
- [文件关联修复](LEGACY_FILE_ASSOCIATION_FIX.md)

以上文档的双构建结论保留其历史日期；当前分发以通用构建文档为准。

## 来源

- [CreateFile2 最低要求 Windows 8](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfile2)。
- [YY-Thunks 1.2.2](https://github.com/Chuyu-Team/YY-Thunks/releases/tag/v1.2.2)：旧系统 API 后备实现。
- [VC-LTL 5.3.1](https://github.com/Chuyu-Team/VC-LTL5/releases/tag/v5.3.1)：C/C++ 运行库适配。
- 第三方许可保留在仓库及构建目录中；依赖未作修改。压缩包仅包含 `TTPlayerRebuild.exe` 和 `SHA256SUMS.txt`。
