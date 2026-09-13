# BaiduMusic8209 迷你皮肤布局

本次是按用户参考图改编皮肤，不将百度 8.2 的非皮肤行为作为 5.7.9 的恢复依据。

## 布局与素材

转换模板位于本地 `BaiduMusic8209/compat/src/Skin.xml`，生成器为同目录
`ConvertSkin.cs`，输出为仓库根目录 `Skin/BaiduMusic8209.skn`。

- 对照源包 `main_mini.xml`，主窗从 370×34 改为 370×35，不再拉伸正常窗口标题条。
- 使用原包 `mini_song_info_bg.png` 合成深色歌曲信息底框，使用 `mini_cut_line.png`
  合成音量与窗口按钮之间的分隔线；播放等按钮保留原始 PNG 四态和透明通道。
- 按源 XML 的外层 1 像素容器修正按钮位置及尺寸，歌曲信息区不覆盖“词”按钮。
- 右侧歌词窗沿用原有迷你模式的独立矩形、200 像素缺省宽度和跟随主窗高度的规则。
  已保存的位置与宽度不强制重置，也不复制正常模式或内置皮肤配置。
- `mini_border` 改用旧解析器真正读取的 `left_top_color` / `right_bottom_color`，
  避免错误属性名导致边框退回普通歌词文字色。

## 可选 mini_lyric 扩展

旧版 `004A88D0` 解析歌词窗口，`004495C8` 使用缺省内边距 `{2,2,4,2}`，
`00449313` 绘制迷你边框。旧版共用正常模式的字体和颜色，不能单靠旧属性表达
8.2 独立迷你歌词外观。因此新增一个**明确属于重建版的可选节点**，不是声称原版已有它：

```xml
<lyric_window ...>
  <mini_border left_top_color="#323f6c" right_bottom_color="#323f6c"/>
  <mini_lyric Font="-12,0,0,0,400,0,0,0,1,0,0,4,0,宋体"
              TextColor="#646464" HilightColor="#323f6c"
              BkgndColor="#f5f5f7" padding="6,6,6,6"/>
</lyric_window>
```

`Font` 复用现有 LOGFONT 描述符解析；颜色、字体各自可省略；padding 是四边内缩量。
未声明、无效字段仍使用原有值，不改变现有皮肤的行为。正常模式、全屏模式不使用这些覆盖值。
迷你模式中的显式样式优先于共用歌词样式，但不会写入 `TTPlayer.xml` 或 `.skn.xml`。

切换后重建活动字体，父窗和歌词控件使用一致的背景色，颜色键透明也使用同一颜色。
主窗、歌词窗仍复用已有 HWND，显隐、拖动、关闭、位置保存沿用现有流程。
歌曲名和歌词保持实时绘制，不把截图中的宣传文字烘焙进皮肤图片。

## 验证

- Release、Debug 构建通过；两者的 Skin 目录已同步新包。
- `skin_png_tests` 新增可选节点/缺省/无效值、正常↔迷你字体和颜色切换、
  小窗口内边距、边框像素、透明颜色键、全屏优先级与用户设置不变回归。
- `skin_profile_tests`、`skin_alignment_tests`、`desktop_lyrics_menu_tests` 一并回归。
- `BaiduMusic8209/compat/test-mini.ps1` 在宿主机隔离目录使用真实鼠标验证进入迷你、
  歌词按钮显隐、两个窗口联动拖动、静音按钮、最小化、恢复、再次进入时保留位置以及关闭。
  可选 `-WithTrack` 使用现场生成的静音 WAV 验证动态歌曲文字，不读取或更改用户音乐。
- `test-host.ps1` 对原版 5.7.9＋辅助 BMP 包、重建版＋PNG 包检查主窗、均衡器、
  搜索/打开文件、迷你切换及恢复。原版忽略 `mini_lyric`，此项不表示新版迷你外观在原版上相同。

源 ZIP 未改动。所有测试配置位于隔离目录；不覆盖用户的活动配置。

最终 Release 动态歌曲文字及鼠标流程结果：
`BaiduMusic8209/compat/mini-tests-20260912-195142/results.json`；
实际屏幕截图为同目录 `mini-pair.png`。4 项 CTest 回归均通过。

## 2026-09-13：卡拉 OK 配色修复

上次示例把 `TextColor` 和 `HilightColor` 都设为 #646464，导致
`0043FC10` 对应的两次裁剪绘制使用同色，卡拉 OK 看起来没有效果。
此次仅将迷你高亮色改为与皮肤主题一致的 #323f6c，普通文字仍为 #646464。
不更改卡拉 OK 开关、渲染算法、全屏或普通歌词设置，也不覆盖用户修改的布局。

`skin_png_tests` 现在用真实歌词绘制路径验证水平/垂直两种滚动、带 offset 的
25%/50%/75% 时间边界、两种颜色的像素数量和菜单开关。
宿主机用相同静音 WAV/LRC 对比旧包和修复包：旧包两种滚动的高亮像素均为0、
开关画面无差异；修复包均出现分色，关闭后整行高亮，再开启恢复。
详见本地 `BaiduMusic8209/compat/karaoke-tests-20260913-160820`（修复前）、
`karaoke-tests-20260913-160957`（配色修复后）及
`karaoke-tests-20260913-161937`（全PNG包）。

用户更新的皮肤位于 `Skin/new/BaiduMusic8209.skn`。
全PNG升级及逐文件保留规则见 `BAIDU_SKIN_PNG_UPGRADE.md`。
