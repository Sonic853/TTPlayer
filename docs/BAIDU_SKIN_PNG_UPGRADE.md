# BaiduMusic8209 重建版 PNG 皮肤

## 2026-10-26 当前修改

以当前 `Skin/new/BaiduMusic8209.skn` 为输入，转换 24 张 BMP，明确保留
`Playlist.bmp`、`Toolbar.bmp`、`ToolbarHot.bmp` 的原始字节。最终为 72 张 PNG、
3 张 BMP、3 个 XML；原有 47 张 PNG 不变，控件坐标、大小、字体、颜色和工具栏区域不变。
源 `BaiduMusic8209/Languages/zh-cn/skin/skin.zip` 不变。

播放压缩包音频会产生缓存百分比。旧包未提供 `fill_image2`，宿主自动暗化整个
`ProgressFill.bmp`，连已经合成的蓝色背景一起暗化，形成矩形条块。现在进度轨道与
播放填充使用源 PNG 的 Alpha，新增 `ProgressCache.png`，来自 `play_slider_cache.png`，
在 XML 中显式绑定 `fill_image2`。背景、缓存、播放填充、滑块依次合成。

音量区域保留用户修改过的细轨道：`VolumeBar.bmp` 和 `VolumeFill.bmp` 按原像素转成 PNG。
源 ZIP 的 `volume_slider_bg.png` 是 85×29 的面板，不能拉伸到本皮肤 73×18 的控件来代替细轨道。
原有圆形 `VolumeThumb.png` 不变。0%、50%、100% 音量与原 BMP 皮肤的运行时绘制逐像素一致。

PNG 按钮新增可选 `hit_test="rect"`，使透明留白也响应鼠标；`hit_test="alpha"` 或省略
属性时仍按正常帧 Alpha 命中。主窗口、迷你窗口的最小化/模式切换/关闭，以及歌词和均衡器
的关闭按钮启用矩形命中。虚拟控件的按下、悬停、捕获后释放，以及原生子窗口使用一致规则。
命中区域不作为 PNG 的绘制遮挡区域，避免透明留白遮掉下层控件。

复现方式：

```powershell
powershell -NoProfile -File BaiduMusic8209/compat/upgrade-to-png.ps1 -RebuildControls -OutputDirectory <新目录>
python BaiduMusic8209/compat/verify-rebuild-png.py <原包> <候选包> <验证目录>
rebuild/build/Release/skin_png_tests.exe <仓库根目录> <候选包> <验证目录>/silent-audio.zip
powershell -NoProfile -File BaiduMusic8209/compat/test-png-windows.ps1 -RectangularButtons -Package <候选包>
```

验证覆盖：24 张图片尺寸与所有 XML 布局、3 张保留 BMP 与 47 张原 PNG 字节、
6 个标题按钮共 1944 个像素命中、透明角落悬停/按下/释放、实际最小化/恢复/模式切换/关闭、
24 次进度重绘、真实 ZIP WAV 的播放/缓存完成/暂停/跳转/恢复/停止，及旧有 40 次 PNG 播放暂停切换。
宿主窗口、搜索/添加/预设菜单、文件对话框、音量及均衡器拖动通过。181 个旧皮肤加载通过。

当前证据目录：`rebuild/out/analysis/baidu_png_upgrade_20261026/`。
最终候选为 `candidate-v2/BaiduMusic8209.skn`，SHA256：
`E4D28C3CC37757464751AC3C0A4C5D02C199B75049D8A6C4A155D4D20EB6245D`。
使用 `rebuild/build/Release/TTPlayerRebuild.exe` 及其配套运行库运行；仓库根目录的原版 DLL
不支持当前重建版的归档接口，不能用来验证压缩包音频。

## 2026-09-13 历史升级记录（以下不是当前包清单）

输入为用户修改后的 `Skin/new/BaiduMusic8209.skn`，不是重新运行早期的皮肤布局生成器。
原始素材来自 `BaiduMusic8209/Languages/zh-cn/skin/skin.zip`，该 ZIP 保持不变。

## 处理范围

27 张 BMP 全部替换为真正的 PNG，最终包包含74张PNG、3个XML，不残留BMP。
原有47张PNG的文件内容逐字节保留。XML仅替换对应图片文件名；原有控件坐标、
工具栏7个独立区域、208像素高均衡器、进度条和音量滑块尺寸、字体、颜色等全部保留。
此前已经修好的迷你卡拉OK配色也保留。

