# 原版 5.7.9：通用初始化配色与内置默认皮肤配色

## 结论与证据范围

结合用户补充的历史背景，原版有两套应分别描述的默认值：

- 旧版默认皮肤配色，沿用为程序通用初始化：深青蓝背景、青蓝文字、浅冰蓝高亮。
- 内置皮肤 `TT.experience`：较亮的蓝灰背景、浅青文字、白色高亮。

旧配色的历史来源由用户补充；本地 5.7.9 原 EXE 则直接证明这些常量仍保留在初始化中。
所以它们本身也是默认皮肤配色，不能因其与 TT.experience 的包内值不同就判定为错误或用户改色。

此次分析时，修复前重建版 `PlaylistSettings{}` / `PlaylistSkin{}` 中的灰色值是第三套值，
与上述两套原版值都不同。此前发行包首次启动漏应用皮肤样式，暴露了这套重建版值。

本次使用本地 `TTPlayer.exe` 5.7.9.0、其反编译结果和原始 `ttpres.dll` 默认 ZIP 副本。
初始化函数的 16 个颜色字段已与原 EXE 机器指令核对；默认 ZIP 共 75 项，SHA-256：

```text
4A3E33B771D6B98E4D8A63AE69C4720D1F0A169055FCF716C4A846CC5773A563
```

本次未启动原版做新的截图测试。下文分别给出静态初始化值、皮肤包定义和绘制规则；
已有配置、当前皮肤、控件焦点和系统颜色会影响实际画面。

本地证据与可视化：

- [配色示意与原始背景位图](../out/default-palette-analysis/palette.html)
- [原 EXE 颜色赋值指令及 SHA-256](../out/default-palette-analysis/original-initializer-colors.json)
- [核对脚本](../out/default-palette-analysis/analyze.py)

## 1. 播放列表：三套值并排比较

| 属性 | 原版通用初始化 | 原版 TT.experience 包内值 | 修复前重建版通用值 |
| --- | --- | --- | --- |
| `Color_Bkgnd` | `#1D3840` | `#4B6782` | `#3B3E43` |
| `Color_Bkgnd2` | `#18333C` | `#405B76` | `#2C2F33` |
| `Color_Text` | `#548EA5` | `#8BBAC6` | `#B4B4B4` |
| `Color_Number` | `#45859E` | `#8BBAC6` | `#B4B4B4` |
| `Color_Duration` | `#45859E` | `#8BBAC6` | `#B4B4B4` |
| `Color_Hilight` | `#D4F5FF` | `#FFFFFF` | `#1E1E1E` |
| `Color_Select` | `#84CEF9` | `#88AACB` | `#FFFFFF` |

