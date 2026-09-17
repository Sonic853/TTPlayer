# XP / Win7 系统关联修复

日期：2026-09-17。

## 问题与结论

Win7 在“选项 → 系统关联”勾选格式时，旧重建版报
`SetAppAsDefault：系统找不到指定的文件`。这是关联后端执行失败，
与 WTL 的树控件绘制无关。

检查旧重建代码可确定两个问题：

1. Win7 分支注册候选程序后依赖 `IApplicationAssociationRegistration::SetAppAsDefault`。
   此调用失败便返回错误，缺少原版的直接设置关联路径。
   XP 则跳过此 COM 调用，但也没有写扩展名默认值，实际上只完成了候选注册。
2. `RegisteredApplications` 的名称为 `TTPlayerRebuild`，而 Capabilities 中
   `ApplicationName` 为本地化名称加 ` (TTPlayerRebuild)`。微软要求两者一致，
   此处违反了注册契约，已统一为 `TTPlayerRebuild`。

`SetAppAsDefault` 的“找不到文件”不能直接解释为 EXE 丢失：调用传入的是
已注册应用的名称与扩展名，系统还需要解析相应注册信息。没有出错机器的完整
注册表和 Win7 实机复现，不能断言名称不一致就是该 HRESULT 的唯一原因。
本次修复移除了 XP / Win7 设置关联对这条 COM 调用的依赖，不是忽略其错误。

## 原版伪代码对照

依据 `reverse/decompiled/TTPlayer.exe.pseudo.c`，原版 5.7.9：

| 函数 | 原版行为 | 重建版处理 |
| --- | --- | --- |
| `FUN_0049D8C0` | 比较树节点原状态、期望状态及图标变化，批量提交；有变化时通知 Shell | 保留变化检测与批量 Shell 通知；沿用用户此前要求的勾选后立即提交 |
| `FUN_0049CA63` | 分发关联或取消关联操作 | 仍使用同一设置后端 |
| `FUN_0049BB64` | 清理 Explorer 旧覆盖项，设置扩展名默认 ProgID、图标和命令 | XP / Vista / Win7 恢复直接注册路径 |
| `FUN_0049B3AA` | 构造播放及加入列表的 Shell 命令 | `"EXE" "%1"` 与 `"EXE" /a "%1"` |
| `FUN_0049C3D4` | 识别自己的关联，并利用 `Backup` 恢复原关联 | 按重建版所有权标记恢复备份，保留后续用户选择 |
| `FUN_0049DFB4` | 手动设置入口通过 `FileTypeAsso` XML 和 `ttpsvr.exe` 辅助程序执行 | 保留系统设置入口；XP 打开文件夹选项，Vista / Win7 打开默认程序 |

`FUN_0049BB64` 的扩展名分支明确包含以下步骤：

1. 打开 HKCU `Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.ext`。
2. 删除该键中的 `Application`、`Progid` 值。
3. 系统主版本大于 5 时，以 `RegDeleteKeyW` 删除 `UserChoice` **子项**。
4. 在 Classes 中设置 `.ext` 默认值为 `Audio.<ext>`，备份旧默认值，
   安装格式说明、图标与播放命令。

该关联链没有 `SetAppAsDefault` 调用，也不以 Capabilities 注册作为设置默认
程序的前提。仅补写候选注册，无法还原它的实际行为。

## 修复实现与有意保留的差异

### 1. 根据运行系统选择流程

| 运行系统 | 勾选关联 | 取消关联 | 手动入口 |
| --- | --- | --- | --- |
| XP / XP x64 | 直接写入每用户默认关联并清理旧 Explorer 值 | 恢复本程序备份 | `control.exe folders`，进入文件夹选项查看“文件类型” |
| Vista / Win7 | 同上，并清理旧 `UserChoice` 子项 | 恢复备份的旧关联 | 默认程序控制面板 / `LaunchAdvancedAssociationUI` |
| Win8 / 8.1 | 注册候选，由用户在系统界面确认 | 由用户选择替代程序 | 系统默认程序界面 |
| Win10 / Win11 | 注册候选，由用户在系统界面确认 | 由用户选择替代程序 | 对应版本的默认应用设置 |

通过动态解析 `RtlGetVersion` 识别实际系统；识别失败时采用现代受保护路径。
即使运行的是 XP / Win7 兼容 EXE，在 Win10 / Win11 上也不会进入旧关联流程。

