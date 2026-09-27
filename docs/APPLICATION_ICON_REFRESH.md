# 播放器自定义图标与任务栏刷新

> 2026-09-28：固定任务栏同步现已补齐。实现、真实旧固定项升级测试与发行说明见
> [APPLICATION_ICON_PINNED_FIX.md](APPLICATION_ICON_PINNED_FIX.md)。本文保留前两轮分析与问题复现记录。

日期：2026-09-27。

## 原版是否实现

实现了播放器运行窗口的图标切换。依据是根目录
`reverse/decompiled/TTPlayer.exe.pseudo.c`，完整调用链如下：

1. `FUN_0049135B` 把系统关联页的按钮 `2106 / 0x83A` 分发到
   `FUN_0049DDD9`。
2. `FUN_0049DDD9` 打开菜单资源 `154 / 0x9A`：`33000` 恢复默认，
   `33001 / 0x80E9` 选择图标，写入全局 `AppIconFile`。
3. 确认选择后，无论文件名是否变化，都向播放器发送
   `SendMessageW(main, 0x7F0, 2, 0x83A)`。
4. `FUN_0046228D` 对该来源调用 `FUN_004037DF` 重载图标，随后调用
   `FUN_0045C937` 发布图标。
5. `FUN_004037DF` 用 `ExtractIconExW` 获取大、小图标；失败时使用程序资源
   `128`。
6. `FUN_0045C937` 优先使用明确指定的自定义图标。没有自定义图标时，优先使用
   皮肤图标，再回退到程序图标。它更新皮肤的图标控件，并发送：

   ```cpp
   SendMessageW(main, WM_SETICON, ICON_SMALL, smallIcon);
   SendMessageW(main, WM_SETICON, ICON_BIG, largeIcon);
   ```

7. 同一函数经 `FUN_00451B5B` 调用
   `Shell_NotifyIconW(NIM_MODIFY, ...)` 更新系统托盘图标。

所以该选项控制播放器本身的图标，不局限于关联文件的图标。

## 重建版发现的问题和修复

此前已存在 `WM_SETICON`，不能将此次现象简单归因为“没有实现任务栏图标”。
修复前，在独立、未固定的运行窗口中，本机和 Win7 都能看到自定义任务栏图标。
本次进一步确认并修复了以下不完整之处：

- 原来先销毁旧图标，再读取新图标。改为先准备新图标，发布后再释放旧图标，
  避免窗口/窗口类仍引用正在销毁的图标。
- 窗口类的大、小图标此前一直保持创建时的值。现在同步更新
  `GCLP_HICON / GCLP_HICONSM` 和 `WM_SETICON`，统一窗口及系统回退读取结果。
  本地测试在修复前明确检出了窗口图标已变化、窗口类图标仍为旧像素的差异。
- 原来确认同一路径的图标文件不会重载；现在与原版一致，每次确认选择都重载，
  支持用户先覆盖 ICO 内容再重新选择同一个文件。
- 选择图标、全部保存/重置、初始化和换肤统一调用 `ApplyApplicationIcons()`；
  同步刷新托盘和皮肤绘制，避免不同入口处理不同。
- 接到 `TaskbarCreated`、`TaskbarButtonCreated` 时重新发布图标，覆盖 Explorer
  或任务栏按钮重新建立的情况。

以上调用均兼容 XP，没有新增只在新系统存在的静态导入。

## Windows 固定项、合并按钮的边界

原版上述调用链没有修改已经存在的任务栏固定快捷方式，也没有设置
`AppUserModelID / RelaunchIconResource`。

运行窗口图标与任务栏分组/固定快捷方式的图标来源可能不同。微软说明，合并后的
任务栏分组可以从开始菜单、桌面快捷方式或 EXE 取得图标；固定快捷方式也有独立的
图标信息。因此，`WM_GETICON` 正确不等于所有固定项都会被同步改写。

