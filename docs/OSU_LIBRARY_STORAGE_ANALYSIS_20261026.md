# osu! 媒体库路径与 lazer 文件存储分析（2026-10-26）

## 1. 结论与本轮范围

`%USERPROFILE%\AppData\Roaming\osu\files` 是 osu!lazer 的**按文件内容 SHA-256 寻址的共享文件仓库**。它不是 osu!stable 的 `Songs` 目录结构，不能直接按 `.mp3`、`.ogg` 等扩展名递归扫描，就认为完成了歌曲库导入。

歌曲名称、音频文件、背景图及谱面之间的对应关系保存在数据根目录的 `client.realm` 中。可靠的后续导入需要同时定位 **数据根、client.realm、files**，从谱面元数据查找实际音频和背景。

本轮实现范围为媒体库设置页及四个路径的保存。这里提供存储分析与后续读取方案；**尚未实现或验证 lazer 数据库歌曲导入**。本次没有启动游戏、打开或修改用户数据库，也没有读取账号令牌。

## 2. 对象身份与证据边界

用户指定对象：

- 安装版：`C:\Users\Sonic853\AppData\Local\osulazer\current`。
- 源码：`D:\Git\OSU\osu`，提交 `efac2716be00cf8ab1ffefecab58c46c65715885`，本次检查时工作区干净。
- 指定默认数据目录：`C:\Users\Sonic853\AppData\Roaming\osu`。

### 2.1 实际文件身份

| 文件 | 字节数 | SHA-256 |
| --- | ---: | --- |
| `osu!.exe` | 256288 | `44f341bec1d2b786265c321a60d708b476e491286e611d2d19f19f86d1c68933` |
| `osu!.dll` | 251168 | `bb263b8bfad9ed54b524119e5ff9949bd14d02b51901d43b5496e76055afdb98` |
| `osu.Game.dll` | 6907168 | `1d33937f2ddc1045f525342ce1243397589d136ed5be6691093cb91bf64959b7` |
| `osu.Framework.dll` | 3963680 | `36302a02937421361f186d85d6fe608e52799ef8bedac0d8fadbfdba68492ae3` |
| `Realm.dll` | 378144 | `a9fb9a527e69901ef41743be22134710b306608f81fdff1d155c73ca20481ba0` |
| `realm-wrappers.dll` | 3895584 | `3d7740f3572e2433bdac83331605380339f5f32f796621cb5a63d325466c1e8c` |

`osu.Game.dll` 的程序集版本为 `2026.1005.0.0`，MVID 为 `f70ba22c-d873-4d4d-87f8-73592f4d211c`；其声明引用 `Realm, Version=20.1.0.0`。这些是当前本机安装文件的事实，不代表所有 lazer 版本都使用相同数据库格式。

### 2.2 REA 与补充静态证据

本轮 REA MCP 工具未暴露。按技能恢复流程执行 `rea-agents@6.5.0 doctor --client codex --json`：运行时可用，Codex 注册缺失。没有修改客户端配置；使用同版本 REA CLI 完成可用的静态检查。

