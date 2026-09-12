# BaiduMusic8209 主窗口与工具栏布局

2026-09-13，按用户截图与后续要求改编。这是经用户同意的可选皮肤扩展，
不是声称 5.7.9 原版解析器已有任意工具栏坐标的能力。

## 工具栏

原版 `00482BAF` 使用菜单 139 的七个组，原有顺序仍为添加、删除、列表、排序、
查找、编辑、模式。新增 `/skin/playlist_window/toolbar/item`，只改变各组的
绘制、命中、悬停提示和菜单弹出位置，不改组索引及命令。

```xml
<toolbar position="8,145,367,201" image="Toolbar.bmp" hot_image="ToolbarHot.bmp">
  <item index="0" position="63,28,112,56"/>
  <item index="1" position="161,28,211,56"/>
  <item index="2" position="211,28,260,56"/>
  <item index="3" position="112,28,161,56"/>
  <item index="4" position="207,2,225,20"/>
  <item index="5" position="260,28,309,56"/>
  <item index="6" position="309,28,359,56"/>
</toolbar>
```

坐标相对工具栏原点，右/下边界不包含。索引必须为 0..6，矩形必须非空且位于
工具栏内。至少一个有效 item 才启用扩展；未声明的项目隐藏。未声明或全部无效
时仍走原来的七格布局，包括高度大于 30 像素时查找位于第二行的规则。

显式布局的 normal/hot 图片是整张合成图，按 1:1 裁剪对应按钮；悬停不重排或
拉伸其它按钮。`PlaylistToolbarItemBounds` 统一命中、工具子窗和绘制边界。

最终外观：左侧“我的音乐”标题宽 70 像素（不含窗口左边框），右侧 296 像素
均分给添加、排序、删除、列表、编辑、模式，分别为 49、49、50、49、49、50 像素。
查找使用源包放大镜，独立放在打开文件框右侧。打开框仍调用文件选择，查找仍
打开含“快速查找”的菜单，不伪装百度网络搜索。仅移除主窗列表显隐按钮，保留
工具栏的列表管理菜单。用户已保存的目录列分隔宽度不强制重写。

## 进度与音量

进度旋钮复用源包 `play_slider_thumb.png`，正常/禁用帧改为全透明，悬停及按下帧
保持原素材。使用已有四态绘制和捕获机制，拖动离开控件时仍显示，松开后隐藏。

音量问题有两个独立原因：

- `PaintSkin` 的音量分支漏绘 `bar_image`。对照 `00451E07`，补上背景→填充→旋钮
  的顺序，同时限制在控件范围内；未提供背景图的皮肤不增加默认图形。
- 本皮肤音量控件宽 73，旋钮宽 18，`00428F41` 的两端内缩各 1，因此中心行程
  为 `73 - 18 - 2 = 53` 像素，中心从 x=294 到 x=347。旧填充图宽 73，零值时
  仍在旋钮左侧画出 10 像素，看起来无法归零。改为居中的 53×4 填充图，底槽
  复用进度条 `play_slider_bg` 的深色素材。保留原音量旋钮、音量填充素材和
  0..100 数值算法，不修改静音、音频输出、均衡器或迷你音量行为。

## 保留与验证

采用 `BaiduMusic8209/compat/update-main-layout.ps1` 增量修改用户的包，不运行旧的
全量生成器。仅修改 Skin.xml、Main.bmp、Playlist.bmp、Toolbar.bmp、ToolbarHot.bmp、
Open.bmp、VolumeBar.bmp、VolumeFill.bmp，新增 ProgressHoverThumb.png；其余 68 个
条目逐字节不变。主窗/播放列表背景在 y=139..201 以外不变。均衡器、歌词、迷你、
桌面歌词 XML 保持用户版本。不改 TTPlayer.xml、已有 .skn.xml 或源 skin.zip。

- Release/Debug 构建通过。
- `skin_png_tests` 新增七按钮坐标、全区域命中、悬停裁剪、隐藏项、无效输入和
  旧布局回退；音量增加零/中/满值、拖出边界、底槽绘制及三层顺序检查。
- `skin_png_tests`、`skin_profile_tests`、`skin_alignment_tests`、
  `desktop_lyrics_menu_tests` 全部通过。
- 宿主机隔离运行目录 `BaiduMusic8209/compat/main-tests-20260913-042740`：真实鼠标
  验证七个菜单、工具子窗、菜单位置、打开文件框、旋钮四态，以及音量拖到两端。
  最小值退出保存为 `Volume=0`；正常响应并关闭。测试用静音 WAV，不改用户配置。

皮肤 SHA256：`AED05762C4312623B751CF791B64A927CF6A8BFB4F8FE161640DD48443ADAEBA`。
