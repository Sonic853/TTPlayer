# BaiduMusic8209 用户更新包：全 PNG 升级

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