| 处理 | 文件 |
| --- | --- |
| 使用原 ZIP 对应图片重新生成，保留源Alpha、按现有尺寸及九宫格适配 | Logo、ProgressBar/Fill、VolumeBar/Fill、EqBar、BalanceBar、ScrollThumb、Splitter |
| 原 ZIP 按钮背景＋旧式命令文字，仍为四态 | Open、Profile、Reset、List |
| 原 ZIP 的 btn_search 图标；其余自定义工具栏内容保留 | Toolbar、ToolbarHot |
| 保留已编辑合成内容，无损转换；原色键改为Alpha=0 | Main、LyricsWindow、EqualizerWindow、MiniWindow、DesktopBar、Playlist、Digits、ScrollBar、ScrollButtons、Selected、SplitterArrow、Stop |

最后12张是适配旧窗口体系的合成图、文字或简单色块，原ZIP没有可直接替换的同尺寸图片。
没有拿新播放器整张窗口覆盖它们，也没有重新生成用户修改的标题、列表布局和数字字体。
其非色键像素经逐像素比较完全一致。均衡器轨道之外的现有填充也保留。

## 可复现工具

本地工具位于 `BaiduMusic8209/compat/`：

- `upgrade-to-png.ps1`：调用 `src/ConvertSkin.cs` 的原素材读取/九宫格代码及
  `src/UpgradePng.cs`，生成独立候选包，不覆盖源文件。
- `verify-png-upgrade.ps1`：检查27张尺寸不变、47张原PNG字节不变、12张合成图像素不变、
  XML非图片设置不变、PNG签名及82处图片引用。
- `deploy-upgraded-package.ps1`：部署前验证旧包哈希，保留完整备份后原子替换；不操作配置XML。
- `repair-mini-karaoke.ps1`：仅修复旧包的同色 mini_lyric 高亮属性，保留所有其他ZIP条目。

最终转换证据：`BaiduMusic8209/compat/png-upgrade-final-20260913/`。
`assets/mapping.csv` 逐文件记录源素材及保留策略。
分隔条采用 `common_vert_split.png` 的实色中段，避免把渐隐端点反复平铺成虚线。
初次转换前的完整包仍保存在 `png-upgrade-20260913-161305/backups/`，最初的同色歌词包
另存于 `karaoke-repair-20260913/`；均可恢复。

原 ZIP SHA256：`81FA4CEBA1271BA313E8B2E5AF26EA35DC7C09517D6EF5B2AF8BC6F94DA57621`。
最终包 SHA256：`A2FD00CDCF2C3168461B8DFA2DD30A4B94CB498EEE1FFDB94EF975BB2A6C6119`。

## 宿主机验证

- `test-png-windows.ps1`：主窗/播放列表、歌词、均衡器无紫色；搜索、添加、预设菜单，
  音量/均衡器拖动和现代文件对话框；进程响应和正常退出。
- `test-mini.ps1`：370×35与200×35迷你窗口、显隐、联动拖动、最小化、恢复、再进入。
- `test-mini-karaoke.ps1`：静音WAV＋时间戳LRC，两种滚动下分色与开关恢复。
- `test-desktop-toolbar.ps1`：330×32 PNG工具栏、预设菜单、A+/A−、行高及保存42像素字号。
- `skin_png_tests`：所有PNG绘制槽位、颜色键与Alpha、滑块/工具栏、迷你歌词像素回归。

测试仅使用隔离目录的配置。脚本已适配当前 `TTPlayerRebuild.exe` / `TTPlayerRebuild.xml`
命名，未将新配置文件的变化误判成原配置没有保存。

最终包的宿主机结果位于 `BaiduMusic8209/compat/png-host-20260913-162555/results.json`
及 `BaiduMusic8209/compat/karaoke-tests-20260913-162602/results.json`；后者确认两种滚动模式
均同时存在普通/高亮像素，关闭卡拉OK改变显示，重新启用后恢复原像素。桌面工具栏结果为
`BaiduMusic8209/compat/toolbar-tests-20260913-162131/results.json`。
Release、Debug主程序构建通过；Release下 `skin_png_tests`、`skin_profile_tests`、
`skin_alignment_tests`、`desktop_lyrics_menu_tests` 四项回归全部通过。

维护用户更新包时不要直接运行旧 `build.ps1` 覆盖它：该脚本从历史模板重新生成布局。
本次使用 `upgrade-to-png.ps1` 从实际用户包转换，后续修改应以 `Skin/new/BaiduMusic8209.skn`
为准，保留现有的布局和自定义素材。
