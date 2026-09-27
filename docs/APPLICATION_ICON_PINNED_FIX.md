# 自定义播放器图标：固定任务栏同步修复

日期：2026-09-28。前置分析及原版函数链见 [APPLICATION_ICON_REFRESH.md](APPLICATION_ICON_REFRESH.md)。

## 问题和原版边界

原版 `0049DDD9 → 0046228D → 004037DF → 0045C937` 会重载 `AppIconFile`，
更新主窗口大小图标、皮肤图标以及通知区域图标。原版没有现代任务栏应用标识、
重新启动属性或固定快捷方式同步。上一轮恢复了原版窗口行为，但不足以改变 Windows
固定按钮的图标来源。

Win7 Aero 实测：旧版即使主窗口、托盘均已变红，固定后的合并按钮以及退出后的启动项
仍显示 EXE 默认图标。对应 `.lnk` 的 `IconLocation` 为空，窗口也没有显式应用标识。

## 本次实现

- 新增 `src/ui/taskbar_icon.cpp`，在原有 `ApplyApplicationIcons()` 路径中发布 Shell 图标。
- 用规范化的 EXE 完整路径生成稳定 `AppUserModelID`：同目录更新版本、更换图标不会
  改变标识，不同便携目录则区分。不会把构建日期或图标内容放进应用标识。
- 为主窗口设置 `RelaunchCommand`、`RelaunchDisplayNameResource`、
  `RelaunchIconResource` 和显式窗口标识；属性值未改变时不重复写入，避免 Win7
  因重复设置标识而重建任务栏按钮。窗口销毁至 `WM_NCDESTROY` 时清理属性。
- 将当前实际采用的大小图标保存为持久 ICO，保留颜色和透明掩码。缓存位于
  `%LOCALAPPDATA%\TTPlayerRebuild\TaskbarIcons\<应用标识>\<内容哈希>.ico`。
  图标内容改变就使用不同文件名，支持用户覆盖同一源 ICO 后再次确认选择；退出后
  保留缓存，固定启动项仍可读取。相同图像复用已有文件。
- 同步当前用户 `User Pinned\TaskBar` 和 `User Pinned\ImplicitAppShortcuts`
  中目标确实为当前 EXE 的链接。后者是 Win7 为带重新启动属性的窗口生成启动信息的
  位置，不能只检查 `TaskBar` 文件夹。保留链接名称、目标、参数、工作目录、说明、
  显示方式和固定位置；不会改动其它安装目录或其它应用的链接。
- 保存链接后发出针对文件的 `SHCNE_UPDATEITEM`。Win7 实测证明此通知本身不足以
  刷新固定分组图标，因此仅在确实修改了对应链接时，追加异步
  `SHCNE_ASSOCCHANGED` 通知，让 Shell 重读缓存。无需重启 Explorer、改写 Taskband
  注册表或程序化取消固定再重新固定。
- 系统关联页面新建桌面、程序菜单、快速启动快捷方式时，也使用当前图标缓存和同一
  应用标识，避免以后从这些快捷方式固定时又回到 EXE 默认图标。
- `SHGetPropertyStoreForWindow` 动态解析；XP、没有此 API 的旧系统继续采用原先
  `WM_SETICON`、窗口类图标及托盘图标路径，没有引入新的现代系统静态入口。

## 已完成验证

### 自动回归

Windows 11（10.0.26200）、Windows 7、Windows XP 本地/虚拟机运行
`skin_rebind_tests --icons` 均通过。测试只保留在本地 `tests` 中。

- 大小图标、窗口类图标、托盘路径；红色 → 绿色 → 同路径覆盖为蓝色。
- mini 模式、换肤、最小化到托盘和还原、任务栏按钮重建、连续重载、恢复默认。
- Win7/Win11 读取生成的 ICO 并检查像素；检查四项 Shell 属性；同名文件覆盖后
  缓存文件名确实变化。
- 独立快捷方式回归检查保留参数、工作目录、说明、显示方式；其它目标不修改，
  相同状态不重写，路径大小写不影响应用标识。
- XP 不进入现代 Shell 属性路径。Release 导入审计保持 x86、子系统 5.01，
  20 个 DLL / 695 个静态导入，XP 和 Win7 库存检查通过。

### Win7 真实固定项升级

为排除前几轮 Shell 缓存干扰，在全新目录
`C:\TTPlayerTray\pinned-upgrade-20260928` 使用旧版实际 `TTPlayerRebuild.exe`：

1. 设置红色图标，从真实任务栏菜单固定。复现主窗口/托盘红色、固定按钮默认图标。
2. 读取旧 `.lnk`：图标为空、没有显式应用标识；保持该固定项，关闭旧版并原位替换 EXE。
3. 点击原固定项启动修复版：原按钮直接变红，仍为一个按钮，位置不变。
4. 读取原链接：目标和参数保留，图标指向持久 ICO，应用标识与窗口一致。
5. 退出后固定按钮继续显示红色；再次点击该按钮可正常启动。
6. 在选项内切换到蓝色 ICO，再选择默认图标：同一个已固定按钮实时同步为蓝色、
   默认图标，不需要取消固定，也不需要重启 Explorer。

证据保存在本地 `tests/artifacts/pinned-upgrade-20260928/`，包括
`before-upgrade.png`、`after-upgrade-red.png`。另一次真实运行覆盖了新建固定项、
蓝色图标和恢复默认的 Shell 缓存行为。

测试后已取消创建的固定项，升级场景检查返回 `matching_pins=0`，关闭测试播放器，
并将 Win7 任务栏恢复为原来的“从不合并”；原有三个固定应用保持不变。

Windows 11 已跑自动回归，本轮未单独操作其真实固定任务栏；Windows 10 未进行独立
实机验证，不将它们写成已完成的真实固定项测试。

## 发行和参考

发行包为 `build/Release/TTPlayerRebuild-2026.09.28.zip`（2,162,043 字节），
SHA-256：`7fdc439c76de9b84aef6c4ec345476979e9342fa0fa8b86961311f951496ff24`。
仅包含 `TTPlayerRebuild.exe`、`TTPUpdater.exe`、`AddIn/ttp_https.dll` 和
`SHA256SUMS.txt`，逐项校验通过。测试源码不提交，发行包不包含测试，Actions 仍然
使用 `BUILD_TESTING=OFF`。

- [Microsoft：应用标识及快捷方式一致性](https://learn.microsoft.com/en-us/windows/win32/shell/appids)
- [Microsoft：固定/分组图标资源](https://learn.microsoft.com/en-us/windows/win32/properties/props-system-appusermodel-relaunchiconresource)
- [Microsoft：窗口属性生命周期](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shgetpropertystoreforwindow)
- [Microsoft：保存快捷方式并通知 Shell](https://devblogs.microsoft.com/oldnewthing/20150903-00/?p=91671)
