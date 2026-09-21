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