- [微软：合并后的任务栏分组图标来源](https://devblogs.microsoft.com/oldnewthing/20150812-00/?p=91831)
- [微软：System.AppUserModel.RelaunchIconResource](https://learn.microsoft.com/en-us/windows/win32/properties/props-system-appusermodel-relaunchiconresource)

第一轮修复恢复原版的运行窗口/托盘语义，没有更改用户的固定快捷方式、分组身份或
Explorer 设置。随后按用户要求进行了真实固定项验证，结果见下文；不能将第一轮
修复表述为已经解决固定项图标同步。

## 验证

测试仅位于本地 `tests/ui/application_icon_tests.inc`，由本地
`skin_rebind_tests --icons` 运行。没有提交测试代码，Actions 保持
`BUILD_TESTING=OFF`，发行包不含测试。

实际通过：Windows 11（10.0.26200）、Windows 7 虚拟机、Windows XP 虚拟机。

- 默认图标 → 红色自定义 ICO → 绿色自定义 ICO。
- 同一路径覆盖为蓝色 ICO 后重载，逐像素检查大、小窗口图标。
- 窗口类图标与窗口图标一致。
- 选项窗口打开时更新、切换迷你模式、重新加载皮肤、最小化到托盘再还原。
- 模拟任务栏按钮重建通知，连续重载 30 次。
- 清除自定义图标后恢复皮肤/默认图标。
- XP、Win7 的真实任务栏截图中，运行按钮和托盘均显示新的蓝色图标。

第一轮测试截图留在 `tests/artifacts/application-icon-20260927/`；该轮没有测试
真实固定项。Windows 10 没有单独运行虚拟机验证。

Release 导入审计通过：x86、子系统 5.01、20 个 DLL / 695 个导入。
输出包为 `build/Release/TTPlayerRebuild-2026.09.27.zip`。

## 第二轮：真实任务栏固定项验证

时间：2026-09-27 23:42–23:49（北京时间）。环境为 Windows 7 Aero 虚拟机。
使用上一轮 Release 的真实 `TTPlayerRebuild.exe`，放在独立目录
`C:\TTPlayerTray\pinned-icon-20260927`，不使用单元测试窗口代替产品窗口。

通过虚拟机的真实任务栏菜单固定程序，再通过“选项 → 系统关联 → 选择播放器图标
→ 自定义图标”选择红色 ICO，保存、退出并点击固定项重新启动。随后临时切换任务栏
的合并模式，并验证在自定义图标已经启用时取消固定、重新固定的结果。

| 场景 | 实际结果 |
| --- | --- |
| 已固定，任务栏“从不合并”，运行中选择自定义图标 | 主窗口、运行按钮、托盘均变红 |
| 退出程序，查看留下的固定启动项 | 固定项仍为 EXE 默认图标 |
| 从固定项重新启动，“从不合并” | 主窗口、运行按钮、托盘恢复红色，配置未丢失 |
| 运行中切换为“始终合并、隐藏标签” | 主窗口和托盘保持红色，任务栏分组按钮改为默认图标 |
| “始终合并”下取消固定 | 运行按钮立即显示红色 |
| 保持红色图标，再次通过任务栏菜单固定 | 按钮重新显示默认图标 |

通过 `IShellLinkW` 读取真实固定项，得到：

```text
target=C:\TTPlayerTray\pinned-icon-20260927\TTPlayerRebuild.exe
icon=,0
args=
matching_pins=1
app_icon="C:\TTPlayerTray\pinned-icon-20260927\red.ico",0
```

`icon=,0` 表示快捷方式没有独立图标路径，继续使用目标 EXE 的图标。应用自己的
`AppIconFile` 已经保存且重启有效；两者是不同来源。这次已实际复现报告中的一种
场景：窗口和托盘图标正确，但固定的合并任务栏按钮保持旧图标。

### 结论与后续修复范围

**固定项场景未通过自定义图标同步验证，上一轮修复不足以覆盖它。重新固定也不是
有效修复方法。** 这不是歌曲或图标文件读取失败，也不是单纯漏发 `WM_SETICON`。

原版分析中找到的是窗口和托盘更新路径，没有找到更新固定项/现代分组图标的实现。
要让该设置同时控制现代任务栏固定项，需要单独补充 Shell 集成：

1. 为新生成的固定项提供稳定的应用身份、重新启动信息和
   `RelaunchIconResource`；仅增加后一个属性、却没有窗口级显式 AppUserModelID，
   会被系统忽略。
2. 处理已经存在的对应快捷方式和 Shell 通知，保持原目标、启动参数以及用户的
   固定顺序；不能只更新窗口属性就声称旧固定项也已更新。
3. 验证恢复默认、换肤、图标文件覆盖、退出/重启，以及升级前已经固定的情况。

这轮按要求完成验证和原因分析，没有据此新增应用身份或改写用户已有的快捷方式。
产品源代码及 Release 包未在这一轮修改。Windows 10/11 的真实固定项没有在此轮
操作，因此本表仅报告 Win7 实测结果。

### 证据与清理

本地证据目录：`tests/artifacts/pinned-icon-20260927/`。

- `win7-combined-before.png`：红色主窗口/托盘与默认任务栏图标同时存在。
- `win7-repinned-custom.png`：先启用自定义图标，再固定，任务栏仍为默认图标。
- `win7-pinned-player.lnk`：只用于复核的固定快捷方式副本。
- 本地 `skin_rebind_tests --pin-inspect` 只读取与测试目录 EXE 完全匹配的固定项。

验证结束后已经取消本次创建的固定项，读回 `matching_pins=0`；关闭测试播放器，
并将虚拟机任务栏恢复为测试前的“从不合并”。本地测试代码不提交、不打包，Actions
仍不运行测试。