| 标记 | 证据 | 结果与限制 |
| --- | --- | --- |
| R1 | `ev_dfaa738d65f116fcdf9ef07110a48e55a81f1b38a77d3b5c4a16ffd08c4a366a` | 本轮 `inspect-managed-artifact` 检查实际 `osu.Game.dll`；身份及元数据覆盖 complete，未执行程序集。 |
| R2 | `ev_370b6d2fa561d17afb82994d4d46e815bd70ce60606e851762eb6d334e78633e` | 实际 `osu.Framework.dll` 的 `DesktopGameHost`、`WindowsGameHost` 成员及 CIL 调用锚点；覆盖 complete，返回 30 个方法。请求中的 `WindowsStorage` 类型不存在，实际存储实现为 `DesktopStorage`，不把这一未匹配项当作已验证类型。 |
| R3 | `ev_0f1dac74c29af12e656ccb598e1606cfd91416f8b5691d55f756c65ea9c947bc` | `osu.Game.dll` 定向成员检查遇到 REA 67108864 字节表示预算限制，coverage=unavailable。此记录只证明限制，不能证明存储方法行为。 |
| R4 | `ev_c18b17f86b4b2da0721ca3f1af07af63823459d2ea9413f31388f15f169e0937` | 复用前一轮安装 EXE 的 REA 记录；本轮重新计算 SHA-256 与其一致。EXE 无 CLI 目录，游戏逻辑位于托管 DLL，不能只反编译 EXE 就得出文件存储结论。 |
| D1 | 本机已有 `dnSpy.Console.exe` 对安装版 DLL 的定向反编译 | 补足 R3 的方法行为证据；保留 metadata token、RVA、文件偏移及输出摘要。它是**补充反编译证据，不是 REA 成员分析成功记录**。 |
| S1 | 上述固定提交的相关 C# 源码快照 | 与 D1 的路径计算、索引映射、配置重定位进行逐项交叉核对。源码不是整个安装版的逐字还原证明。 |
| F1 | 指定目录的有限目录元数据检查 | `files` 与 `client.realm` 存在，Realm 文件 65536 字节；默认根无 `storage.ini`，安装目录无 `framework.ini`。只采集一个分片路径样本，未按内容识别其用途。 |

原始记录、定向反编译、源码快照和 SHA 清单仅保存在私有 `rebuild/tests/osu_paths_20261026/`。`storage-evidence-ledger.json` 将每份记录与文件身份关联；测试证据不加入 Action 或发布包。

## 3. 安装目录和数据根是两个独立位置

### 3.1 默认根的计算

实际安装版框架中的 `WindowsGameHost.get_UserStoragePaths`（token `0x06000661`）使用 `Environment.SpecialFolder.ApplicationData`，即 Windows Roaming 应用数据目录。`GameHost.GetDefaultGameStorage`（`0x060004EA`）在该目录下使用游戏名创建/查找子目录。源码 `osu.Desktop/Program.cs` 的正式版游戏名为 `osu`。

因此普通 Windows 安装下默认结构为：

```text
%APPDATA%\osu\                  数据根
    client.realm               本地谱面、文件映射等 Realm 索引
    files\                     共享内容仓库
    exports\                   用户导出文件

%LOCALAPPDATA%\osulazer\current\osu!.exe   可执行程序
```

用户给出的 `%USERPROFILE%\AppData\Roaming\osu\files` 与本机默认布局一致。对重定向用户目录，Windows 的 `%APPDATA%`/已知文件夹 API 才是系统给出的 Roaming 位置；设置中的字面默认路径与将来自动探测的实际路径应区分。不要由 `current\osu!.exe` 的父目录直接推导歌曲目录。（R2、D1、S1）

### 3.2 两种覆盖默认根的方式

1. **便携安装。** `DesktopGameHost.GetDefaultGameStorage`（`0x06000461`）检查 `IsPortableInstallation` 或启动目录的 `framework.ini`；成立时以启动目录为默认存储根。
2. **自定义数据位置。** `OsuStorage` 构造函数（`0x0600A565`）在默认根读取 `StorageConfigManager`；其文件名为 `storage.ini`（`0x0600BC8C`），键 `FullPath` 默认空。非空时，`TryChangeToCustomStorage`（`0x0600A569`）通过 `host.GetStorage(CustomStoragePath)` 改用该根。访问失败会恢复先前存储；空目录另有 `AccessibleButEmpty` 状态，不能把“能读取字符串”视为新数据根有效。

`ChangeDataPath`（`0x0600A568`）只更新配置，源码明确说明它本身不搬迁文件；迁移是另一套操作。播放器只需只读定位，无需调用它。

本机 F1 没有发现上述两个配置文件，只能说明此次检查时默认布局可用；不证明用户将来不会迁移，也不构成读取当前数据库内容的验证。

## 4. files 中的文件如何定位

`RealmFileStore` 构造函数（`0x0600BB83`）执行 `storage.GetStorageForDirectory("files")`。`Add`（`0x0600BB84`）对输入流计算 SHA-256，以 `RealmFile.Hash` 查询已有文件，只有需要时才复制到仓库。

`ModelExtensions.GetStoragePath`（`0x0600B8A5`，RVA `0x0022E145`）与源码 `osu.Game/Extensions/ModelExtensions.cs:23` 均给出：

