# XP / Windows 7 兼容版

下载 `TTPlayerRebuild-XP-Win7-版本号.zip`，解压后把其中的 `TTPlayerRebuild.exe`
放入已安装的千千静听 5.7.9 目录，与 `TTPlayer.exe`、`ttpcomm.dll`、
`ttpres.dll`、`AddIn`、`Skin` 放在一起。运行目录内的 `TTPlayerRebuild.exe`。
压缩包不包含原版插件和个人配置；设置文件仍为 `TTPlayerRebuild.xml`。
兼容版在进程启动时导入同目录 `ttpcomm.dll` 的序号 3，使 XP 为原版 DLL 正确分配
线程局部存储（TLS）。该 DLL 必须随原版运行文件一起放置；缺失时由系统加载器报告错误。

## 版本选择

| 附件 | 适用系统 | 运行库 |
| --- | --- | --- |
| `TTPlayerRebuild-版本号.zip` 内的 EXE | 现代 Windows（建议 Windows 10 / 11） | 当前 MSVC 运行库 |
| `TTPlayerRebuild-XP-Win7-版本号.zip` 内的 EXE | Windows XP SP3 / Windows 7，也可用于更新系统 | VC-LTL 5.3.1 + YY-Thunks 1.2.2，使用系统 `msvcrt.dll` |

两者均为 x86。兼容版保留现代系统上的可用功能；缺少组件时使用后备实现：

- XP 文件打开、保存和目录选择使用经典窗口。目录选择的“包含子目录”选项在选定目录后单独询问。
- 缺少 Media Foundation 时，PCM/浮点 WAV 使用内置读取器，MP3/WMA 使用系统
  Windows Media Format（`wmvcore.dll`），其它格式仍优先使用原版 AddIn。
  精简系统需要保留 Windows Media 运行组件；后备解码器不提供新的编解码器。
- XP 不提供任务栏缩略图按钮、DWM 合成等系统功能。Shell 元数据不可用时保留其它元数据读取路径。
- 专辑封面支持 PNG；WIC 不可用时使用系统 GDI+ 解码，文件属性预览也使用相同
  后备路径。已在 XP SP3 / Win7 SP1 虚拟机验证常见格式、透明度及内嵌 PNG，
  详见源码仓库 `docs/LEGACY_ALBUM_COVER_FIX.md`。
- 多格式封面已补齐 APE 标签服务和属性保存路径；M4A / MP4、APE / MAC、WMA / ASF、
  RM / RA 可通过对应插件读取和编辑 JPEG / PNG。裸 AAC、TTA 没有封面接口。
  AAC / ASF / RM 等插件仍需其配套 x86 VC++ 2012 运行库，详见源码仓库
  `docs/ALBUM_COVER_FORMAT_RECOVERY.md`。
- XP / Win7 的“系统关联”按原版直接设置当前用户的文件关联，取消时恢复本程序
  保存的旧关联，不再依赖 `SetAppAsDefault`。兼容 EXE 在 Win8 及以后运行时仍
  使用系统默认程序确认流程。详见源码仓库 `docs/LEGACY_FILE_ASSOCIATION_FIX.md`。
- 在线服务仍取决于系统 WinHTTP、证书和 TLS 能力。未给 XP 添加 TLS 1.2 实现，
  不通过降低 HTTPS 安全级别或忽略证书错误实现连接。HTTP 可用；不支持安全重定向策略的系统禁用自动跳转。

随机播放按用户要求单独采用低内存策略：**XP／Win7 版无论歌曲多少，始终只在后台
生成一份随机索引，并在这份索引内首尾循环**，每轮结束不重新洗牌。普通版才在
5000 首以内使用三轮滚动索引。启用“自动切换列表”时，完成一轮仍会进入下个真实列表。

## 构建

需要 MSVC x86、CMake 4.2+（VS 2026 生成器）和 Python 3。首次配置联网下载固定版本
YY-Thunks / VC-LTL，CMake 校验 SHA-256。兼容版使用独立目录，只生成 Release：

```powershell
cmake -S . -B out/legacy -G "Visual Studio 18 2026" -A Win32 -DBUILD_TESTING=OFF -DTTPLAYER_STAGE_RUNTIME=OFF -DTTPLAYER_LEGACY_WINDOWS=ON
cmake --build out/legacy --config Release --target ttplayer_rebuild --parallel 4
powershell -NoProfile -ExecutionPolicy Bypass -File cmake/package_legacy.ps1 -BuildDirectory out/legacy -Destination out/TTPlayerRebuild-XP-Win7.zip
```

普通构建保持 `TTPLAYER_LEGACY_WINDOWS=OFF`。不能用修改 `_WIN32_WINNT` 或
EXE 版本头的方法修复当前标准库已有的导入。兼容构建在链接时接入 XP thunk 对象、
VC-LTL 的 XP CRT 适配库及 `/MT`，设置 PE 子系统版本 5.01。

每次链接后，`cmake/check_legacy_imports.py` 会将系统 DLL/函数/序号导入与
YY-Thunks 包中的 XP、Win7 系统导出清单核对，并拒绝过高的 PE 版本或未审核的延迟导入。
另强制检查原版应用依赖 `ttpcomm.dll!#3` 存在，只有这一项可绕过系统 DLL 清单。
检查失败则构建失败。`legacy-imports.json` 记录 EXE 的 SHA-256，打包前再次核对启动导入和哈希，
防止检查后替换 EXE。此检查覆盖主 EXE；第三方插件需自行满足旧系统要求。

## 验证范围

已在当前 Windows 主机检查 PE 导入并进行隔离启动；通过强制关闭 Media Foundation /
属性系统的测试验证内置 WAV、Windows Media MP3/WMA 解码和定位、文件复制、内存流、
播放工作线程的后备路径。

2026-09-18 已在 VirtualBox 的 Windows XP Professional SP3（5.1.2600）中复现并修复
启动崩溃：从共享 Z 盘复制修复版到 `C:\Documents and Settings\853\My Documents\TTPlayer`
后，连续三次 `--smoke-test` 均返回 0，普通启动、打开选项和正常退出均通过。另用对照程序确认 DLL 与 EXE 的
TLS 索引分离，三个并发线程的随机数状态和 C++ 局部静态初始化正常。
详见源码仓库 `docs/XP_STARTUP_TLS_FIX.md`。

Win7 本轮未进行来宾运行测试。完整功能仍需逐项验证：添加文件/目录、MP3/WAV/WMA 和常用 AddIn 播放、
暂停/定位/切曲、歌词、转换、关闭与重启。XP SP3 及 Win7 的驱动和插件差异需在目标机器验证。

## 来源

- [CreateFile2 最低要求 Windows 8](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfile2)。
- [YY-Thunks 1.2.2](https://github.com/Chuyu-Team/YY-Thunks/releases/tag/v1.2.2)：旧系统 API 后备实现。
- [VC-LTL 5.3.1](https://github.com/Chuyu-Team/VC-LTL5/releases/tag/v5.3.1)：C/C++ 运行库适配。
- 第三方许可保留在仓库及构建目录中；依赖未作修改。压缩包仅包含 `TTPlayerRebuild.exe` 和 `SHA256SUMS.txt`。
