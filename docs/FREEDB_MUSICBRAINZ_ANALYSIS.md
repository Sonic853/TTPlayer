# 原版 freedb 实现与 MusicBrainz 替代分析

MusicBrainz 可以承接原版的 CD 和 CUE 整碟信息查询，但需要新的查询后端、准确的轨道布局和结构化结果映射。保留原版“查询、选择、预览、确认保存”的交互及本地缓存，比把 MusicBrainz 强行包装成旧 CDDB 文本协议更适合当前重建版。

分析日期：2026-10-06。原程序依据为 `reverse/decompiled/TTPlayer.exe.pseudo.c` 和本地 EXE 反汇编；原 EXE SHA256 为 `c7999ea5823469c1684148cac1bd196009824348ccac5cdd45978f8fb574e28c`。下文地址均为该二进制首选基址下的 VA，函数名称按行为归纳。

本文主体记录实施前的静态分析。2026-10-06 已按建议接入 MusicBrainz 并恢复 CDA 播放、属性与本地缓存；最新实现及本机／虚拟机验证见 [CDA 与 MusicBrainz 实施记录](CDA_MUSICBRAINZ_IMPLEMENTATION.md)。没有上传用户音频，也未进行实体光驱测试。

## 原版功能范围

freedb 查询由播放器 EXE 实现，不是独立音频解码插件。它使用整张盘的轨道起点、曲目数量及结束时间查询专辑信息，不需要把音频内容发送到服务器。

| 入口 | 原版行为 |
| --- | --- |
| 播放列表命令 `0x7EFA`，`00483D4E` | 取得焦点曲目，仅接受 CD Audio/CDA 或 CUE 来源 |
| 自动查询，`0047FEA3` | 检查自动查询开关、CD 类型及已有信息的空值条件后进入查询 |
| `0048004F` | 已有相关窗口时不重复打开；否则进入布局构建 |
| `0043840A` | 根据物理 CD 或 CUE 生成整碟查询参数和保存目标 |

自动查询的空值检查位于信息读取后的回调路径。该调用点的伪代码没有完整恢复传入的字段参数，本文不把它扩大解释为“所有标签为空”，也不声称它对任意 MP3 自动联网。

## 整体调用流程

```text
焦点 CDA 或 CUE
  → 取得整盘轨道布局和保存目标
  → 工作线程发送 cddb query
  → 接收候选专辑
  → 用户选择候选
  → 工作线程发送 cddb read
  → 显示专辑和曲目，可调整文本编码
  → 用户确认
  → 写本地 CDDB XML 或 CUE
  → 通知播放列表刷新
```

| 地址 | 职责 |
| --- | --- |
| `004380DC`、`004380F3` | 时间数字和、物理 CD 分支的 Disc ID 组合 |
| `00437F31` | 初始化服务器、身份字符串并启动 query |
| `00438176` | 拼接 hello、客户端版本和协议版本 |
| `004381B2`、`00438241` | HTTP query 与 read |
| `004382D0` | 网络工作函数，建立连接并分发结果 |
| `00438E0C` | 查询候选解析 |
| `00438F7D` | 专辑记录解析 |
| `004391F2`、`004394B8` | 结果窗口和编码列表初始化、预览刷新 |
| `004397F3` | 切换候选并读取详情 |
| `004398AA` | 应用选择，保存 CDDB 或 CUE |
| `00438DB2` | 结束网络任务、清理并关闭窗口 |

## 轨道布局与 Disc ID

### 物理 CD

`0043840A` 先解析盘符并取得卷序列号，再尝试以 MSF 地址模式读取 TOC。MSF 是“分、秒、帧”，每秒 75 帧。设备读取失败时，原版还有枚举 `.cda` 描述文件、从描述中的时间恢复布局的后备路径。

内部为每条记录保存分钟、秒、帧和组合地址；最后追加 lead-out，即音频结束位置。query 参数使用各音轨的帧地址，末尾另附结束时间的秒数。

必须区分三种标识：

| 标识 | 原版用途 | 是否可以直接给 MusicBrainz |
| --- | --- | --- |
| Windows 卷序列号 | 本地 `.cddb` 文件名及部分进程内去重 | 否 |
| freedb Disc ID | 旧网络协议的 8 位十六进制标识 | 否 |
| MusicBrainz Disc ID | 根据完整音轨布局计算的标识 | 是，需要重新计算 |