```text
相对路径 = hash[0:1] / hash[0:2] / hash
完整路径 = 数据根 / files / 相对路径
```

例如哈希以 `abc...` 开头的文件放在 `files\a\ab\abc...`，不是 `files\ab\cd\...`。末级名称不携带 `.mp3`、`.jpg`、`.osu` 等原始扩展名。音频、背景、谱面、皮肤资源等可进入同一仓库；相同字节内容可以复用同一物理文件。（D1、S1；SHA-256 定义见 `IO/IFileInfo.cs`）

F1 的一个有限路径样本符合上述一字符/两字符分片和 64 字符文件名规则。本轮没有读取其内容、校验其 SHA 或推断其媒体类型，不能由这个样本声称检查了整个用户库。

## 5. 曲目、音频和背景如何关联

### 5.1 索引结构

源码 `OsuGameBase.cs:91,283` 将 `client.realm` 交给 `RealmAccess`。当前安装版 `RealmAccess.getConfiguration` 使用 `SchemaVersion=52`，并设置迁移回调。`client.realm` 是 Realm 数据库，不能当 SQLite 打开；旁边的 `online.db` 不能代替这个本地文件映射索引。（R1 的 Realm 引用、D1、S1）

相关模型为：

| 模型 | 与歌曲导入有关的内容 |
| --- | --- |
| `BeatmapSetInfo` | `Beatmaps`、`Files`、`DeletePending`、集合 ID；一套谱面可含多种难度和多个音频。 |
| `BeatmapInfo` | 该难度的谱面路径、所属集合及 `Metadata`。 |
| `BeatmapMetadata` | `Title/TitleUnicode`、`Artist/ArtistUnicode`、`AudioFile`、`BackgroundFile` 等。 |
| `RealmNamedFileUsage` | 原始相对文件名 `Filename`，以及指向 `RealmFile` 的 `File`。 |
| `RealmFile` | 以 `Hash` 为主键；映射数据库类型名 `File`。 |

### 5.2 实际播放路径

```text
BeatmapInfo.Metadata.AudioFile（例如 audio.mp3）
    ↓ 在所属 BeatmapSetInfo.Files 中按 Filename 查找
RealmNamedFileUsage.File.Hash
    ↓ GetStoragePath
files/<首字符>/<前两字符>/<完整 SHA-256>
    ↓
音频流
```

背景把第一步换成 `BeatmapInfo.Metadata.BackgroundFile`。原始 `.osu` 文件也可通过 `BeatmapInfo.Path` 和同一文件映射定位。

`BeatmapSetInfoExtensions.GetFile`（`0x0600BF71`）用 `StringComparison.OrdinalIgnoreCase` 比较原始文件名；`GetPathForFile`（`0x0600BF70`）把查到的文件转为哈希分片路径。安装版 `WorkingBeatmapCache.BeatmapManagerWorkingBeatmap.GetBeatmapTrack`（`0x0600C150`）和 `getBackgroundFromStore`（`0x0600C14F`）分别采用这两个元数据字段，验证了映射并非仅靠类名猜测。（D1、S1）

因此：

- 不能默认一套谱面只有一个音频；应遍历每个 `BeatmapInfo` 的元数据，再按实际音频 Hash 去重或按“集合+音频 Hash”分组。
- 不同难度可能共用音频但使用不同背景。播放器需要定义稳定的代表谱面/背景选择规则，不能把哈希目录中第一张图当封面。
- 文件头识别可以帮助解码一个已定位的无扩展名文件，却不能恢复其曲目名、背景关联或区分歌曲与短音效。
- 仅扫描 `.osu` 内容也不足以普遍代替 Realm：其中 `AudioFilename`/背景名是原始文件名，物理仓库已经改成内容哈希；缺少集合内的文件名映射仍会丢失关系。

## 6. 四个设置字段的建议

