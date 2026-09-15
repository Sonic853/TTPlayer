# 无配置首次启动的默认皮肤配色

## 问题与复现

`TTPlayerRebuild 2026.09.15.7z` 包含皮肤和运行 DLL，但没有 `TTPlayerRebuild.xml`、
`TTPlayer.xml`、`Skin/Default.xml`。包中 EXE 与修复前本地现代 Release 相同，SHA-256：
`6386156dd65f80bca66decfa1f6ebac8691a017d826e15b1739e6f7368ea11e3`。

在隔离目录运行包内修复前重建版 EXE 的 `--smoke-test` 后，主配置保存了重建版通用的播放列表配色，
与内置默认皮肤（`ttpres.dll` 的 `ZIP/<DEFAULT_SKIN>`）里的 `Playlist.xml` 不同：

| 属性 | 错误首次启动值 | 默认皮肤值 |
| --- | --- | --- |
| `Color_Bkgnd` | `#3b3e43` | `#4b6782` |
| `Color_Bkgnd2` | `#2c2f33` | `#405b76` |
| `Color_Text` / `Color_Number` / `Color_Duration` | `#b4b4b4` | `#8bbac6` |
| `Color_Hilight` | `#1e1e1e` | `#ffffff` |
| `Color_Select` | `#ffffff` | `#88aacb` |

`LoadStartupSkin` 只在“指定皮肤加载失败、回退到默认皮肤”时应用包内字体和配色。
正常启动成功加载皮肤时，窗口还未创建，`LoadSkin` 中的运行时切换分支也不会执行。
没有主配置和皮肤配置的发行包因此保留了 `Settings{}` 的灰色列表，而未读取包内默认值。
关闭时还会把这些错误值保存成用户配置，使后续启动继续沿用。

这里的“通用配色”特指修复前重建版 `Settings{}` 的灰色值。原版 5.7.9 的通用初始化值是
另一套深青蓝色，详见[原版默认配色分析](ORIGINAL_DEFAULT_PALETTE.md)。
后续按用户要求，通用初值也已改为[读取 DLL 默认皮肤的配色](DEFAULT_COLOR_INITIALIZATION.md)。

## 修复后的顺序

正常启动在创建窗口、字体和画刷之前完成：

1. 加载并验证实际使用的皮肤包。
2. 读取包内 `Playlist.xml`、`Lyric.xml` 的字体和全部配色。
3. 按主配置中实际存在且有效的字段叠加自定义字体和配色。
4. 再叠加当前皮肤的独立配置：默认皮肤为 `Skin/Default.xml`，外置包为对应 `.skn.xml`。

主配置缺失、损坏、不含样式或有无效样式字段时，保留皮肤包基准。
明确保存的自定义颜色仍有效，即使该值恰好等于之前通用灰色主题的值。
旧配置导入和只读目录下的内存导入也保留明确的自定义样式。

皮肤加载失败的回退仍忽略旧皮肤主配置中的样式，只使用默认包和 `Default.xml`。
普通启动的 Visual 全局设置、播放行为和窗口状态保持原有恢复流程。
启动预加载不写入任何配置文件。

## 测试

- 新回归在修复前失败：`normal startup did not layer package / explicit main / explicit skin colors`。
- 默认皮肤、外置 LX-iPlay、Skin/new PNG 皮肤 × 6 种主配置来源/状态 × 4 种皮肤配置，
  共 72 组普通启动测试，检查所有列表配色、字体、歌词配色、覆盖优先级、全局选项及不写配置。
- 实际 EXE 在无配置的新目录首次启动、关闭、再启动，核对主配置与 `Default.xml`
  保存的配色均与内置包一致；现代版和 XP/Win7 版均执行该宿主测试。
- 原有 16 组皮肤缺失/损坏回退、3 种正常启动 Visual 保留场景、配置迁移、运行时切换和
  默认/外置同内容皮肤的独立配置测试继续通过。

## 给测试者

优先将修复版完整测试包解压到**新的目录**运行，验证没有历史配置的首次启动。

已经运行过旧包的目录可能在 `TTPlayerRebuild.xml` 和 `Skin/Default.xml` 中保存了错误配色。
这些值与用户主动选择的颜色无法可靠区分，因此更新 EXE 不会自动删除已有配置。
如需仅重置默认皮肤的列表配色，关闭播放器并备份这两个文件后，删除其中 `PlayList` 节点的
`Color_Bkgnd`、`Color_Bkgnd2`、`Color_Text`、`Color_Number`、`Color_Duration`、
`Color_Hilight`、`Color_Select` 属性，保留节点和其它属性，再启动修复版。
请勿把某个用户的配置文件打包为所有测试者的默认配置。

此次配色测试在当前 Windows 主机运行；XP/Win7 版另经旧系统静态导入检查，
未新增 XP/Win7 实机验证。修复前后产物及回归日志在 `out/skin-startup-20260915`。