### 原版物理 CD 算法的一处差异

`004380DC` 对整数逐位求十进制数字和。`004380F3` 对所有音轨起始秒数求和并模 255，将其放到高 8 位；低位组合轨道数量和结束时间。

核对 `00438139` 至 `0043816E` 的机器码后，可以确认此函数直接使用最后记录的 `分钟×60+秒`，没有减去第一轨起始秒数。调用方 `00438790` 起把绝对 lead-out 转成最后记录，故这不是单纯的反编译漏参。

标准 freedb 算法的时间项则是 `leadout_seconds - first_track_seconds`，可对照 [libdiscid 的实现](https://raw.githubusercontent.com/metabrainz/libdiscid/master/src/disc.c)。在首轨从 150 帧开始的普通布局中，这两种算法相差 2 秒的时间项。

用 MusicBrainz 官方六轨示例做离线计算：

| 项目 | 结果 |
| --- | --- |
| 起点 | `150, 15363, 32314, 46592, 63414, 80489` |
| lead-out | `95462` 帧 |
| 标准 freedb ID | `3404f606` |
| 原版 CDA 函数组合规则 | `3404f806` |
| MusicBrainz ID | `49HHV7Eb8UKF3aQiNmu1GR8vKTY-`，与官方示例一致 |

这证明算法差异，不能据此断言某台历史 freedb 服务器会怎样模糊匹配。迁移时应从准确 TOC 重新计算 MusicBrainz ID，不携带这个旧差异。

### CUE

CUE 路径通过 `004DD5FD` 读取曲目布局，必要时打开关联媒体取得结束时长。生成查询时可见 `floor(2.5)` 的两秒起点，以及轨道帧差先除以 75、再累加秒数并乘回 75 的处理。由于先进行了整数除法，后续 `floor(x+0.5)` 不能恢复已经丢失的帧精度。

该路径能为旧式近似查询构造参数，但不适合直接生成精确 MusicBrainz ID。重建时需要直接保留 `INDEX 01` 的帧值，并补足末尾时长、多个 FILE 的累计位置和间隙语义。

## 网络线程与协议

默认端点为旧 `http://freedb.freedb.org/~cddb/cddb.cgi`。两步请求形式为：

```text
?cmd=cddb+query+<discid>+<tracks>+<offsets...>+<end-seconds>
?cmd=cddb+read+<category>+<discid>
```

`00438176` 追加 hello 字段、`ttplayer+5.7.9` 及 `proto=5`；初始化代码包含 `anonymous@anonymous.com` 身份字符串。

HTTP 层使用 WinINet。`004382D0` 将超时参数初始化为 `60000` 毫秒，并复用代理配置。`00411A5B` 用 `_beginthreadex` 创建工作线程；响应解析使用临界区，再通过 `PostMessageW(..., 0x7F6, ...)` 通知界面。查询和读取详情可以复用同一 worker 的不同任务类型，并非每条歌曲同时发请求。

取消路径使用事件和线程清理，关闭界面前会结束任务并清空结果。原版的基本原则是网络处理离开 UI 线程、结果在窗口线程应用，这一点适合保留。

## 返回内容与结果窗口

候选解析函数接受 `200`、`210`、`211` 返回码。详情解析识别 `DISCID`、`DTITLE`、`DGENRE`、`DYEAR` 和 `TTITLEn`，跳过以 `#` 开头的注释。

原版以窄字符串保留服务器文本。结果窗口枚举系统可用代码页，默认取 `GetACP()`；用户切换代码页后，重新生成候选名称和属性预览。此处的编码选择用于解释服务器返回的字节，不是重新识别音频格式。

候选切换会清空旧预览、禁用确认等控件，发起详情请求；收到详情后再允许确认。最终以 `DTITLE` 中的 ` / ` 分隔艺人和专辑，曲目标题中的同样分隔可表达独立艺人，供合辑使用。

以下是恢复旧协议时应单独验证的边界：

- `200` 与多行返回进入同一解析块，首行内容的处理可能漏掉单条精确结果，需要协议响应夹具验证。
- 行分隔 helper 明确寻找 CRLF 或 LFCR，对只有 LF 的响应缺少同等处理。
- `TTITLEn` 的数字虽被解析，当前函数却按遇到的行追加曲目；没有看到按数字归位及续行合并的逻辑。
- 详情返回数量和来源轨道数不能仅以最小值裁剪后就认定匹配。

这些是静态代码边界，不是本次通过在线旧服务复现的运行结果。MusicBrainz 后端应使用独立结构化解析，不继承上述文本协议限制。

## 确认后写到哪里

### CD 音轨

目标为 `<播放器目录>\CDDB\<十进制卷序列号>.cddb`。`004398AA` 组装专辑和轨道字段，再调用 `004DF1EC` 保存 UTF-8 XML。根节点为 `CD`、版本为 `3`，专辑在根属性，轨道使用 `Track01` 等子节点。

这一步写本地文件，不修改音频 CD。CDA Reader 通过 `004DF142` 读取缓存；手动属性修改则在 Reader 释放时合并写回，具体见 [CDA 实现分析](CDA_IMPLEMENTATION_ANALYSIS.md)。

### CUE

`004398AA` 重新读取 CUE，更新专辑、表演者及曲目字段，再调用 `004DD8BF` 保存。该分支没有对每首关联音频执行标签写入；不能把“更新 CUE”描述为已经更新 FLAC/APE 等文件的内嵌标签。

成功后向来源窗口发送 `0x7F6` 刷新通知。保存失败有单独提示，不能将网络请求成功等同于本地保存成功。

## 实施前重建版的基础与缺口

| 部分 | 当前代码状态 |
| --- | --- |
| 菜单入口和曲目筛选 | `playlist_network_commands.cpp`、`player_window_playlist_network.cpp` 已有；最终仍显示后端不可用 |
| 自动查询和服务器配置 | Settings 保存 `FreedbAutoQuery`、`FreedbServer`；存在字段不代表已有运行后端 |
| 光驱 TOC | `audio_engine.cpp` 的 CDA 源读取 TOC，但需要抽出整碟查询共用模型 |
| CUE 精确起点 | `CueTrack` 保存 75 fps 帧值、INDEX 00/01、源文件和轨道号 |
| 完整 CUE 光盘布局 | 尚需多文件长度、PREGAP/POSTGAP、末轨结束等归一化处理 |
| CUE 保存 | `CueSheet::WriteTrackMetadata` 已有快照检查及替换式保存；整碟应用需要批量事务入口 |
| CDDB 本地读写 | 当前 CDA 源未恢复原版缓存闭环 |
| HTTPS | 独立 `ttp_https.dll` 已有 TLS、代理和取消基础，但公开接口仍偏歌词请求 |
| MusicBrainz 查询和结果映射 | 未实现 |

`WriteTrackMetadata` 会拒绝过期快照。不能对同一个 `CueSheet` 快照循环写多首，第一笔修改后便可能触发过期检查；也不宜通过逐首重新加载把整碟保存拆成多次非整体提交。应一次校验、合并所选字段并写回。

## MusicBrainz 替代方式

### 使用正式 API

MusicBrainz 已发布旧 freedb 网关的停止维护公告，并要求迁移到正式接口；它不是可直接填入旧 `cddb.cgi` 配置的服务器。[官方公告](https://blog.metabrainz.org/2018/09/18/freedb-gateway-end-of-life-notice-march-18-2019/)

正式 API 使用 HTTPS，可返回 JSON 或 XML，公开信息读取通常不需要登录或 API key。Disc ID 查询支持关联发行版，并可根据 TOC 做近似查找；有 CD stub 的响应分支，应明确选择是否接纳。[MusicBrainz API](https://musicbrainz.org/doc/MusicBrainz_API)

建议第一版使用正常 release 结果，并设置 `cdstubs=no`，把无匹配与网络失败分开。请求可按以下逻辑分成两步，避免一开始拉取所有候选的完整信息：

```text
GET /ws/2/discid/<MB-discid>?fmt=json&inc=artist-credits+labels&cdstubs=no&toc=<TOC>
GET /ws/2/release/<release-mbid>?fmt=json&inc=recordings+artist-credits+discids+genres
```

正式实现应按官方响应夹具验证 include 组合、字段缺省及容量上限，不把这里的接口设计当作已经完成联网验收。模糊结果需要显示匹配性质，不能伪装成 Disc ID 精确命中。

2026-10-06 实际 API 验证：discid 查询使用 `artist-credits+labels`。分析草案中曾拟议的 `media+discids` 组合返回 HTTP 400，已更正；不能直接沿用 release 查询的 includes。

### Disc ID 要重新生成

MusicBrainz 对首末音轨号、lead-out 和 99 个音轨偏移的规定格式计算 SHA-1，再使用其特殊 Base64 字符表，结果为 28 个字符。LBA 输入需统一到含 150 帧偏移的坐标；已是绝对 MSF 帧的值不能再加一次。混合模式与多区段光盘还需处理数据轨和音频段结束位置。[Disc ID 计算规则](https://musicbrainz.org/doc/Disc_ID_Calculation)

建议建立 `DiscLayout`，显式保存来源、坐标单位、音轨编号、轨道类型、帧偏移、lead-out 和布局可信度。普通 CD 和完整单文件 CUE 先接通；只有截取片段、缺失末轨时长或无法解释多文件间隙的 CUE，不能伪造精确布局。

官方 `libdiscid` 能从光驱读取或通过 `put()` 接受已有布局，提供 Windows 构建，许可为 LGPL 2.1 或更新版本。[libdiscid](https://musicbrainz.org/doc/libdiscid)

当前项目已经有光驱和 CUE 代码，第一阶段可按公开算法实现小型 ID 计算器，用官方示例和 libdiscid 对照校验，减少新运行依赖。若采用 libdiscid 的设备读取层，应独立验证其混合模式行为和所选构建的 XP 兼容性，不能由“提供 Win32 包”推断支持 XP。

### 候选需要区分发行版和碟号

MusicBrainz 的 release 表示具体发行版本，每个 medium 表示其中一张碟及其曲目表。同一专辑的不同地区版、再版和豪华版可能是不同 release，多碟专辑不能直接取 `media[0]`。[Release 模型](https://musicbrainz.org/doc/Release)

候选界面应显示专辑、艺人、发行日期、地区、厂牌或目录号、碟号和轨数。先选匹配的 medium，再按轨道位置映射。不要因为轨数相同就自动覆盖，也不要依据当前播放列表排序分配歌名。

| 原版字段 | 建议的 MusicBrainz 映射 |
| --- | --- |
| 专辑 | release 标题 |
| 专辑艺人 | release 的 artist-credit |
| 曲目标题 | 选定 medium 的 track 标题；必要时回退 recording 标题 |
| 曲目艺人 | track 的 artist-credit；按响应语义回退，并保留连接文字 |
| 日期 | 所选 release 日期；不要默认为最早发行日期 |
| 曲号及碟号 | medium 内曲序、medium position 和总碟数 |
| 流派 | 单独映射实际返回的 genre；不存在时保留原值 |
| 来源标识 | MB Disc ID、release MBID、recording MBID 分开保存 |

track 是某次发行中的曲目，不能与跨发行复用的 recording 混为一谈。[Track 定义](https://musicbrainz.org/doc/Track)。官方示例还展示了 artist-credit 的连接文字和多碟结构，解析时应保留这些信息。[API 示例](https://musicbrainz.org/doc/MusicBrainz_API/Examples)

MusicBrainz 返回 Unicode 文本，正常结果预览不需要继续让用户猜代码页；旧 freedb、本地 ANSI CUE 的编码兼容应留在各自读取层。没有返回的字段不自动清空用户标签。

### 封面与普通文件是后续扩展

选定 release 后，可以通过独立的 Cover Art Archive 接口查找封面。封面有单独请求、重定向和未收录情况，应与文字查询分开处理。[Cover Art Archive API](https://musicbrainz.org/doc/Cover_Art_Archive/API)

缺少 TOC 的普通 MP3/FLAC 不属于原版 freedb 入口。可另做标题、艺人、专辑搜索；音频内容识别还需要另加指纹流程。不能把任意播放列表的曲目时长拼起来，就认为得到原始 CD Disc ID。

## XP 和 Win7 的网络接入

`ttp_https.dll` 的独立源码仓库位于 `D:\Projects\Backup\TTPlayer\mbedtlsmin`。该仓库已提供面向 XP/Win7 的独立 HTTPS 层及证书校验，因而无需仅为 MusicBrainz 更换播放器构建方式。接入 MusicBrainz 时还需要补充以下接口能力：

- `https_client.cpp:419` 固定发送 `User-Agent: TTPlayerRebuild/Lyrics`，没有实际版本和维护者联系地址。
- C ABI 没有请求级 User-Agent/Accept 参数。
- 非 200 响应被转换为错误文字，调用方拿不到结构化 HTTP 状态、响应头及错误响应体。
- 目前响应主要暴露歌词专用 `tt-title`、`tt-url`，没有通用 `Retry-After` 信息。

MusicBrainz 要求可识别应用及维护者的 User-Agent，并限制请求频率；应通过全局队列限制在每秒最多一次，并对临时限流做退避。[请求限流与身份要求](https://musicbrainz.org/doc/MusicBrainz_API/Rate_Limiting)

建议增加向后兼容的新版本 HTTPS 接口，保留旧歌词和更新器布局，补充请求标识、HTTP 状态和必要响应头。然后抽出中立网络适配器，复用现有代理、取消、证书校验，而不是把 MusicBrainz 业务塞进歌词协议代码。

DLL 缺失时的 WinHTTP 回退仍受系统 TLS 能力约束；接入后需分别验证 XP、Win7 的成功、证书失败、代理和取消路径。VC-LTL、YY-Thunks 解决的是运行库/API 兼容，不能替代 HTTPS 传输与证书验证。

## 缓存和保存设计

保留原版 `.cddb` 兼容读取与必要的基本字段写入；新来源信息建议使用独立的缓存记录，以归一化 TOC、MusicBrainz Disc ID、所选 release 和 medium 关联。卷序列号作为兼容定位信息，不能作为唯一可信的网络缓存键。

保存流程建议为：

1. 网络结果先进入候选模型，展示字段差异。
2. 用户确认选定版本、碟号和要更新的字段。
3. 重新校验光盘身份或 CUE 快照；换盘、文件变化时拒绝应用旧结果。
4. CDDB/CUE 以单次事务保存，成功后更新内存及播放列表。
5. 区分未收录、取消、网络错误和本地保存失败，失败不得显示已保存。

并发网络只会更快触发限流。更有效的优化是同一整碟请求合并、正向缓存、有限期的无结果缓存，以及对过期异步结果使用任务代号校验。

## 建议实施顺序

| 顺序 | 工作 | 验收重点 |
| --- | --- | --- |
| 1 | 抽出 DiscLayout、准确帧坐标和 Disc ID 计算 | 官方 TOC 示例、物理 CD 与 CUE 一致性、混合模式边界 |
| 2 | 恢复 CDDB 存储，增加 CUE 整碟事务 | 离线可用、只读失败、快照变化、原始内容保留 |
| 3 | 扩展 HTTPS ABI，接入 MusicBrainz 查询 | User-Agent、状态码、限流、取消、旧系统代理与证书 |
| 4 | 恢复候选选择和预览应用 | 多发行版、多碟、合辑、字段缺省、过期结果 |
| 5 | 接回手动入口，再恢复自动查询 | 原版 CD/CUE 范围、按盘去重、已有信息保护 |
| 6 | 按需增加封面与普通文件检索 | 作为独立扩展，不影响已确认的整碟流程 |

第一版的目标应是完整恢复原入口的整碟信息体验，以 MusicBrainz 替换网络提供方；不需要先重建已经退役的 freedb 服务端。保留 provider 分层，可以日后另接兼容 CDDB 的服务，而不会混淆两种 Disc ID 或缓存格式。

本地离线校验脚本和结果仅位于 `rebuild/tests/freedb_analysis/`，不加入发行包或 Action 测试。当前只有官方示例计算通过；实际光盘、CUE 变体、API 响应、跨系统联网及保存闭环仍属于实施后的验证工作。