表中的“原版通用初始化”对应上述历史旧默认皮肤方案。
原版初始化证据是 `FUN_00401E96`，播放列表位于选项掩码 `0x40` 分支。
[赋值代码](../../reverse/decompiled/TTPlayer.exe.pseudo.c#L1519)与
[`CSettings_SerializeXml` 的 XML 属性映射](../../reverse/decompiled/TTPlayer.exe.pseudo.c#L176858)
可相互校验。两者的字段偏移一致：

| 对象偏移 | 原版 COLORREF 整数 | 对应 RGB |
| --- | --- | --- |
| `+0x5FC` | `0x00A58E54` | `#548EA5` |
| `+0x600` | `0x00FFF5D4` | `#D4F5FF` |
| `+0x604` / `+0x608` | `0x009E8545` | `#45859E` |
| `+0x60C` | `0x00F9CE84` | `#84CEF9` |
| `+0x610` | `0x0040381D` | `#1D3840` |
| `+0x614` | `0x003C3318` | `#18333C` |

这里必须转换字节顺序：COLORREF 的整数是 `0x00BBGGRR`，XML 是 `#RRGGBB`。
例如 `0x0040381D` 对应 `#1D3840`，不能直接写成 `#40381D`。

内置皮肤七个值来自原始 [Playlist.xml](../out/default-palette-analysis/skin/Playlist.xml)。
通用方案中，序号和时长比正文略暗；TT.experience 则把三者统一为 `#8BBAC6`。

### 列表状态如何使用颜色

原版歌曲列表绘制 `FUN_00487C0D`，对应
[颜色和状态分支](../../reverse/decompiled/TTPlayer.exe.pseudo.c#L130121)：

1. 普通行：正文用 `Color_Text`，序号用 `Color_Number`，时长用 `Color_Duration`。
2. 未选中时：零基偶数行用 `Color_Bkgnd`，零基奇数行用 `Color_Bkgnd2`。
   用户看到的第 1 行为第一背景，第 2 行为第二背景。
3. 正在播放且未选中：正文和序号改用 `Color_Hilight`，时长仍用 `Color_Duration`。
   播放高亮本身不把整行背景换成高亮色。
4. 选中行：若皮肤确实加载了 `selected_image` 则绘制图片；否则以 `Color_Select`
   向 `Color_Bkgnd` 绘制从上向下的渐变。不是整行纯色填充，也不是向第二背景渐变。
5. 选中后的三类文字统一使用系统 `GetSysColor(COLOR_HIGHLIGHTTEXT)`。
   如果该值恰好等于 `Color_Select`，则改用 `Color_Hilight`。因此选中文字通常为白色，
   但其首选来源是系统设置，不能把白色写成适用于所有系统主题的皮肤常量。
6. 控件失去焦点且没有 `LVS_SHOWSELALWAYS` 时，会暂时隐藏选中和焦点的绘制状态。
   原版左侧播放列表名称控件 `FUN_004897C1` 也有对应的交替背景、选中渐变和文字规则。

渐变函数 `FUN_0045032C` 调用 `FUN_00408852` 按通道做整数插值。
高度为 H 时，纵坐标 y 的通道值相当于 `(起点 * (H-y) + 终点 * y) / H` 的整数结果，
其中 y 为 0 到 H-1；最后一行趋近终点，而不必恰好等于终点。

`Skin.xml` 虽写了 `selected_image="playlist_selected.bmp"`，但原始默认 ZIP 的
75 项中没有这个文件，因此该默认包使用程序的渐变分支。

## 2. 窗口歌词与可视化

| 区域 / 属性 | 原版通用初始化 | 原版 TT.experience 包内值 |
| --- | --- | --- |
| 歌词 `TextColor` | `#548EA5` | `#8BBAC6` |
| 歌词 `HilightColor` | `#D4F5FF` | `#FFFFFF` |
| 歌词 `BkgndColor` | `#18333C` | `#31475B` |
| 频谱顶部 / 中部 / 底部 / 峰值 | 均为 `#194D5C` | 均为 `#27435F` |
| `BlurScopeColor` | `#194D5C` | `#27435F` |
| 可视化 `TextColor` | `#FFFFFF` | `#27435F` |

初始化分别见 `FUN_00401E96` 的歌词 `0x30` 与可视化 `0x01` 分支；
包内定义见 [Lyric.xml](../out/default-palette-analysis/skin/Lyric.xml) 和
[Visual.xml](../out/default-palette-analysis/skin/Visual.xml)。

TT.experience 的频谱四个高度相关颜色相同，XML 没有定义红黄绿等多色频谱。
包内另指定 `SpectrumWide=1`、`Blur=1`、`BlurSpeed=3`，未指定 Type 和 FPS。
这些颜色仍需结合可视化模式与背景来解释，不能把它们当作整个可视化区域的背景色。

上述 Lyric 是普通窗口歌词。原版另有全屏歌词和桌面歌词设置，不能把普通窗口三色
套用于这两个模式。例如初始化中的全屏歌词是正文 `#0080C0`、高亮 `#00FF00`、
背景 `#000000`；它们对应另一组字段。

默认包的 Playlist.xml、Lyric.xml 均未指定 Font。原版初始化通过
`SystemParametersInfoW` 读取系统字体信息，再供相关区域继承；不能将重建版
硬编码的 `SimSun/-13` 说成该皮肤 XML 指定的字体。

## 3. 窗口外框和控制区由位图定义

七个列表颜色只负责列表内容，不包括所有边框、标题条、按钮和滚动条。
[Skin.xml](../out/default-palette-analysis/skin/Skin.xml) 同时定义了：

- 主窗口与迷你窗口信息文字：`#FFFFFF`，Tahoma，font_size=13。
- 主窗口声道 / 状态文字：`#8BBAC6`，Tahoma，font_size=13。
- 浏览器窗口底部文字：`#5C7C9C`，SimSun，font_size=12。
- 全局及部分组件的色键透明色：`#FF00FF`。这表示透明像素，不是正常显示的强调色。
- LED 时间数字使用 `number.bmp`，并非直接按上面的文字色绘制。

原始背景位图中可直接数出的常见色示例：

| 位图 | 尺寸 | 代表性像素颜色 |
| --- | --- | --- |
| `player_skin.bmp` | 327 × 141 | `#3B5269`、`#415B74`、浅色边框 |
| `playlist_skin.bmp` | 327 × 124 | `#4B6782`（20,120 像素）、`#3B5269` |
| `lyric_skin.bmp` | 327 × 122 | `#31475B`（25,301 像素）、浅色边框 |
| `eq_skin.bmp` | 327 × 99 | `#49657F`、`#39536E`、浅色边框 |

这进一步说明 TT.experience 的列表和歌词包内背景与原始位图是配套的。
文字抗锯齿、选中渐变、图像自身的渐变会产生其它中间像素色，实际截图不只含表中的色值。

## 4. “默认值”与实际配置的关系

`FUN_00402D9E` 先调用通用初始化，再读主配置。皮肤包样式对象的构造函数
`FUN_0048E635`（列表）和 `FUN_0048EA53`（歌词）以当前全局设置为基准，
然后解析包内字段；它们不是每次都从固定的初始常量重新开始。

绑定目标皮肤时是否复制包内样式，由 `DAT_00547740` 等状态控制。
`FUN_0047E6FC` 在相应分支复制全部七个列表颜色；运行时切换皮肤的
`FUN_0045D5FA` 再处理目标皮肤旁的配置。由此不能仅凭截图颜色判断其来源。

多数颜色通过 `FUN_0048DDB2` 逐字段覆盖；缺失或无法解析的字段保留已有值。
有一个需要单独说明的例外：包内 `Playlist.xml` 的 `Color_Select` 用
`FUN_0048DD6C(属性, GetSysColor(COLOR_HIGHLIGHT))` 解析，缺失或无效时取系统选择背景色。
这是包内列表样式解析器的规则；主配置的同名字段仍使用保留现值的 `FUN_0048DDB2`。
整个 PlayList 节点不存在与节点存在但 Color_Select 属性缺失也不是同一种情况。

此前报告将 `Skin/Default.xml` 中 `#1D3840`、`#18333C` 表述为“自定义值”，
容易让人误以为已证明用户主动改色。本次确认它们与原版通用初始化常量完全一致；
准确表述应为“已保存的覆盖值，来源不能仅凭颜色确定”。

## 5. 对重建修复的含义

上一轮已修复首次启动遗漏应用实际皮肤包颜色的路径。
本次进一步确认，分析时重建版的通用灰色初值也没有与原版 `FUN_00401E96` 对齐。
这是两个独立层面：启动正确读取完整皮肤后，仍可能在缺省字段或其它兜底路径暴露
通用初值的差异。若继续追求原版行为一致，应同步核对设置对象与皮肤布局对象的
兜底值、缺失字段继承规则及 Color_Select 的特殊处理。

随后用户要求根据本分析调整，并明确将通用初始化改为读取 DLL 默认皮肤配色。
现已接入共享的资源配色基准、当前设置继承及 Color_Select 特殊解析规则；
详见[从 DLL 初始化通用配色](DEFAULT_COLOR_INITIALIZATION.md)。已有用户配置仍保留。
