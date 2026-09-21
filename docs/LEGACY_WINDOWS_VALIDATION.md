# 2026-09-15 旧系统兼容修复验证

> 2026-09-21 补充：相关功能已在 XP SP3 / Win7 SP1 虚拟机中执行回归。覆盖项、修复和未覆盖边界见 [虚拟机验证记录](XP_WIN7_VM_VALIDATION.md)。下文保留原日期的历史结论。

## 根因

本地原 `build/Release/TTPlayerRebuild.exe` 是 MSVC 14.51 / x86，PE 子系统版本 6.00。
`dumpbin /imports` 发现 `KERNEL32!CreateFile2`、`CopyFile2`，以及 `MSVCP140.dll`、
`MSVCP140_ATOMIC_WAIT.dll`、`VCRUNTIME140.dll` 和 UCRT API-set DLL。
源码没有直接调用前两个 API；它们由预编译 C++ 标准库带入。
另外，XP 缺少的 DWM、MF、属性系统及 Shell / Common Controls 入口也在启动导入表中。

## 产物与验证

- 普通 Release：`build/Release/TTPlayerRebuild.exe`。
- 独立旧系统包：`build/Release/TTPlayerRebuild-XP-Win7.zip`。
- 正式旧系统构建目录：`out/legacy-distribution`，使用真实远程下载和 SHA-256 校验，
  `BUILD_TESTING=OFF`、`TTPLAYER_STAGE_RUNTIME=OFF`，不使用本地依赖目录替代下载。
- 旧系统 EXE：3,252,224 字节；SHA-256：
  `76429767537912d6529d355810111d4e3fde63be823f5655e37e7eb15f5aa9f7`。
- XP / Win7 两套导出清单均通过：18 个系统 DLL、598 个静态函数/序号导入。
  PE 为 x86，OS 5.1、子系统 5.01，无延迟导入，无上述新版 VC/MF/DWM/属性系统启动依赖。
- 对已通过检查的 EXE 注入 `CreateFile2` 导入，检查按预期拒绝。
  检查完成后更改 EXE 内容，打包脚本按预期拒绝过期报告。
- ZIP 解压后核对内外 EXE 和 SHA-256 一致；精确使用 ZIP 内 EXE 运行隔离启动测试，
  16 种缺失/损坏皮肤和配置组合及 3 次正常启动通过。

## 运行回归

现代 Release 的以下 5 项 CTest 全部通过（11.26 秒）：

- `legacy_windows_tests`
- `runtime_paths_tests`
- `lyric_search_tests`
- `default_programs_tests`
- `conversion_recovery_tests`

旧系统运行库构建下，组件后备测试、启动/路径测试、默认程序测试均通过。
组件后备测试强制禁用 MF 和属性系统，验证 WAV 解码、文件创建/复制、内存流、转换、
损坏 WAV 拒绝，以及实际播放工作线程能够进入输出设备校验。
额外生成的 MP3、WMA 各 1 秒测试文件分别解码出 93,408 和 86,016 字节 PCM，定位后再次读取成功。

`tests/cmake/test_manual_release.ps1`：6 个北京时间/配置场景、19 个模拟发布场景通过，
覆盖两个附件的校验、错误配置拒绝、版本分配、安装说明与 notes 文件。
未调用远程发布 API，未创建标签或 Release。

## 尚未验证

没有 XP / Win7 虚拟机或实机，本次运行测试宿主是当前 Windows。
静态导出表核对和强制缺组件测试不代替真实旧系统上的驱动、编解码器及第三方插件验证。
经典文件选择窗口的 XP 分支未做实机交互验证。

尝试构建原有本地 `audio_recovery_tests` 时，未跟踪的
`tests/audio_recovery_tests.cpp:858` 使用 `int*` 作为歌词搜索回调，
与现有 `PluginManager::CreateLyricSearch` 的 `LyricSearchCallback*` 接口不匹配而编译失败。
该项未计入通过结果；没有为本次兼容修复改动这份旧测试。

对应日志和临时验证产物位于 `out/compat-deps`；它们不是发布资源。
