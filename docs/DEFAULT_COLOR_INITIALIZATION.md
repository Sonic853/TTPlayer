# 从资源 DLL 初始化通用配色

> 2026-09-21 补充：相关功能已在 XP SP3 / Win7 SP1 虚拟机中执行回归。覆盖项、修复和未覆盖边界见 [虚拟机验证记录](XP_WIN7_VM_VALIDATION.md)。下文保留原日期的历史结论。

## 行为

根据用户要求，通用初始化的颜色以 EXE 同目录 `ttpres.dll` 的
`ZIP/<DEFAULT_SKIN>` 为来源，读取：

- `Playlist.xml`：正文、高亮、序号、时长、选择背景、第一及第二背景，共 7 色。
- `Lyric.xml`：普通窗口歌词正文、高亮、背景，共 3 色。
- `Visual.xml`：频谱顶部、底部、中部、峰值、波形及文字，共 6 色。

`PlaylistSettings`、`LyricSettings`、`VisualSettings` 与 `PlaylistSkin`、
`LyricSkin` 共用同一个只读基准。新建设置、读取不完整主配置、恢复所有默认设置及
单独恢复可视化设置都会使用它，不再分别保留重建版灰色初值和历史旧皮肤初值。

当前 5.7.9 DLL 因而提供列表背景 `#4B6782`、交替背景 `#405B76`、
正文 `#8BBAC6`、高亮 `#FFFFFF`、选择渐变起点 `#88AACB`；
歌词背景为 `#31475B`，可视化六色均为 `#27435F`。
这些数值没有写死为正常初始化常量，更换 DLL 默认包后重启即可采用新资源中的颜色。

仅更改普通皮肤控制的颜色。可视化 Type/FPS、全屏歌词和桌面歌词的独立设置
继续按各自的初始化及配置规则工作。字体和位图不是此次配色读取接口的内容。

## 实现

`skin/default_colors.cpp` 首次使用时通过 `GetModuleFileNameW` 定位 EXE 目录，
以 `LOAD_LIBRARY_AS_DATAFILE` 打开该目录的资源 DLL，并用同目录 `ttpcomm.dll`
已有的 ZIP 解压接口读取三个 XML。XML 在内存流中解析，不解压皮肤图片、不写临时
配色文件，也不查找工作目录、源代码目录或 `Skin/Default.xml`。

结果按进程缓存一次。修改设置对象不会污染共享基准，也不必在每次新建布局或
重置设置时重复读取资源。缺失/损坏资源或字段使用原版 `00401E96` 的历史常量作为
异常回退，避免配置对象含未定义颜色；单个 XML 失败不会丢弃其它两个 XML 的有效颜色。

## 旧皮肤的继承规则

`PlayerWindow::LoadSkin` 将当前设置传给 `LegacySkin::Load`，后者先继承当前
播放列表和普通歌词颜色，再解析目标包。首次启动的当前设置已经以 DLL 配色初始化。

- 目标包明确给出的有效颜色覆盖当前值。
- 缺失或无效的普通颜色字段保留当前值。
- 配色 XML、节点缺失或解析失败时保留已有基准。
- 原版的特殊情况：包内 `PlayList` 节点存在，但 `Color_Select` 缺失/无效时，
  使用系统 `COLOR_HIGHLIGHT`。DLL 默认包的同一字段也遵守此规则。
- 主配置和皮肤旁配置仍逐字段覆盖；其中无效的 `Color_Select` 保留当前值，
  不套用包内 XML 的系统色例外。

现有选择渐变、系统选中文字、播放高亮与时长颜色的绘制规则继续适用，详见
[原版配色分析](ORIGINAL_DEFAULT_PALETTE.md)。

## 验证

新增 `default_colors_tests`：

- 在隔离子进程读取原始 DLL，核对全部 16 色。
- 构造另一份具有 16 个不同颜色的资源 DLL，验证设置对象、布局对象和 Reset All
  随 DLL 改变，从而检查实际资源读取路径。
- 子进程工作目录放置另一份 DLL，确认只使用 EXE 同目录资源。
- 缺少 DLL、损坏 DLL、XML 损坏与部分字段缺失的回退。
- 旧皮肤缺失配色文件、稀疏字段、无效字段、Color_Select 特例及主配置覆盖。

`runtime_paths_tests` 的实际发行 EXE 首次启动/重启检查扩展为全部 16 色，
同时保留主配置与皮肤配置的 72 组覆盖测试和启动回退测试。

本次 Release 验证结果：

- `default_colors_tests`、`runtime_paths_tests`、`skin_profile_tests`、
  `skin_rebind_tests`、`ttplayer_tests` 全部通过。
- XP/Win7 发行 EXE 也通过实际首次启动、重启和 16 色保存检查（当前 Windows 宿主）。
- XP/Win7 EXE 的 18 个 DLL / 598 个静态导入通过 XP 与 Win7 清单检查，
  PE 子系统版本为 5.01；没有在 XP/Win7 实机上运行此次测试。

更新产物：

- `../TTPlayerRebuild 2026.09.15-dll-palette.7z`：现代版完整包。
  从原始测试包重新打包，153 个文件中仅替换 EXE，其余 152 个文件的 SHA-256 相同。
- `build/Release/TTPlayerRebuild-XP-Win7.zip`：兼容版 EXE、导入报告和许可文件，
  解压到已有完整播放器目录使用。
- `build/Release/SHA256SUMS.txt`：现代 EXE 与兼容版 ZIP 的校验值。

完整包 EXE SHA-256：
`1392b09a0434f58b7b2302eb19e7db181fbc8c280fae815ccea2151d7c74c60c`。
XP/Win7 EXE SHA-256：
`8d59a5012f52fe5c9f7f42abd313c1e8e747e8e51a754c8219481d6e46b67ec0`。

已有配置中的明确颜色仍会覆盖新基准。升级不会自动删除已经保存的历史配色或
用户选择；无配置发行目录与已有配置目录因此可能显示不同颜色。
