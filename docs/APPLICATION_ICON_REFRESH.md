# 播放器自定义图标与任务栏刷新

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

本次恢复原版的运行窗口/托盘语义，没有更改用户的固定快捷方式、分组身份或
Explorer 设置。用户尚不确定发生问题时是否已固定到任务栏，因此不能将此次
修复表述为已复现并排除了所有固定项缓存场景。

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

测试截图留在 `tests/artifacts/application-icon-20260927/`；没有修改真实固定项
来声称完成固定项测试。Windows 10 没有单独运行虚拟机验证。

Release 导入审计通过：x86、子系统 5.01、20 个 DLL / 695 个导入。
输出包为 `build/Release/TTPlayerRebuild-2026.09.27.zip`。
