# 5.7.9 默认皮肤：DLL 与外置 SKN 的加载差异

## 本次结论

`Skin/DEFAULT_SKIN_579.skn` 是 5.7.9 `ttpres.dll` 的
`ZIP / <DEFAULT_SKIN>` 资源的完整副本，不是图片解压或 BMP 透明色损坏。
本地源目录及 `build/Release` 中的两份 ZIP 均为 130105 字节，SHA-256 为：

```text
4A3E33B771D6B98E4D8A63AE69C4720D1F0A169055FCF716C4A846CC5773A563
```

两条路径最终都调用 `SkinPackage::ExtractTo` 和 `LegacySkin::Load`。
回归测试逐项比较了 ZIP 内的 75 个文件，解压内容完全一致。

但是，它们的用户配置身份不同：

- DLL 默认皮肤：`Skin/Default.xml`。
- 外置副本：`Skin/DEFAULT_SKIN_579.skn.xml`。

本次检查时，Release 的 `Default.xml` 已保存歌词背景 `#18333c`、
播放列表背景 `#1d3840` 等自定义值；外置副本尚无自己的配置。
皮肤包里的 `Lyric.xml`、`Playlist.xml` 默认背景则分别是
`#31475b`、`#4b6782`。因此已有配置不同，本身就会造成外观差异。

按用户最后的选择，本次**不复制、不覆盖、不合并用户配置**，也不因 ZIP
相同就共用 `Default.xml`。已有的个性化差异仍然保留。

## 原版证据与代码修复

仅以 5.7.9 伪代码为本次配置加载行为的依据：

1. `CSkinManager_LoadPackageXml`（004A747D）：默认皮肤标识只改变 ZIP
   来源，之后仍加载包内的 `Skin.xml`、`Visual.xml`、`Lyric.xml`、`Playlist.xml`。
2. `FUN_0045D5FA`：先保存旧皮肤配置，解析并绑定目标包
   （0045DDEE；播放列表绑定 0047E6FC），然后读取目标的独立配置。
3. `CSettings_SerializeXml`（004B605A）在现有设置对象上逐字段读取。
   `FUN_0048DDB2`、`FUN_0048DE50` 遇到缺失或无效颜色、字体字段时，
   不用一个新建设置对象的默认值替换已有字段。

重建版原来有两处遗漏：

- 只在目标配置文件不存在时才应用包内歌词、播放列表的默认设置。
  配置存在但不完整时，歌词可能残留上一款皮肤的配色。
- `LoadSkinVisualProfile` 使用新建的 `LoadLegacyXml` 设置对象再复制
  播放列表视觉字段，遗漏的字段因此变成通用默认值，而非目标皮肤的值。

现在运行时切换先应用包内视觉设置，再按配置中实际存在的字段覆盖。
配置解析在临时副本上完成后统一提交，保留未指定的颜色、字体、位置及
可见性，并区分“未指定矩形”和显式的 `0,0,0,0`。全局滚动模式、播放
行为和 Visual 的 Type/FramesPerSec 不受皮肤配置覆盖。
有效皮肤的正常启动路径仍保留 TTPlayerRebuild.xml 中的视觉全局设置。
已保存的皮肤缺失或损坏时则属于皮肤替换，不能继续沿用旧样式，详见
[启动回退修复](SKIN_STARTUP_FALLBACK.md)。

## 验证

新增 `skin_profile_tests`，在宿主机的独立临时目录运行，不读取或写入用户
实际的皮肤配置。测试使用生成的配置验证：

- 内置资源与外置文件的 75 项内容一致。
- 相同配置下，播放列表、歌词窗口背景/标题、歌词控件绘制像素一致；
  切回后结果仍一致。
- 配置不存在、为空、损坏及部分字段覆盖时使用正确的默认值。
- 保留全局行为、显式空矩形；拒绝非配置根节点。
- 两个皮肤独立保存设置，切换不重新创建主窗口、歌词及播放列表窗口。

修复前分别复现“缺失字段替换皮肤基准值”和“未先应用目标包默认值”的
失败；修复后通过。Release 构建及 skin_profile_tests、skin_alignment_tests、
project_links_tests、skin_png_tests、skin_rebind_tests、ttplayer_tests 均通过。
本次没有运行原版进程进行新的截图对照；原版依据来自上述伪代码，
像素对照比较的是重建版的两种资源来源，不代表全部原版功能的等价验证。
