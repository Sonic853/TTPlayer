# osu! 专用列表在 Release 配置下显示文件名的问题

## 原因

本机 Release 的 `TTPlayerRebuild.xml` 中保存的是：

```xml
<PlayList TagFormat="1" TagTitleFormat="" DefTitleFormat="" />
```

DLL 已正确读取 stable 的 10 首和 lazer 的 2 首歌曲信息。例如第一首 stable 为 `nekodex - new beginnings`，lazer 第一首为 `cYsmix - triangles`。后台信息回读也保留了谱面元数据。

但是统一显示函数 `FormatPlaylistTitle` 依次尝试标签格式、默认格式，两者为空时按照普通歌曲的原版行为回退到 `%F`。因此 stable 的 `audio.mp3` 显示为 `audio`，lazer 的无扩展名文件显示为哈希。这是显示阶段的问题，独立于此前已修复的后台回读覆盖元数据问题。此前曲库测试采用默认 `%A - %T`，没有覆盖实际 Release XML 中两个格式为空的情况。

## 修复规则

在 `src/ui/player_window_playlist.cpp` 的统一显示函数中增加专用曲目的回退：

1. 启用且有效的自定义标签格式仍然优先。
2. 对带有效 osu 来源标记的曲目，在标签格式为空、无效或关闭时，优先显示谱面的 `艺术家 - 标题`。缺少其中一个字段则只显示已知字段。
3. 普通歌曲维持原版的默认格式、文件名回退规则。

默认格式中的 `%F` 不再覆盖这个专用曲目回退；若用户明确把有效标签格式设为 `%F`，仍尊重该显式选择。

列表绘制、皮肤插件曲目回调、搜索/排序使用的显示文本都经过同一函数。无需修改游戏文件、用户 XML 或 `ttp_osu.dll`。

## 验证

Windows 11 本机完成：

- 直接使用 Release 的现有配置及实际 stable/lazer 曲库；修改前稳定复现第一首显示 `audio`，同时日志证明 DLL 返回了正确标题。
- 修改后 10 首 stable、2 首 lazer 的异步导入、后台音频信息重读、标题/艺术家、时长和皮肤插件回调均通过。
- 已查看默认皮肤的实际窗口截图，两类列表均正确显示曲名。
- 空格式、非法格式、不存在字段、关闭标签格式、有效自定义格式、显式 `%F`、缺少艺术家/标题，以及普通歌曲的回退行为通过。
- 371 项游戏文件、Realm 数据库及 Release 用户配置的前后 SHA256 一致。
- Release 编译、XP/Win7 静态导入检查、发行 ZIP 内容与哈希校验通过。本轮未进行 XP/Win7 虚拟机实测。

私有测试及证据保存在 `tests/ui/osu_library_display_tests.inc` 和 `tests/osu_display_20261027`，不上传、不加入 Action 或发行包。

## 输出

已更新 `build/Release/TTPlayerRebuild.exe`、`TTPlayerRebuild-2026.10.27.zip` 和 `SHA256SUMS.txt`。替换主程序并重新启动即可，无需重设标题格式或重装插件。

| 文件 | 字节数 | SHA256 |
| --- | ---: | --- |
| `TTPlayerRebuild.exe` | 4447744 | `2ddf8703a1f07859822fb8afb28d5ff428bc0672126a68d35aafe9e00d9e67b7` |
| `TTPlayerRebuild-2026.10.27.zip` | 3443333 | `1e9599052b5710c78db8dcee517f80224800cd37062be9afaf22fe3bc832427a` |
