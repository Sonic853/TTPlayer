# 歌词菜单：上传、调整、简繁转换、字符编码、内嵌歌词

> 2026-09-21 补充：相关功能已在 XP SP3 / Win7 SP1 虚拟机中执行回归。覆盖项、修复和未覆盖边界见 [虚拟机验证记录](XP_WIN7_VM_VALIDATION.md)。下文保留原日期的历史结论。

2026-09-15。依据仓库根目录的 **5.7.9** `reverse/decompiled/TTPlayer.exe.pseudo.c`，
并用同一 `TTPlayer.exe` 的指令和数据表核实伪代码遗漏的寄存器参数。
不以 TTPlayer6120 为参照。以下是行为恢复，不是原私有类的逐字/二进制等价实现。

## 上传歌词

原版调用链：

`0x8035 → 0044CF5E → 00448427 → 0044895B → 004487A5`

- `0044CF5E` 要求当前曲目及非空歌词，按 `SaveCompress` 序列化歌词，捕获歌曲信息。
- `00448427` 保存 artist、title、album、lyrics 四个 BSTR 快照。
- `0044895B` 建立客户区 **500×480** 的可缩放浏览器窗口、底部状态栏及进度条，
  经 `IWebBrowser2::Navigate2` 打开上传页面，设置页面所在目录的 `Referer`。
- 原地址由 `00540A1C` 指向 `http://www.qianqian.com/dll/lrcup.php`，并非搜索插件的上传 ABI。
- `004487A5` 从 HTML document 的 all 集合查找 `artist/title/album` 三个 input 和
  `lyrics` textarea（名称字面量在 `0051DAAC/0051DAA0/0051DA94/0051DA84`），调用各自的 `put_value`。
  **原程序并不自动提交表单**；提交由网页及用户操作完成。
- `00448D99/00448DB0` 更新状态和进度；`00448CE3` 重排浏览器与状态栏；`00448CC5/00448E0D` 关闭并释放对象。

按用户本轮追加要求，重建版不再固定使用官网地址，改为当前选中的歌词搜索服务器的
**协议、主机、端口 + `/dll/lrcup.php`**：

```text
https://lyrics.example:8443/api/search/?key=1
→ https://lyrics.example:8443/dll/lrcup.php
```

保留 HTTPS、显式端口、IPv6；不继承搜索路径、查询字符串、fragment 或账号密码。
无有效服务器时给出提示，不偷偷回退到旧官网。
服务器列表尚未读取时，复用后台 catalog job；先捕获曲目信息，避免读取完成前切歌导致填入另一首歌。

`lyric_upload_window.cpp` 用 Windows SDK 的 OLE 接口承载系统 WebBrowser 控件，
不引入 ATL/原版 EXE 依赖。接收 DocumentComplete/ProgressChange，支持键盘消息、缩放、关闭、重新打开。
页面不可用/缺少必要字段时显示失败状态；30 秒仍未完成则停止浏览器导航，窗口始终可关闭。
只给请求地址对应的顶层文档填值，不把歌词泄露给重定向后的其它地址或 iframe；
字段通过 DOM 属性赋值，不拼 HTML/脚本，也不自动 POST。

