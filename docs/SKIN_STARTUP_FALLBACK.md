# 删除已选皮肤后的启动回退

## 原因与原版依据

旧启动顺序是：读取 `TTPlayerRebuild.xml` → 读所选 `.skn.xml` → 尝试加载
`.skn` → 失败时加载内置皮肤图片 → 创建窗口。启动时 `LoadSkin` 没有 HWND，
不会进入运行时切换皮肤的样式／配置分支。于是旧歌词、播放列表字体和配色、
视觉效果及窗口位置仍然生效，关闭时还可能写入 `Skin/Default.xml`。
即使同时删除 `.skn.xml`，主配置中保存的旧皮肤样式也足以触发问题。

5.7.9 `TTPlayer.exe.pseudo.c` 的 `FUN_0045D5FA`（0045D5FA）在请求包不存在时，
先替换选择器为 `<Default_Skin>`。`CSkinManager_LoadPackageXml`／0045DDEE
绑定实际皮肤后，条件 `非首次加载 || 请求包不存在` 使首次启动的回退也进入
目标配置恢复分支：初始化附属窗口可见性，再读取 `Skin/Default.xml`。
没有目标配置时还清空主窗口保存矩形并初始化相关字体，不能视为普通启动恢复。
本次未使用 6.1.20 的非皮肤逻辑，也未宣称全部原版二进制行为等价。

## 修复

应用入口改用 `PlayerWindow::LoadStartupSkin`，在创建任何窗口、字体或画刷之前完成：

1. 验证实际皮肤包可解析、主窗口区域有效，再决定配置身份。
2. 外置包缺失、损坏或无法绑定时，加载内置包，不读取遗留 `.skn.xml`。
3. 回退时恢复包内歌词／播放列表字体及全部颜色、视觉样式基准，清除旧皮肤的
   六组正常／迷你窗口矩形并初始化正常附属窗口可见性。
4. 在基准上逐字段应用 `Skin/Default.xml`。已有个性化样式和位置优先；配置缺失、
   损坏或缺少字段时使用内置皮肤基准，不混入上次皮肤的值。
5. 保留音量、置顶、歌词滚动行为等无关全局设置；Visual Type/FramesPerSec
   仍是全局值。有效皮肤正常启动时保持已有完整 visual-global 恢复语义。

预加载不写配置。关闭仍按原有流程保存已正确恢复的默认皮肤状态；不删除或改写
被移走皮肤留下的配置，不复制用户配置，不改变运行中切换皮肤的 HWND 生命周期。

## 宿主机验证

`runtime_paths_tests` 使用独立临时目录和合成配置，不操作用户皮肤文件。

- 修复前，实际 EXE 回归稳定报错：`missing package fallback retained the removed skin's styles`。
- `Skin`／`Skin/new` × 包缺失／损坏 × 默认配置缺失／损坏／错误根节点／稀疏配置，
  共 16 组；核对字体、全部歌词／列表颜色、视觉配色、六组矩形、可见性及全局选项。
- 默认皮肤、LX-iPlay 和 PNG 皮肤三种有效包的普通启动保留原有视觉全局值。
- 实际 `TTPlayerRebuild.exe --smoke-test` 共启动五次：首次配置迁移、再次加载 PNG
  皮肤、仅移走所选包后的回退、回退保存后的再次启动、无遗留 sidecar 的根目录包回退。
- Release 构建及 `runtime_paths_tests`、`skin_profile_tests`、`skin_alignment_tests`、
  `skin_png_tests`、`skin_rebind_tests`、`window_topmost_tests`、`taskbar_playback_tests`
  七项宿主机回归通过。

测试对照的原版依据来自上述伪代码，没有重新运行原版做截图对照。如果旧版本已经
把错误值保存到了 `Default.xml`，代码无法区分它们与用户主动定制的值，因此本次
不会自动清除已有默认皮肤配置。