| 页面字段 | 本轮字面默认值 | 用途 |
| --- | --- | --- |
| osu!stable 路径 | `%USERPROFILE%\AppData\Local\osu!\osu!.exe` | stable 程序位置；不由它强行覆盖用户填写的歌曲目录。 |
| osulazer 路径 | `%USERPROFILE%\AppData\Local\osulazer\current\osu!.exe` | lazer 程序位置；不等于数据根。 |
| osu!stable 歌曲路径 | `%USERPROFILE%\AppData\Local\osu!\Songs` | stable 普通谱面文件目录的默认值。 |
| osulazer 歌曲路径 | 先读 `storage.ini` 的 `FullPath` 并追加 `files`；无有效配置时为 `%USERPROFILE%\AppData\Roaming\osu\files` | 按用户追加要求实现；这里是内容仓库，歌曲索引位于上一级 `client.realm`。 |

在界面空间允许时，最后一项可写为“osulazer 歌曲路径（files）”，减少把它与 `Songs` 等同的误解。本轮不需要增加第五个“数据根”输入框：标准布局中可以由 `files` 的父目录得到数据根。以后实现导入时，内部应显式使用 `{data_root, database_path, files_path}`，而不是只传一个歌曲文件夹字符串。

设置应保留用户写入的变量形式，使用时再展开环境变量。未安装游戏或目标盘暂时离线时仍允许保存路径；输入框保存不应创建游戏目录或初始化 Realm。实际导入时再校验 `files` 与上级 `client.realm` 是否成对有效，并说明无效原因。

本轮已加入初始路径探测：保留播放器已经保存的 `LazerFiles`（包括明确清空的值）；尚未配置时读取系统 Roaming `osu` 下的 `storage.ini`。所选安装目录有 `framework.ini` 时，改读该便携根的 `storage.ini`。配置不存在、不可读、空或格式错误时回退到表中的字面默认路径。`FullPath` 指向数据根，必须追加 `files`；不会把它直接当歌曲路径。

路径配置只读，不检查目标盘是否在线、不创建目录，允许保存暂时离线的移动盘位置。更改播放器中的程序路径不会静默覆盖已经填写的歌曲路径；重新导入旧配置且缺少 `LazerFiles` 属性时，会按照其中的程序路径重新识别便携配置。全部重置重新执行默认探测。

## 7. 后续实现建议

1. **先完成设置与只读路径解析。** 区分程序、stable Songs、lazer files；校验环境变量展开、Unicode、移动盘、空路径及无安装情况。
2. **独立设计 lazer 索引读取。** 选择兼容所测 schema 的 Realm 只读访问或用户导出的明确清单；针对游戏退出后的数据库副本验证。不要直接实例化游戏的 `RealmAccess`：实际反编译中包含更新时间、迁移、恢复、备份和重建数据库等写入行为。
3. **实现映射和去重。** 读取未删除谱面集合，逐难度解析音频/背景，通过 Hash 找实际文件；保留来源集合和代表背景，处理多音频、共享音频、缺失文件和大小写路径。
4. **接入播放器。** 对无扩展名资源按实际内容选择解码器，同时保证不触发“错误后缀改名”或标签写回到游戏仓库。保留仓库只读；需要转换时写到用户另选目录。
5. **补充兼容性验证。** 游戏运行中数据库更新、schema 升级、迁移到其他盘、便携版、大库、重复音频、删除等待项、缺失哈希文件等均需测试。Realm 组件对旧系统的支持不能由播放器自身 XP/Win7 兼容性推导；若采用独立现代索引导出工具，应明确其运行系统边界。

本轮没有把 Realm.dll 或游戏组件作为播放器运行依赖，也没有声称以上后续导入已经可用。

## 8. 问题覆盖与尚待验证项

| 问题 | 状态 | 依据 |
| --- | --- | --- |
| 默认数据目录如何得到 | 已回答 | R2 + D1；Windows Roaming、游戏名 osu。 |
| files 如何分片、文件为何无扩展名 | 已回答 | D1 + S1，F1 有限路径佐证。 |
| 音频与背景如何映射 | 已回答 | D1 + S1 的实际播放/背景方法链。 |
| 是否支持自定义根/便携版 | 已回答 | D1 + S1，当前本机 F1 仅为默认布局。 |
| 能否当普通歌曲目录扫描 | 已回答：不能保证正确歌曲库 | 缺失类型、名称与集合映射，且共享资源混存。 |
| 用户数据库当前含哪些曲目 | 未读取、未回答 | 本轮不需要读取数据库内容。 |
| 新导入器的运行正确性与 XP/Win7 支持 | 尚未实现/验证 | 本轮只增加路径设置和 `storage.ini` 默认探测；静态分析不是运行测试。 |
| REA 是否完整恢复了游戏所有方法 | 否 | R3 明确受表示预算限制；只用 D1/S1 补足本次存储问题。 |

