# DSP 窗口与主程序的任务栏分组修复

日期：2026-09-29。

## 原因

重建版 `PublishTaskbarIcon` 为主窗口和固定快捷方式设置了显式 `AppUserModelID`，但启动流程没有设置进程级别的相同标识。

真实 `dsp_izOzone.dll` 创建的 `iZotope Winamp Ozone` 是本进程内的独立顶层窗口：没有 owner，也没有自己的窗口级 `AppUserModelID`。它依赖进程身份，因此此前与使用显式身份的主窗口分成两个任务栏组。图标相同并不意味着分组身份相同。

本次测试在同一个探针中分别模拟旧版仅设置主窗口身份的路径和修复后的路径，复现了上述差异。问题来自重建版的任务栏身份初始化遗漏，与上一轮 DSP 音频工作线程修复无关。

## 修改

1. 新增 `InitializePlayerTaskbarIdentity`，在普通播放器启动、任何界面显示之前，为进程设置与主窗口相同的稳定 ID。
2. 使用现有的“规范化 EXE 完整路径哈希”生成标识，与主窗口和固定快捷方式共用规则；不同便携目录仍可区分。
3. 未指定独立身份的同进程插件窗口自动使用该进程身份，不按 Ozone 名称建立特例。
4. 保留插件窗口自己的 owner、样式和关闭行为，以及原有音频线程分工。
5. 通过 `GetProcAddress` 动态解析 API。XP／Vista 无此 API 时返回 `S_FALSE`，继续使用系统原有分组方式；后台探测 worker 不进入此播放器初始化路径。

依据微软文档，进程级身份用于把关联窗口归入同一任务栏按钮，应在显示界面前设置；窗口级身份可覆盖进程身份：

- [SetCurrentProcessExplicitAppUserModelID](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-setcurrentprocessexplicitappusermodelid)
- [System.AppUserModel.ID](https://learn.microsoft.com/en-us/windows/win32/properties/props-system-appusermodel-id)

这是任务栏身份修复。Windows 的任务栏合并偏好仍由系统控制，插件自己的窗口不被隐藏或改成子窗口；独立外部进程或主动指定另一身份的插件不在此次自动继承范围内。

## 验证

| 环境／项目 | 结果 |
| --- | --- |
| Windows 11，旧路径＋真实 Ozone | 主窗口显式 ID 与插件继承的进程身份不同，复现问题 |
| Windows 11，修复路径＋真实 Ozone | 两者有效身份一致；关闭和重新打开均通过 |
| Windows 7 虚拟机，真实 Ozone | 初始化、配置窗口重开、有效身份一致及卸载后身份保持均通过 |
| Windows XP 虚拟机 | 新 API 缺失时正常回退，退出码 0 |
| Release 主程序和更新程序 | XP／Win7 静态导入检查通过 |

Windows 11／Win7 测试读取真实可见 Ozone 窗口的属性与当前进程身份，并核对插件的 owner 和任务栏资格未被改变。该测试验证分组依据，不是对所有 Windows 任务栏合并设置的截图回归。

本地测试代码：`tests/dsp_taskbar_identity_probe.cpp`；结果：`tests/artifacts/dsp_taskbar_identity/`。未加入 Actions 或发行包。

## 交付

- `build/Release/TTPlayerRebuild.exe`
- `build/Release/TTPlayerRebuild-2026.09.29.zip`

关闭旧进程后使用新 EXE 启动，启动期设置的进程身份才会生效。
