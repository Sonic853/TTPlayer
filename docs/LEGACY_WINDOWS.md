# XP / Windows 7 兼容版

下载 `TTPlayerRebuild-XP-Win7-版本号.zip`，解压后把其中的 `TTPlayerRebuild.exe`
放入已安装的千千静听 5.7.9 目录，与 `TTPlayer.exe`、`ttpcomm.dll`、
`ttpres.dll`、`AddIn`、`Skin` 放在一起。运行目录内的 `TTPlayerRebuild.exe`。
压缩包不包含原版插件和个人配置；设置文件仍为 `TTPlayerRebuild.xml`。

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
- 在线服务仍取决于系统 WinHTTP、证书和 TLS 能力。未给 XP 添加 TLS 1.2 实现，
  不通过降低 HTTPS 安全级别或忽略证书错误实现连接。HTTP 可用；不支持安全重定向策略的系统禁用自动跳转。

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

每次链接后，`cmake/check_legacy_imports.py` 会将全部静态 DLL/函数/序号导入与
YY-Thunks 包中的 XP、Win7 系统导出清单核对，并拒绝过高的 PE 版本或未审核的延迟导入。
检查失败则构建失败。`legacy-imports.json` 记录 EXE 的 SHA-256，打包前再次核对，
防止检查后替换 EXE。此检查覆盖主 EXE；第三方插件需自行满足旧系统要求。

## 验证范围

已在当前 Windows 主机检查 PE 导入并进行隔离启动；通过强制关闭 Media Foundation /
属性系统的测试验证内置 WAV、Windows Media MP3/WMA 解码和定位、文件复制、内存流、
播放工作线程的后备路径。**这些检查不是 XP / Win7 实机测试。**
发布到实际旧系统前仍需验证：启动、添加文件/目录、MP3/WAV/WMA 和常用 AddIn 播放、
暂停/定位/切曲、歌词、转换、关闭与重启。XP SP3 及 Win7 的驱动和插件差异需在目标机器验证。

## 来源

- [CreateFile2 最低要求 Windows 8](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfile2)。
- [YY-Thunks 1.2.2](https://github.com/Chuyu-Team/YY-Thunks/releases/tag/v1.2.2)：旧系统 API 后备实现。
- [VC-LTL 5.3.1](https://github.com/Chuyu-Team/VC-LTL5/releases/tag/v5.3.1)：C/C++ 运行库适配。
- 第三方许可在压缩包的 `licenses` 目录中；依赖未作修改。