### 2. 处理旧 UserChoice 的访问权限

原版删除的是整个子项。修改或删除其 `Progid` 值需要 `KEY_SET_VALUE`，
当该键拒绝此权限时，“只删除值”的替代实现仍会失败。

修复采用只读查询和 `RegDeleteKeyW`，不修改 ACL。备份放在独立的
`TTPlayerRebuild.Audio.<ext>\LegacyUserChoiceBackup` 子项中，避免往
不可写的旧 `UserChoice` 中存储元数据。此路径仅在 Vista / Win7 启用；
Win8 及以后不会删除、写入 `UserChoice` 或其 Hash。

### 3. 备份与恢复

- 保留重建版独立 ProgID `TTPlayerRebuild.Audio.<ext>`，不占用原版的 `Audio.*`。
- 写入 HKCU Classes，作用于当前用户；没有复刻原版 HKCR / 提升辅助程序的全部实现。
- 先安装命令和图标，再发布 `.ext` 默认值，避免指向尚未准备好的处理程序。
- 原扩展名默认值、Explorer 的 `Application` / `Progid` 和 Win7 的旧
  `UserChoice.Progid` 均保存恢复信息。原版仅清除 Explorer 覆盖项；这里额外保留
  旧关联选择，便于取消时恢复。
- 重复勾选不覆盖第一次保存的备份。
- 仅恢复仍处于本次安装状态的值。用户后来改了扩展名默认值或创建了新的
  `UserChoice`，不会再恢复会遮盖新选择的旧 Explorer 覆盖项。
- 兼容旧重建版没有 `InstalledPresent` 字段的默认值备份格式。
- 注册表拒绝必要的读写或删除时仍返回实际错误，不把失败显示成关联成功。

本修复还原原版在旧系统上的关联效果，并保留重建版的所有权管理；不宣称与
原版每条注册表写入、权限范围或提交时机完全相同。

## 验证

本地测试源位于 `rebuild/tests/`，关联测试全部重定向到带进程 ID 与时间标记的
独立 HKCU 子树，不改宿主实际文件关联。测试不纳入发行 ZIP，Actions 继续
`BUILD_TESTING=OFF`。

覆盖范围：

- XP、XP x64、Vista、Win7、Win8、Win8.1、Win10 / Win11 的分支选择。
- XP / Win7 默认值、播放命令、加入列表命令、旧 Explorer 覆盖项处理。
- Win7 `UserChoice` 禁止 `KEY_SET_VALUE` 时仍能关联，并在取消后恢复原 ProgID。
- 重复注册 / 取消、后续用户选择保护、旧备份格式恢复。
- 现代系统候选注册不改变默认值、Explorer 覆盖项和 `UserChoice` / Hash。
- 默认程序注册名称、独立 ProgID、图标保留、跨安装所有权及卸载清理。
- 现有文件关联、Audio CD / 文件夹 Shell 集成和临时快捷方式回归。

本次验证结果：

- 普通版 Release 构建成功，`default_programs_tests`、`file_association_tests` 2 / 2 通过。
- XP / Win7 版 Release 构建成功，同两项测试在当前主机 2 / 2 通过。
- 兼容 EXE 静态导入审计通过：x86、子系统 5.01，18 个 DLL、641 项导入均通过
  XP / Win7 导出清单检查。
- 兼容分发构建目录已恢复 `BUILD_TESTING=OFF`。

**隔离测试在当前 Windows 主机运行，不等同于 XP / Win7 实机验证。**
旧系统最终验收应确认：勾选 MP3 等格式、Explorer 双击播放、右键加入列表、
取消后恢复原播放器、手动关联入口，以及改用其它播放器后执行取消 / 卸载。

## 官方参考

- [默认程序注册契约](https://learn.microsoft.com/en-us/windows/win32/shell/default-programs)：
  HKCU / HKLM Capabilities 注册与 ApplicationName、RegisteredApplications 的一致性要求。
- [SetAppAsDefault](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-iapplicationassociationregistration-setappasdefault)：
  调用以已注册应用名称和关联类型为参数。
- [默认程序接口及 Windows 8 起的限制](https://learn.microsoft.com/en-us/windows/win32/shell/vista-managing-defaults)。