接口确认参见微软 [DocumentComplete](https://learn.microsoft.com/en-us/previous-versions/windows/internet-explorer/ie-developer/platform-apis/aa768282%28v%3Dvs.85%29)
与 [IWebBrowser2](https://github.com/MicrosoftDocs/sdk-api/blob/docs/sdk-api-src/content/exdisp/nn-exdisp-iwebbrowser2.md)。
搜索服务能返回歌词 **不代表** 它提供兼容的上传网页。本轮没有对任何外网服务器提交歌词，
没有验证用户配置服务器的上传后台；该功能依赖服务器实现以上表单及系统 WebBrowser 组件。

## 菜单核查及已修复部分

| 功能 | 原版依据 | 重建版修正 |
| --- | --- | --- |
| 调整本句 | `0044C572 → 004460B7 → 0043D771` | 首句之前无当前行时不修改第一句；仅修改当前行 ±500 ms |
| 调整其后 | `004460EC → 0043D795` | 从 **当前行 + 1** 开始；最后一行之后为 no-op，首句之前为全部 |
| 全部调整 | `0043E92C → 0043D7D0`、`0044C471` | 保留有符号时间相加、不 seek、不加音频淡入淡出；对话框初值 1000 ms、范围 ±10000 ms、防重复进入 |
| 显示联动 | `004460B7/004460EC/00446125 → 00416CB1` | 普通歌词更新后同时清除桌面歌词字形缓存；记录 dirty 状态，进入编辑器不再从磁盘覆盖修改 |
| 简繁转换 | `0044DCCF/0044DD4E → 00446125 → 0043D803 → 004C1C5E` | 普通模式转换全部歌词正文；编辑模式仍只替换选区并保留撤销；使用线程 locale，不转换元数据或时间戳 |
| 字符编码 | `0044AAB2/0044D601` | 补齐动态菜单、选中状态和字体字符集通知；不重新读取文件、不更改文件编码 |
| 内嵌读取 | `0044C648/004AD30E` | 保留 Lyrics/Lyric 读取路径；编辑器直接载入返回的原文，不先经显示模型重排 |
| 内嵌写入 | `0044C708/004AD3E9/004C80AC` | 改为可写 metadata 流、直接 slot 6（不误套文件属性 bit 4 限制），成功后清空旧 Lyric，释放 reader 使插件完成写回 |
| 内嵌删除 | `0044C88F/004AD3E9` | 置空 Lyrics 后清理 Lyric，并更新当前曲目的 metadata 缓存；不再 Stop 当前音频，不把失败当作成功 |
| 可用状态 | `00449E98` | 分别按当前曲目/非空歌词启用读取、写入、删除、上传、重载等命令 |

资源菜单 143 的“字符编码”原本只有“系统默认”和分隔线。
`0044AAB2` 将资源字符串 **0x8171** 按 `|` 分割作为项目名，不足时用 EXE 的英文表回退。
`0x805C..0x806A` 的字符集依次为：

```text
0, 134, 136, 130, 128, 178, 186, 238, 161, 129, 177, 204, 222, 162, 163
```

伪代码接受 `0x805C..0x806F` 路由范围，但后五个值不在有效表中。
表首地址 `0053B9A0` 实际也存放调整对话框初值 1000；原版显式把首项解释为 charset 0，不能把 1000 截断后用于字体。

序列化函数 `0043DEC8` 的第二个参数是 **合并重复歌词时间标签**，不是“应用 offset”。
上传按 `SaveCompress`，嵌入歌词固定使用合并序列化。重复正文保留一个条目，后遇到的时间标签向前插入。
重建模型独立存储 offset，新序列化将其折入实际显示时间，不另写 offset，避免再次载入产生二次偏移。

## 仍未完全等价的部分

以下前三项记录本专项完成时的边界。后续已接入保存策略、自动标签写入和活动 reader
队列；当前结果及 CUE／插件限制见 [WTL 界面后续恢复](WTL_UI_RECOVERY.md#1-歌词保存策略与内嵌标签)。

- 原版 `004AD25D` 可取得当前音频的现有 reader；重建版内嵌修改目前仍另开 metadata reader。
  播放期间原只读流可能禁止第二个写句柄。宿主机 FLAC 测试确认这种情况下删除返回失败，
  但播放状态与进度不会被停止/重置；这不等价于原版复用当前 reader 的完整写入能力。
  未实现该共享 reader 事务前，不应声称所有正在播放的格式均可实时修改内嵌歌词。
- `0044A6E6` 的完整 dirty 文档保存策略（`LyricSaveMode` 的不保存/询问/自动保存，
  换歌及关闭时保存普通显示模式的修改）仍未完整接入。此次补上 dirty 和进入编辑器后的保留，
  但并未补齐所有离开文档时的事务；需要持久化调整/简繁转换时应进入编辑器保存。
- `AutoSaveLyricTag`、CUE 逻辑条目和实际底层文件的完整共享写入流程仍有前述恢复文档中的限制。
- 没有验证历史网站、真实上传、所有第三方格式以及每种 Windows/WebBrowser 环境。

## 宿主机测试

`tests/ui/lyric_menu_tests.cpp` 仅保留在被 Git 忽略的本地 tests 目录：

```powershell
cmake -S . -B build -DBUILD_TESTING=ON -DTTPLAYER_STAGE_RUNTIME=OFF
cmake --build build --config Release --target lyric_menu_tests ttplayer_rebuild --parallel 4
ctest --test-dir build -C Release -R '^lyric_menu_tests$' --output-on-failure
# 可选：使用传入音频的临时副本验证真实 AddIn 写入（需匹配的 AddIn）。
./build/Release/lyric_menu_tests.exe .. '<generated-fixture.flac>'
```

测试包括：所有调整范围边界、普通/编辑模式转换、dirty 保留、15 个字体字符集及菜单勾选，
压缩序列化与 offset 往返、HTTP/HTTPS/端口/IPv6 地址生成、非法地址拒绝。
真实 WebBrowser 加载**临时本地 HTML**，通过 DOM 核验四个字段（含中文及 `&<>`）、
500×480 客户区、缩放、关闭/重新打开、不完整网页不填值；无外网请求或自动提交。

另用 FFmpeg 生成的 2 秒 FLAC 临时副本执行：嵌入→释放→重新读取→删除→再次读取；
确认旧 `Lyric` 清空、播放条目缓存更新，静音播放期间的删除失败不打断播放。
不改写用户歌曲、原版配置或服务器 INI。

关联回归：`lyric_menu_tests`、`lyric_association_tests`、`lyric_search_tests`、
`lyric_service_catalog_tests`、`desktop_lyrics_menu_tests`、`fullscreen_lyric_render_tests`。
宿主机回归结果 **6/6 通过**（13.35 秒）；真实 FLAC 附加测试输出
`embedded FLAC: write/read/delete passed; live delete=0, playback preserved`。
Release 输出为 `build/Release/TTPlayerRebuild.exe`，运行时资源部署关闭，保留现有配置。

## 2026-10-20：MP3 内嵌歌词入口回查

本节是本轮**分析、原版对照和诊断测试**，尚未修改播放器实现或重新发布 EXE／DLL。
此前的插件 FLAC 测试及标签层单元测试不能证明内置 MP3 的歌词菜单已接入。

### 1. 结论和样本

“该音频文件不支持文件属性标签！”来自宿主把 `E_NOINTERFACE` 映射为资源 `0x817F`。
两份用户样本均为有效 MPEG-1 Layer III；现有内置 MP3 标签写入器可以写入、读回和删除歌词。
实际缺口在歌词菜单、播放来源包装和标签服务之间的连接。

| 样本 | 音频 | 原有标签 | 对应 LRC |
| --- | --- | --- | --- |
| 陈慧娴 - 千千阙歌.mp3 | 11,963,036 字节；44.1 kHz，立体声，320 kbps；约 299.076 秒，11,449 个完整帧 | 没有 ID3v2、APEv2、Lyrics3、ID3v1 | 5,871 字节；GBK，无 BOM；67 个行时间标签、396 个逐字时间标签 |
| 苏幕遮 - 范仲淹AI.mp3 | 5,430,821 字节；48 kHz，立体声，192 kbps；约 226.219 秒，9,428 个完整帧 | APEv2＋ID3v1；有标题、艺术家、备注、副标题；没有内嵌歌词 | 1,409 字节；UTF-8 BOM；37 个普通行时间标签 |

没有旧标签的 MP3 也能新增歌词标签。两种 LRC 编码和第一份的逐字时间标签均不是接口错误的原因。
两份音频未发现帧区间之外的未解释数据；无需转码。

原始文件 SHA-256：

```text
陈慧娴 - 千千阙歌.mp3
120ea2d58d0bed806b3355008fd5ab6e9a1588c75880295b5e4a9679ec9d27d6
苏幕遮 - 范仲淹AI.mp3
fbbcfac553d672c16c918bb59a3c1cf05028b456e78fbeee427d20810105013a
陈慧娴 - 千千阙歌.lrc
2d7986d781c103b73253b76981b5bb29e9c8f9e9cbd6956c66f65a9aee049988
苏幕遮 - 范仲淹AI.lrc
302639d3a40d8604407798c9c95d5e28faca11005dd4718178a9c800c1c15bc2
```

### 2. 原版 5.7.9 的完整相关流程

目标 `TTPlayer.exe`：1,882,544 字节，版本 5.7.9.0，SHA-256
`c7999ea5823469c1684148cac1bd196009824348ccac5cdd45978f8fb574e28c`。
证据来自 `reverse/decompiled/TTPlayer.exe.pseudo.c`、原二进制反汇编／虚表及运行副本。
本轮 REA MCP 未注册；没有把已有伪代码或普通反汇编描述为新的 REA 会话证据。

1. **收集文本**：`0x8031 → 0044C708`。普通模式通过 `0043F319(..., 1)` 序列化，编辑模式通过 `00443213` 取得编辑文本；没有当前曲目或文本为空时结束。
2. **取得当前 Reader**：`004AD25D` 可复用曲目 `+0x9C` 持有的对象；`004139B5` 查询 `ISoundContent`。不存在接口时才使用 `0x817F`。
3. **修改标签**：`004C80AC` 处理只读属性；`004AD3E9` 调用 metadata 虚表 slot 6：先写 `Lyrics`，成功后清空旧 `Lyric`。设置失败使用 `0x8180`“该音频文件无法写入歌词标签！”。
4. **MP3 自带元数据对象**：`004C8B76 → 004E477D → 004D9A0C`，MPEG Reader 与标准标签对象共享生命周期，播放解码器并不决定 MP3 是否支持标签。虚表 `00524EFC` 的 slot 6（`00524F14`）指向 `004D9E23`。
5. **先改内存，后落盘**：`004D9E23` 设置字段及 dirty；最后释放 Reader 后，`004E4828 → 004D9A59 → 004D9B56` 才保存。菜单释放临时引用不等于最后一次 Release。本轮两份副本在菜单完成及 Stop 后仍未改变，退出释放对象后才出现 Lyrics3。
6. **读取／删除使用同一对象**：`0044C648 → 004AD30E` 读 `Lyrics`，该查询失败才尝试 `Lyric`；删除 `0044C88F → 004AD3E9` 传空文本。活动 Reader 的内存修改可以被立即读回。
7. **写入策略**：`004D9B56` 使用 `MP3WriteTagType`；`004DC39E` 按策略生成标签。选择 ID3v1 时歌词经 `004DBBEF` 写入 Lyrics3，歌词并非写进 ID3v1 的定长文本字段。ID3v2 可使用 USLT，APEv2 可使用 Lyrics。
8. **读取优先级**：二进制 `004D9B0B` 压入全局 `0053CA40`，`004D9B23` 调用 `004DC174`，使用 XML 的 `MP3ReadTagPriority`，逐字段合并各标签。

直接二进制还确认 `0044C79E push 1 → 0044C7A4 call 004AD25D`，
`004AD445` 从曲目 `+0x9C` 取 Reader，`004AD46A` 调用 `+0x18` 的 setter。
接口 IID 为 `{7AD84E00-5FEF-4481-B532-FBBD677E67C2}`。

原版菜单成功仅能说明 setter 接受了修改，不能证明延迟保存一定成功；旧 Release 没有提交结果返回值。
另外原版 `004AD3E9` 先更新曲目缓存再调用 setter，失败时缓存与磁盘也可能不一致，恢复时应保留成功校验。
逐字标签在编辑文本中可以保留，不据此声称原版具有重建版的逐字高亮能力。

### 3. 当前重建版为什么报错

相关实现：

- `src/ui/player_window_lyrics.cpp:3894`，`WriteEmbeddedLyrics`。
- `src/audio/audio_engine.cpp:2116`，`AudioEngine::WriteCurrentLyrics`。
- `src/audio/audio_engine.cpp:1344`，`RecoveredFileSource::WriteLyrics`。
- `include/ttplayer/audio/audio_engine.h:218`，默认 `DecodedAudioSource::WriteLyrics` 返回 `E_NOINTERFACE`。
- `src/audio/builtin_file_info.cpp:2286`，已有 `WriteBuiltinFileInfo`。

当前链路为：

```text
歌词菜单
  → WriteEmbeddedLyrics
  → 活动 AudioEngine 的 WriteCurrentLyrics
  → RecoveredFileSource 仅转发给实际解码来源
  → 系统 MP3 来源没有标签写入方法，返回 E_NOINTERFACE
  → 菜单显示 0x817F
```

活动来源返回的 `optional<HRESULT>` 即使包含失败值也被视为已处理，不会进入后面的另开插件 Reader 分支。
没有活动来源时，该分支也只向 PluginManager 查询，仍没有接入内置 MP3 写入器。
因此只补“活动返回 E_NOINTERFACE 后再试插件”不能解决内置 MP3。

读取也存在同类缺口：`ReadEmbeddedLyrics`（同文件 3876）仅通过 `OpenReader` 查询插件，
没有内置 MPEG 标签回退，也不读活动 Reader 的待提交文本。普通自动加载却会使用播放元数据，故两个入口可以表现不同。
删除、已内嵌文档保存、自动内嵌均复用 `WriteEmbeddedLyrics`，必须一起覆盖。

### 4. 本轮副本实测

使用当前 Release EXE，SHA-256
`f6f92b67f8ac3c643584a53b88dfe39d56a03800594ea6a29927f011e2828e40`。

| 检查 | 千千阙歌 | 苏幕遮 |
| --- | --- | --- |
| 原版菜单嵌入 | 无错误；最终 Release 后写入 Lyrics3 | 无错误；保留 APEv2／ID3v1，增加 Lyrics3 |
| 重建版菜单嵌入 | 复现 0x817F；文件未改变 | 复现 0x817F；文件未改变 |
| 直接使用现有文件属性工作进程，单字段 Lyrics | 播放实例运行期间及 Stop 后均 S_OK | 播放实例运行期间及 Stop 后均 S_OK |
| 成功落盘后再用重建版菜单读取 | 仍提示标签中未包含歌词 | 同左 |
| 写入类型 5／4／8 的独立写、读、删 | 三种均通过 | 三种均通过 |

三种策略共 **6/6** 通过。独立解析 Lyrics3／APEv2／USLT 验证歌词原文，包括 396 个逐字标签；
音频帧区间在写入、删除前后逐字节相同，苏幕遮原有四个文本字段在写入后保持。
测试中的 ID3v2 编码使用 UTF-16；没有把该结果扩大为所有编码策略的完整测试。

还有一个与本次报错独立的字节差异：无标签样本在原版策略 5 下只生成 Lyrics3，
重建版写入器还生成空 APEv2／ID3v1。可单独优化空标签省略，不影响上述接口错误的定位。

测试范围为本机 Windows 11；没有把历史 XP／Win7 测试算作本轮重测，也没有保证所有解码器占用状态都能原地保存。
原样本四个文件的 SHA-256 在测试前后相同。

证据与脚本：

- `out/embedded-lyrics-analysis-20261020/final/runtime-results.json`、各副本目录的错误对话框截图。
- `out/embedded-lyrics-analysis-20261020/final/container-results.json`、`original-hashes.json`。
- `tests/audio/embedded_lyrics_analysis.py`，本地测试，不接入 Actions，不提交或推送测试。

## 2026-10-20：同类缺口扩查

### 1. 已实测的额外问题

#### G1：部分插件写入歌词后又被别名清理删除（高优先级）

`LegacyPluginSource::WriteLyrics`（`audio_engine.cpp:627`）与独立 Reader 分支
（`player_window_lyrics.cpp:3923`）均先设置 `Lyrics`，再设置 `Lyric=""`。
宿主标准标签对象的 `CanonicalField`／`CanonicalTagName`
（`builtin_file_info.cpp:371`、`:428`）把两者合并，所以第二次操作删除第一次结果。

APE、TTA、TAK、原始 AAC 调用宿主 `CreateStandardContent`；可分别追到
`ttp_ape/src/reader.cpp:301`、`ttp_flac/src/reader.cpp:270`、
`ttp_tak/src/reader.cpp:163`、`ttp_aac/src/reader.cpp:45`。

用真实 Release 菜单和独立生成样本副本已复现 **APE、TTA、原始 AAC**：
嵌入不报错，手工读取报告没有歌词，退出后磁盘依旧没有 Lyrics。
相同副本改由工作进程仅写一次 canonical `Lyrics`，均成功且原文读回一致。
TAK 目前是相同调用链的代码确认，未列为本轮运行通过／失败。

修正不能把这两次操作原样接进内置 MP3。内置 writer 使用单个规范字段；
插件路径需兼容真正分开存储单复数字段的实现，可考虑先清理旧别名再写规范字段，并区分不存在与真实写入失败。
保存后必须校验结果，不能只看第一条 setter 的 HRESULT。

#### G2：播放读取不遵守 MP3 标签优先级

`RecoveredFileSource::Open`（`audio_engine.cpp:1244`）给内置读取器传默认 `{}`，
而播放列表后台、文件属性、媒体库传入用户设置（分别见
`player_window_playlist_info.cpp:351`、`player_window_playlist_properties.cpp:298`、
`player_window_library.cpp:1634`）。`PlaybackOptionsFromSettings` 没有携带 MP3 标签策略。

已构造 APEv2 Lyrics 为 `Priority A`、ID3v2 USLT 为 `Priority B` 的副本：

| 读取路径 | 实测结果 |
| --- | --- |
| 文件属性工作进程，APEv2 优先 `0x04080102` | Priority A |
| 文件属性工作进程，ID3v2 优先 `0x08040102` | Priority B |
| 配置同为 ID3v2 优先，实际播放后进入歌词编辑器 | Priority A |

因此会出现属性与播放信息不同，并影响内嵌歌词选择。应向播放来源传递同一配置快照。
转换为 LAME MP3 的 `playlist_transforms.cpp:1688` 也使用默认写入策略，需一并核对全局保存选项的传递；
转换差异本轮仅代码确认，未声称完成转换界面复现。

运行证据：`out/embedded-lyrics-analysis-20261020/related-gaps/results.json`、
`related-gaps/priority/result.json`；脚本 `tests/audio/embedded_lyrics_gap_checks.py`。

### 2. 调用链确认、尚未逐项运行复现的问题

| 编号 | 缺口与触发条件 | 代码证据与影响 |
| --- | --- | --- |
| G3 | 新插件活动修改成功，外层返回旧快照 | `audio_engine.cpp:1269` 因格式识别信息设置 `metadata_override_`；`:1344` 只写子来源，`:1359` 返回 Open 时快照。新 FLAC／OGG 等的内存 Lyrics 可以仍旧，删除也可残留 |
| G4 | 写入后实际播放列表未同步 | `ProcessLyricWrite:2136` 未增加 `metadata_revision_`；`player_window.cpp:7672` 因版本不变跳过更新；歌词菜单仅改 `opened_track_`，而 `playback_track_state.h:31` 优先返回列表行。重载歌词可能取旧值，现有测试只查 opened 快照漏掉此路径 |
| G5 | 最后一个字段删除后，空快照不能清缓存 | `player_window.cpp:7685`、`:7700` 只有 entries 非空才同步。即使补版本通知，也需区分“未提供标签”与“已提供空标签”，才能删除最后一个 Lyrics |
| G6 | 活动 Reader 未落盘时手工读回旧歌词 | `ReadEmbeddedLyrics:3876` 始终另开 Reader；活动插件可能只有内存更改。原版读取复用当前 Reader，重建版可能读回旧文本或报无歌词。读取必须理解待提交删除，不能在缓存缺失时把磁盘旧值恢复 |
| G7 | 压缩包内嵌歌词自动显示与手工读取不一致 | 播放支持 archive stream，但手工读取把逻辑包内路径交给普通 `OpenReader → CreateNamedFileStream`（`plugin_manager.cpp:2278`），没有接入只读包内流。嵌入／删除菜单只查当前曲目及文本，也缺少显式只读提示；底层仍拒绝写入，没有证据表明会写回压缩包 |
| G8 | CUE 已能扫描写入增益，却不能按分轨删除增益 | `replay_gain_scanner.cpp:530` 按 subtrack 写 CUE；删除菜单 `player_window_playlist.cpp:5959` 只保留 path 丢掉 subtrack，`RemoveReplayGainTags:1438` 没有 CUE 分支。`:5988` 丢弃删除结果，删除端也缺少扫描端已有的 fresh-reader 校验 |

G3、G4、G5 是三个不同环节；仅把“读取内嵌歌词”改为读缓存仍会读到未同步的旧缓存。
G4 同时需要覆盖无活动来源的停止态写入，以及其它播放列表中同源条目的缓存失效。

### 3. 不应误判为缺口的行为

- 文件属性工作进程已接入内置 MP3 写入、插件显式 Commit、旧 Reader 释放及重开校验，无需再实现一套标签格式写入器。
- 活动插件延迟保存本身与原版一致；问题在立即读取、状态反馈及最后提交失败的处理，不能把所有活动插件强行 Commit，某些实现提交后不能继续解码。
- 外部 LRC 保存成功而可选自动内嵌失败，`SaveModifiedLyrics` 返回外部保存成功是现有约定，应区分两种结果，不把外部保存判失败。
- 文件属性已拒绝压缩包写入；CUE 的 Lyrics 也不在可写字段中。继续保持压缩包只读，不把单个 CUE 分轨的歌词写进整张专辑文件。
- 本轮不改 NCM，不为其增加识别接口。

### 4. 建议修复顺序

1. **先解决成功但丢失歌词**：G1 别名冲突，统一规范字段、旧别名清理和提交后校验；覆盖 APE／TTA／TAK／原始 AAC 与独立标签插件。
2. **补齐内置 MP3 接入**：手动读取、嵌入、删除、已内嵌文档保存、自动内嵌复用已有内置标签读写器。按内容识别和目标身份选择服务，不根据系统解码器是否实现 writer 判断文件能力。
3. **恢复共享标签状态**：采用活动文件的共享标签会话，接受更改后更新所有者快照；G3 外层快照、G4 版本和列表同步、G5 空集合删除同时修复，再修 G6 活动读回。同步依据路径、子轨和文件版本，避免切歌后写错目标。
4. **完成持久化边界**：沿用原版“活动 Reader 接受修改，最终释放提交”的语义，实际写盘复用现有 writer；保留可选显式提交能力与错误反馈。大文件保存不放进解码实时循环，也不把 3 秒请求超时当作已经撤销写入。保存失败应保留可恢复文本。
5. **统一用户策略**：G2 的 MP3 读取优先级贯通列表、播放、属性及歌词；核对转换的写入类型、编码、填充。补空标签省略的原版字节差异。
6. **补特殊来源与其它菜单**：G7 压缩包只读读取／写入禁用，G8 CUE 分轨增益删除及逐项失败反馈。
7. **回归验证**：两份用户样本副本；上述插件；普通与逐字 LRC；播放／暂停／停止；立即读取、重载、切歌、退出重开；最后一字段删除；磁盘只读／占用／版本变化；冲突标签优先级；压缩包只读。最终增加 XP／Win7 与原版对照，不能以标签层测试替代实际菜单测试。

上述扩查阶段只增加分析记录和本地诊断脚本；随后经用户授权实施的修复见下节。

## 2026-10-20：内嵌歌词与同类缺口修复

### 1. 实现结果

| 范围 | 修复内容 |
| --- | --- |
| 用户两份 MP3 | 歌词菜单接入已有内置 MPEG 标签服务，不再要求系统解码器提供插件标签接口；按实际内容判断 MPEG，支持停止态读取、嵌入、删除和编辑保存 |
| G1 别名冲突 | `LegacyReaderSession::SetLyrics` 先清理旧 `Lyric`，再写规范 `Lyrics`，并检查实际值；setter 或检查失败时尝试恢复两个字段的原值，避免最终 Release 把中间清空状态保存；内置写入只提交一个规范字段 |
| G2 标签策略 | `PlaybackOptions` 携带 MP3 读取优先级、写入类型、编码、填充设置；播放来源及 LAME 转换写入使用同一配置快照 |
| G3 来源快照 | 格式识别包装层在修改后更新歌词，保留来源格式与封面，不再返回 Open 时的旧歌词 |
| G4 列表同步 | 更新 metadata revision、正在播放条目、歌词文档条目，以及所有播放列表中的同源同子轨条目；使旧后台信息结果失效 |
| G5 空集合删除 | 增加 `tags_authoritative`，区别“未提供标签”和“已提供空标签”；最后一个 Lyrics 被删后也可清空列表缓存 |
| G6 活动读取 | 先读取匹配当前路径和子轨的活动来源快照；待保存的删除视为明确空值，禁止重新从旧磁盘标签恢复 |
| G7 压缩包 | 手工读取接入只读包内流并验证完整性；压缩包、网络地址、CUE 子轨禁用嵌入及删除，保留读取；不写回压缩包 |
| G8 增益删除 | 保留 `path + subtrack`，CUE 按所选分轨删除；普通文件执行提交、释放、重开校验；后台向 UI 返回逐项失败，只有验证成功才清理缓存 |

### 2. 保存时机与失败处理

对照原版 `004AD25D`／`004AD30E`／`004AD3E9` 和 MPEG Reader
`004D9E23 → 004D9A59 → 004D9B56`，活动来源接受修改后首先更新内存。
手工读回立即看到新歌词；实际文件保存发生在解码器关闭、文件句柄释放之后。
停止态则在后台执行文件操作，等待完成并重新打开验证结果。

重建版具体通过 `DecodedAudioSource::FinishLyrics` 完成最终保存：

1. 活动 MP3 保留请求文本和 Open 时的文件版本，解码实时循环只处理内存修改。
2. 解码线程退出 PCM／输出对象作用域后降低回普通优先级，释放读句柄。
3. 持有文件修改锁，检查文件版本未被其它操作改变，调用已有标签 writer。
4. 插件优先调用可选提交接口，随后释放 Reader，以兼容原版仅最后 Release 保存的实现。
5. 重新打开文件核对歌词；读取错误与字段不存在分别处理，删除不再因读取失败而误报成功。

保存失败通过 `LyricSaveFailure` 返回路径、子轨、错误码和恢复文件位置。
待保存文本以 UTF-8 BOM 保存到配置目录的 `LyricRecovery`，失败时再尝试系统临时目录。
有窗口时显示错误并保留编辑状态；程序关闭阶段仍保留恢复文件。
文件属性操作等待解码器真正释放文件，并收集退出淡出队列的引擎错误。

这里恢复的是“活动 Reader 接受修改、最终释放时提交”的语义，不声称原版与重建版
在 Stop、切歌、退出时持有 Reader 的具体时长完全相同。菜单接受成功不等于此时已落盘；
最终保存错误有单独反馈。第三方插件若已经进入 setter，原有 3 秒等待超时不能视为撤销，
不会因超时再次转入独立 writer 重复写入。
插件原接口没有事务撤销方法，因此原值恢复属于尽力恢复；若第三方插件连恢复用 setter
也拒绝，不能保证撤销成功，仍返回最初的保存错误，不宣称插件写入具备完整事务保证。

### 3. 标签容器差异

在只写 Lyrics、默认写入类型 `5` 的无标签 MP3 上，原版只增加 Lyrics3，
不会额外创建空 APEv2 与空 ID3v1。重建版现已省略这两类空容器；已有字段、未知二进制项、
封面仍保留。其它写入策略继续使用现有 APEv2／ID3v2 USLT 实现。

测试必须核对音频帧字节不变，不能只检查歌词能显示。普通歌词和逐字时间标签
均作为文本保存，不主动改写用户提供的 LRC 文件。

### 4. 验证记录

| 验证 | 结果与范围 |
| --- | --- |
| Release 构建 | 成功；XP／Win7 静态导入检查：21 个 DLL、735 个导入 |
| Windows 11 实际菜单 | 原版 MP3 × 2、重建版 MP3 × 2、APE／TTA／原始 AAC × 3，共 7 项通过；修复版另有两项删除后读回及退出保存通过 |
| 原生集成 | Windows 11、XP、Win7 各 181 项通过，包含播放／暂停、延迟提交、配置优先级、格式识别包装、列表同步、删除最后字段、版本冲突与恢复文件、ZIP 读取／只读／CRC 错误 |
| 插件失败恢复 | Windows 11、XP、Win7 各 586 项通过：58 个编辑／删除失败案例及 4 种布局的版本检查；覆盖独立字段、同义别名、隐藏别名、仅旧字段和最终 Release 保存 |
| 增益删除 | Windows 11、Win7 157 项通过；XP 首次单项失败后，同一旧测试程序重跑和两次增加诊断的测试均为 157 项通过，详见下方限制 |
| 已有回归 | 内置标签完整测试、歌词菜单与 FLAC 活动读写／自动内嵌测试通过；异步来源 UI 元数据更新 15 项通过 |
| 原件与音频帧 | 两份用户 MP3、对应 LRC 及三个生成样本原件哈希不变；菜单写入后独立提取音频主体哈希一致；千千阙歌 396 个逐字时间标签保留 |

证据目录：

- 实际菜单：`out/embedded-lyrics-menu-fix-20261020-final/results.json`，附编辑器读回文本和原件哈希。
- 最终核心集成测试：`tests/artifacts/embedded-lyrics-20261020/20261020-065453-942225/test.log`；
  XP／Win7 日志：`tests/artifacts/embedded-lyrics-fix-20261020/XP-native-final.log`、`Win7-native-final.log`。
- 插件失败恢复：`tests/artifacts/lyric-set-rollback-20261020/20261020-065340-331094/test.log`；
  XP／Win7 日志在 `tests/artifacts/embedded-lyrics-fix-20261020/` 的 `XP-rollback.log`、`Win7-rollback.log`。
- 增益故障注入：`tests/artifacts/replaygain-removal-20261020/20261020-064444-372656/test.log`；
  XP／Win7 复查：`20261020-065045-104408/` 下的四份日志。
- 已有回归：`tests/artifacts/embedded-lyrics-fix-20261020/support/`，包括
  `builtin-full-with-runtime.log`、`lyric-menu-final.log`、`archive-ui.log`。

验证边界：

- XP 增益删除首次停在首个 CUE 删除断言，旧日志未打印 HRESULT。新目录中原程序重跑通过，
  增加诊断后两次均返回 `S_OK`。原因未确定，不把它宣称为已定位或修复的 XP 缺陷；
  后续测试已增加 HRESULT 与异常诊断，以便复发时定位。
- 原版播放器实际菜单对照在 Windows 11 进行；XP／Win7 使用相同生产核心的原生集成测试，
  不把这两种证据混为所有系统上的逐像素／逐菜单对照。
- 本轮没有 TAK 实体样本的实际菜单验证，修复了它共用的标准内容接口路径；
  LAME 转换验证到配置传递和标签 writer，未新增完整转换界面端到端用例。
- 故障注入覆盖提交失败、虚假写入成功、读取错误、版本变化与占用；没有进行断电实验。

本地测试保留在 `rebuild/tests`，不加入 Actions，不提交或推送本轮测试。
不修改 NCM 插件及其接口。

最终输出：`build/Release/TTPlayerRebuild.exe`。
SHA-256：`f969e53b69b3d5e5d58330ded58feaadbee47679c7788917a7839f4654cb9c8c`。
最终构建日志：`out/embedded-lyrics-analysis-20261020/build-final.log`。
