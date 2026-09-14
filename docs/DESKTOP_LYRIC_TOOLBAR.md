# 桌面歌词工具栏：PNG 与可选字号按钮

本次用于 `Skin/BaiduMusic8209.skn` 的330×32工具栏改编；不把百度8.2的非皮肤逻辑作为
5.7.9恢复依据，也不改变现有皮肤的按钮坐标。

- PNG按钮不再通过兼容HBITMAP视图做SRCCOPY，而是调用 `SkinImage::Draw` 在父背景上合成。
  BMP保持原有不透明复制路径，避免改变老皮肤的颜色键行为。
- 皮肤存在有效 `desklrc_bar` 时，只显示该皮肤声明了图片和有效矩形的按钮。
  缺失节点不再留下默认位置的透明点击区；相关悬停提示同步注册/移除。
  无有效皮肤时仍使用原有11个按钮和图标，共12个提示。
- 可选 `zoomin`、`zoomout` 使用与其他按钮相同的四态图片和 `position` 属性。
  没有声明它们的旧皮肤不显示这两个按钮。
- A+/A−为重建扩展：每次调整2像素，限制12～96像素，写回已有 `DeskLrc/Font`，调用
  `ApplySettings` 更新字体、行高、位置和渲染缓存。悬停文字位于EXE资源中，
  不假定旧 `ttpres.dll` 包含新增命令。其内部命令为0xE926/0xE927。
- 适配包的网格图标对应 `settings`，打开原有预设菜单（首项0x80A2），不是 `list` 的播放曲目菜单。

验证：

```powershell
cmake --build rebuild/build --config Release --target ttplayer_rebuild desktop_lyrics_menu_tests
ctest --test-dir rebuild/build -C Release -R '^desktop_lyrics_menu_tests$' --output-on-failure
```

新增合成PNG用例验证全透明/半透明合成、四态中的悬停切换、重复重绘、缺失节点与换肤恢复、
提示注册、字号上下限和预设菜单映射。已有12个5.7.9资源提示、动态播放菜单、真实Win32菜单
初始化/样式用例继续通过。该测试仍依照既有约定使用仓库上级的可选 `ttpres.dll` 资源夹具。

另在宿主机隔离运行Release：真实点击预设、A+、A−，验证330×32工具栏、40→42→40字号对应
60→63→60行高，以及退出后保存42像素字号。脚本及截图保存在本地
`BaiduMusic8209/compat/test-desktop-toolbar.ps1` 和 `toolbar-tests-20260912-192253`。
Debug及Release均已构建。用户原有设置和原始素材包未改动。