## 9. 设置页实现

- 现有“媒体库”页增加“媒体库 / osu!”两个子选项卡。原媒体库启用、监视文件夹、递归勾选及增删按钮保留原控件与消息处理；缩减原启用分组框中的空白以容纳页签。
- osu! 子页包含四个可编辑路径框及 `storage.ini` / `client.realm` 说明。窗口缩放、页签切换和焦点转移沿用现有选项页的实现方式。
- 新字段单独存放在 XML 的 `OsuLibrary` 节点中：`StableExecutable`、`LazerExecutable`、`StableSongs`、`LazerFiles`，不混入普通媒体库监视目录。
- 缺失 XML 属性采用默认值；显式空值保留，Unicode 和环境变量字面形式往返保存。路径编辑不触发普通媒体库监视线程重建。
- `storage.ini` 按 UTF-8 键值文本解析，兼容 UTF-8 BOM、UTF-16 BOM、空白、注释、重复键（后者优先）、路径中的空格及等号；最多读取 64KiB。空值、相对路径、非法内容及不可读文件回退默认值。不会读取游戏数据库或调用游戏存储迁移方法。

生产修改位于 `settings/settings.h`、`settings/settings.cpp`、`settings/osu_storage_path.h`、`ui/options_library_tabs.h` 和 `ui/player_window_options.cpp`。不修改原版资源 DLL，不增加 Realm 或游戏程序集依赖；本轮没有添加启动游戏、歌曲导入或遍历哈希仓库的功能。

## 10. 验证与发行输出

Windows 11 本机、XP 和 Win7 虚拟机均通过本轮真实选项窗口测试：

- 两个子页的标题、顺序、鼠标/键盘切换和隐藏控件焦点转移；控件边界及鼠标命中，反复切页、布局、重新初始化和关闭重开。
- 四个输入框的默认值、即时设置更新、中文/环境变量/特殊字符 XML 往返保存、显式清空；原媒体库目录和递归设置保持、原启用复选框继续生效。
- 无 `storage.ini`、空值、错误值、相对路径、过大配置、UTF-8/BOM、UTF-16 BOM、中文/空格/等号、重复键、离线盘符、便携配置优先和缺失 XML 属性时的重新探测。

测试发现原媒体库页重复初始化会叠加文件夹条目，已修正为清空条目再填充，并避免重复创建列和初始化期间触发提交。三个系统的截图均人工查看，未发现输入框或说明文字被裁切。测试使用隔离的 `storage.ini` / XML 样本，没有修改用户的 osu! 配置或数据库；这些结果不代表尚未实现的 Realm 歌曲导入测试。

私有测试代码：`tests/ui/osu_library_options_tests.inc`；日志、截图及发行核验：`tests/osu_paths_20261026`。测试不上传、不加入 Action 或发行包。

Release 编译与 XP/Win7 静态导入审计通过（22 个 DLL、747 个导入）。核心包校验、包内文件与 Release 字节一致性、可选插件分包检查通过。沿用当前配置中的构建版本 `2026.10.25`，更新输出至 `build/Release`：

| 文件 | 字节数 | SHA-256 |
| --- | ---: | --- |
| `TTPlayerRebuild.exe` | 4395520 | `9eae6003a6d4252e5ee97258afae3672bf6fb599af8ad09decf6e797edf5df2b` |
| `TTPlayerRebuild-2026.10.25.zip` | 3418085 | `e55c7990d2c625b06085ab439b05c4eeedeb1d4572395b15c5f60e5dcf038a1a` |

`ttp_osu.dll` 沿用已有版本，仍为可选独立插件。本轮未暂存、提交或推送 Git。
