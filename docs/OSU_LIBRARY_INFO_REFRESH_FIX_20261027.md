# osu!lazer 信息回读丢失谱面信息修复

后续更新：[专辑封面接入及两种星环独立优先级](OSU_ALBUM_ARTWORK_PRIORITY_20261027.md)。最新发行文件和摘要以该文档为准。

## 1. 问题与原因

用户报告专用列表中部分歌曲显示哈希文件名，播放时提示把无扩展名文件改成 `.mp3`，并且缺少对应 `.osu` 的壁纸。

初始导入以及播放完成时已有谱面信息保护，但异步信息队列有一条遗漏路径：

1. `ReadPlaylistInfoIsolated` 只接收音频路径和子曲目，生成新的 `PlaylistInfoResult`；此结果没有游戏来源信息。
2. `ApplyPlaylistInfoResult` 收到结果后，原来只保留行的路径与评分，直接 `SetTrack` 替换整条记录。
3. 替换因此丢失 `TTPlayer.Osu.Source/Identity/Title/Artist/Album/Beatmap/Background`。列表改用音频标签或文件名；后缀检查失去游戏来源保护；视觉模块也收不到背景及谱面路径。

lazer 音频以哈希为文件名、没有扩展名，是游戏存储格式，并非需要修复的文件名。DLL 对数据库与文件映射的解析无需改变。

## 2. 修复

- 异步信息回填逐行保留原行的 osu 来源、谱面标签、身份、背景和节拍文件，再恢复标准标题、艺术家及专辑标签。音频时长、编码、采样率等继续采用后台探测结果。
- 同一个哈希被多个谱面集引用时，分别使用各行原有的身份与背景，不把首行信息复制给其它行。
- 同时保留已有音乐服务身份 `TTPlayer.Tid`，与其它信息回读路径一致。
- 后缀更改的入队处再检查当前游戏曲目，避免绕过播放完成时的保护。普通歌曲的后缀提示设置与识别逻辑不变。
- 壁纸仍遵循“音频内嵌封面 → 谱面背景 → 全屏备用图片”的优先顺序。修复的是背景路径丢失；不需要更换 `ttp_osu.dll`。

## 3. 验证

修复前，新增集成用例在真实后台任务完成后失败，报 `background information replacement lost beatmap metadata`；修复后通过。

此前窗口用例未连接 `PluginManager`，没有真正触发异步音频信息补读。本次私有测试接入该依赖及真实隔离子进程入口，并明确等待后台回填完成，避免只验证刚导入时的正确状态。

Windows 11 本机验证：

- stable 和 lazer 导入、后台读取、重载后的标题/艺术家/专辑、来源身份、背景与节拍文件保留；包含不同谱面集共用音频哈希。
- 回读后真实 WaveOut 播放；游戏曲目无后缀弹窗，直接调用更改队列也被阻止；全局提示设置保持启用。
- 可视化数据发布保留对应谱面/背景，哈希文件名的 PNG 可以解码。
- 当前本机实际 lazer 库有 2 首 MP3：`triangles` 与 `new beginnings`。后台读取后仍使用谱面标题，时长分别为 126066 ms、131112 ms；数据库字节未改变。
- 实际 `new beginnings` 的背景映射和无后缀图片解码通过；`triangles` 的 `.osu` 没有背景声明，应使用既有备用路径。
- 两种星环、两种全屏布局的实际 DLL 像素测试通过，包括封面/谱面背景/备用图优先级、PNG、坏图、清空、替换及取消。

XP、Win7 虚拟机本轮查询均未运行，未重复实机测试；最终 EXE 的 XP/Win7 静态导入审计通过（23 个 DLL、751 项导入）。Windows 10 未实机测试。

测试代码和日志仅在私有 `tests` 中，不加入 Action 或发行包：

- `tests/ui/osu_library_lists_tests.inc`
- `tests/osu_library_20261026/info-refresh-before.log`
- `tests/osu_library_20261026/info-refresh-after.log`
- `tests/osu_library_20261026/info-refresh-wallpaper.log`
- `tests/osu_library_20261027/release-verification.json`

## 4. 交付

保持本地现有构建版本 `2026.10.26`，更新 `build/Release/TTPlayerRebuild.exe`、对应 ZIP 及 `SHA256SUMS.txt`。ZIP 与 Release 文件逐字节比对、包内摘要检查通过，不包含私有测试或可选 osu DLL。

| 文件 | 字节数 | SHA256 |
| --- | ---: | --- |
| `TTPlayerRebuild.exe` | 4439552 | `bfa2cdf9ec35c84647a7b9be8b1ab0c9bee9ec15557876de6d8acbb83df7b219` |
| `TTPlayerRebuild-2026.10.26.zip` | 3440181 | `7c11d54b4570ffc4ca72e44b57fe43052e177413617f94ee32d4999f8b1ad55a` |

更新 EXE 后重启播放器，或重新勾选专用列表，重新从游戏索引载入谱面信息。此次没有修改游戏文件，没有提交或推送 Git。
