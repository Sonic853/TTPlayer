# CDA 与 MusicBrainz 实施记录

日期：2026-10-06。

本次按照 [freedb 替代建议](FREEDB_MUSICBRAINZ_ANALYSIS.md) 和 [CDA 原版分析](CDA_IMPLEMENTATION_ANALYSIS.md) 完成播放、元数据、在线查询的代码恢复。网络提供方改为 MusicBrainz，本地 CDDB 继续兼容原版 XML。没有实体音频 CD／光驱；用户指定使用虚拟方法，因此验证以实际生产代码连接虚拟设备后端、XP／Win7 虚拟机和公开 TOC 在线查询为主。

## 原版对应关系与本次修复

| 原版位置 | 原版行为 | 本次实现 |
| --- | --- | --- |
| `0047D023`、`004DECA1`、`004DED2D` | TOC 枚举、轨道起止和导入 | `DiscLayout` 供导入、播放、属性及查询共用；检查返回长度、轨号、MSF、lead-out、数据轨 |
| `004E778B`、`004E7E0F` | 开播预读和缓存交付 | 抽出 `cda_source.cpp`；预读不超过 48 扇区，单批不超过 24，失败减小请求；短读按实际完整扇区推进 |
| `004E7F14` | MulDiv 舍入定位 | 定位到最近 CD 扇区并报告实际时间；拒绝终点；失败不破坏原有游标与缓存 |
| `004EF94C` | 句柄失效、媒体变化时恢复 | 特定错误重开后核对完整 TOC 与序列号；同盘恢复，换盘停止 |
| `004E743C` | 属性读取的 CDA 描述后备 | 解析 44 字节 RIFF/CDDA 描述，返回轨号、时长和缓存信息；部分布局不允许生成 Disc ID 或保存 |
| `ttpcomm` 序号 106、通用 decoder | 识别 DTS-CD 并选择解码器 | 接回原探测器与现有 decoder 工厂；缺少解码器时明确失败，阻止已识别压缩数据被当作 PCM 输出 |
| `004DF142`、`004DF1EC`、`004E7675` | 本地 XML 标签读取、合并保存 | 恢复 CDDB 兼容读取，互斥合并、临时文件和原子替换；保存结果返回属性窗口 |
| `0043840A`、`004381B2`、`00438241` | 整盘布局、query/read | MusicBrainz Disc ID → 候选 release/medium → 曲目详情；不沿用 freedb Disc ID |
| `004391F2`、`004394B8`、`004398AA` | 候选、预览、确认和应用 | 工作线程读取，WTL 对话框选择发行版，确认后一次保存 CDDB/CUE，成功再刷新列表 |
| `0047FEA3` | 缺少信息的 CD 自动查询 | 仅播放成功的 CDA、艺人和专辑皆空、开关启用时排队；按盘符和序列号抑制本次会话的重复查询 |
| `004A4E8C` 等通用增益流程 | reader 解码、分析、元数据写回 | 手动 CDA 扫描使用实际解码源，增益／峰值写本地 CDDB；扫描后换盘拒绝提交；移除增益也接回缓存 |

保留 `IOCTL_CDROM_RAW_READ`，没有照搬 Win9x ASPI 或原版 SCSI 传输层。其 `DiskOffset=LBA×2048` 是 Windows 接口约定，输出仍为每扇区 2352 字节的 CD 音频。

## 播放与错误处理

- 普通 CD 为 44.1 kHz、双声道、16 bit PCM；轨道边界和时长从 TOC 取得。
- 调用者请求不是整扇区时，只交付其容量允许的完整 PCM 帧，多余数据保留在缓存。不会超量写入，也不会丢弃未交付的半个扇区。
- 整扇区短读可继续；零字节、非整扇区或返回量大于请求量视为读取失败。
- 1～49 扇区短轨、奇数末尾、48 扇区预读边界均走相同生产实现。
- 设备读取失败逐步缩小批量；无盘、设备未就绪及最终失败会停止，不无限重试。
- 切换光盘时不复用旧 TOC。CDA 仍禁止曲目重叠打开同一光驱。
- DTS 桥接使用 `ttpcomm` 的原格式探测和插件管理器；额外同步头检查用于拒绝无法确认的压缩数据。完整 DTS 解码仍取决于可用的兼容解码器。

## 属性、缓存与可靠保存

缓存路径保持：

```text
<TTPlayerRebuild.exe 目录>/CDDB/<十进制卷序列号>.cddb
```

读取支持旧版本（小于 4）、`TrackNN` 及 `Track ID` 节点，轨道缺少专辑时继承根节点 Album。新文件仍写 UTF-8、`version="3"`，额外保存 TOC 身份与 MusicBrainz 标识；原版可忽略这些附加字段。卷序列号不是 MusicBrainz Disc ID。

保存时先获取路径对应的跨进程互斥锁，重读最新缓存、合并变更，再写临时文件并刷新、替换。多轨更新不会逐个覆盖掉其他轨道。空值删除字段；专辑作为整盘字段传播。只读、损坏 XML、目录权限和替换失败返回实际失败，不伪报成功。

文件属性进程协议升为内部版本 6，携带读取时的光盘身份。保存前重新读取 TOC，身份变化或缺少身份时拒绝写入。CDA 属性保存不再尝试解除光盘文件的只读属性；改动对象是本地缓存。只复制 `.cda` 描述到硬盘只能读取描述信息，不能变成可播放音频或可写的挂载光盘。

ReplayGain 也保留扫描时实际打开的光盘身份，提交前核对。文件属性、在线查询和增益写入均复用同一缓存事务。播放时自动增益扫描的现有 CDA 排除规则未扩大；本次补的是手动扫描和删除缓存标签。

## CUE 与 MusicBrainz

使用原始 75 fps 的 INDEX 01，不先取整到秒。多 FILE 的长度由现有解码源读取（精度取决于对应解码器提供的时长），累计长度和显式 PREGAP／POSTGAP 后形成整碟布局。没有音轨、非连续曲号、重复 FILE、缺少索引、无法取得音频长度、越界索引及数据轨混合布局会报错，不构造假精确 ID。

普通纯音频 CD 的 SHA-1 与 MusicBrainz 特殊 Base64 实现在宿主内部，未增加 libdiscid 运行依赖。官方示例：

```text
tracks:   1..6
offsets:  150 15363 32314 46592 63414 80489
lead-out: 95462
Disc ID:  49HHV7Eb8UKF3aQiNmu1GR8vKTY-
```

实际服务验证后的请求：

```text
/ws/2/discid/<id>?fmt=json&inc=artist-credits+labels&cdstubs=no&toc=<TOC>
/ws/2/release/<id>?fmt=json&inc=recordings+artist-credits+discids+genres
```

discid 查询不接受先前分析草案中的 `media+discids` 包含参数组合，实际返回 HTTP 400，已更正源码和分析文档。

候选按发行版及 medium 列出专辑、艺人、日期、地区、厂牌／目录号与碟号。按匹配的 medium 和曲目 position 分配标签，不取固定 `media[0]`，不跟随当前播放列表排序。保留 artist-credit 的连接文字，曲目标题和艺术家优先使用发行版轨道值。无对应字段时保留用户现有内容。

选定结果一次写回整张 CUE，保留其编码、其他行、索引和换行；修改前检查快照。出现不合法文本或外部改动时整次失败，不产生半张盘已改、半张盘未改的结果。继承的全局 REM 标签可以在单轨用显式空值覆盖，不误清空其他曲目。CUE 内嵌双引号／换行的字段仍会明确拒绝保存。

## 网络、界面与配置

- 播放列表原 FreeDB 命令改为 MusicBrainz，范围仍为 CDA/CUE。
- 网络连接页的对应标签、说明及服务器名称已更新；旧 `FreedbAutoQuery`、`FreedbServer` 配置键保留以迁移设置，退役 freedb 地址转换为默认 HTTPS API 根地址。
- 手动查询始终显示错误；自动查询遵循“自动查询失败时显示错误信息”。本次错误展示使用查询窗口，没有恢复原版右下角通知窗。
- 导入歌曲或复制 CDA 描述不会触发自动查询。查询结果只在点击保存后应用，确认保存前取消不会改标签。
- 请求和详情读取在工作线程执行；关闭会发取消请求，并等工作线程退出后销毁状态。
- 使用可识别应用的 User-Agent。每进程请求队列至少间隔 1.1 秒，缓存最近 64 个响应；正常响应 30 分钟，无匹配 5 分钟。
- 429／503 遵守 Retry-After（秒数或 HTTP 日期）并有限重试；较长等待明确返回繁忙，后续请求仍遵守等待时间。区分无结果、网络失败、取消和本地保存失败。

HTTPS 扩展在独立 `mbedtlsmin` 项目的 `ttp_https.dll` 中。新 ABI 4 增加请求头、HTTP 状态和 Retry-After，ABI 1/2/3 的函数表布局与旧行为保留。MusicBrainz 适配器使用代理、取消和正常证书验证；不会在证书错误后悄悄绕过 DLL。DLL 缺失或明确要求系统回退时使用 WinHTTP，旧系统回退能力仍受系统 TLS 限制。

## 验证方法与结果

本地测试位于 `rebuild/tests/freedb_analysis`，不上传测试源码，不进入发行包，不接入 Actions。测试程序链接实际 `ttplayer_core`，只替换 `CdAudioDevice` 设备边界，生产版继续使用 Windows 光驱实现；没有把模拟器装进系统或改变系统驱动。

覆盖范围：

- 真实 `CreateCdaSource` 的预读、不同请求容量、连续 PCM 内容、全长、EOF、寻址、失败后位置保留。
- 虚拟设备注入整扇区短读、部分扇区、零字节、受限批量、句柄失效、换盘及 DTS 同步头。
- TOC、描述文件、官方 Disc ID、Unicode 缓存、旧 XML、并发合并、只读失败、外部实体拒绝。
- 单／多文件 CUE、75 fps 精度、间隙、快照变化、批量保存原子性及继承标签删除。
- 真实 API JSON 的候选、碟号、曲目映射，错误响应和取消；HTTPS ABI 1～4 的兼容表、头注入拒绝、释放及取消。
- 文件属性 IPC 中读取与保存的光盘身份传递。

最终结果：

| 环境 | 离线／虚拟设备检查 | 真实 Release EXE 属性子进程 | MusicBrainz 查询、详情、404 |
| --- | --- | --- | --- |
| Windows 11 本机 | 986 项通过 | 7 项通过，共 993 项 | 直连通过 |
| Windows XP 虚拟机 | 986 项通过 | 7 项通过，共 993 项 | 临时 CONNECT 转发通过；直连路由失败 |
| Windows 7 虚拟机 | 986 项通过 | 7 项通过，共 993 项 | 直连通过 |

离线计数包含逐次读取的容量／内容断言，不表示有 986 种独立场景。最后新增的 4 项为实际虚拟设备的连续整扇区短读和读完缓冲后的退盘失败；在线回归所用生产代码与最终离线代码相同。

EXE 属性子进程测试验证：复制 CDA 的格式／时长／部分布局身份、拒绝给复制 CDA 写缓存、普通 WAV 属性不回归。首次 VM 部署漏放启动依赖 `ttpcomm.dll`，补齐后通过；发行目录原本已有该依赖。

本地日志：`final-win11-offline.log`、`final-xp-offline.log`、`final-win7-offline.log`；在线分别为 `final-win11-live.log`、`final-xp-live-proxy.log`、`final-win7-live.log`。最终播放器构建日志 `build-player-final6.log`，测试构建日志 `build-tests-final4.log`，均在本地测试目录。

最终播放器为 x86，XP／Win7 静态导入检查通过（20 个 DLL，719 项导入）；更新器也通过。HTTPS DLL 的兼容导入检查通过。真实硬件读取未由这些检查替代。

网络测试使用公开示例 TOC，不读取或上传用户音频。XP 直连因虚拟机路由无法连接目标，随后使用仅允许 `musicbrainz.org:443` 的临时 CONNECT 转发验证成功。TLS 握手及证书验证仍由 XP 内的 DLL 执行，转发器不解密内容，也未修改系统代理或 DNS 设置。转发器测试后关闭。

## 明确保留的验证边界

1. 没有实体光驱，不宣称 USB 光驱、真实退盘、坏盘和驱动错误已通过硬件测试。普通数据 ISO 无法代替音频轨道。
2. 混合模式／多区段盘的精确 MusicBrainz 查询暂拒绝；音频轨播放仍过滤数据轨。需要完整音频 session TOC 才能补齐准确 ID。
3. 已验证 DTS 识别失败和无解码器时拒绝输出；未验证真实 DTS-CD 的完整解码、格式切换和定位。
4. 未增加 CD-TEXT、安全抓轨校验、光驱偏移校正或 Win9x ASPI。同步光驱 IO 的取消延迟取决于驱动返回。
5. 未增加封面下载、普通文件文本搜索或音频指纹识别，这些属于原建议中的后续可选扩展。
6. Windows 10 没有独立测试环境；按此前约定以 Windows 11 作为现代系统参考，不能称为 Windows 10 实机结果。
7. 查询窗口的初始化及异步查询／详情回调已运行；未完成最终窗口外观与所有鼠标交互的人工验收。

## 构建与部署

本次更新 `rebuild/build/Release` 内的播放器和 `AddIn/ttp_https.dll`，没有发布远程版本。两个项目继续独立构建。Actions 会从 HTTPS 独立仓库下载已发布组件；正式发布本次功能时需先发布包含 ABI 4 的 HTTPS DLL，再构建／发布播放器，不能用仍为 ABI 3 的旧组件验证 XP 在线能力。

## 产物与主要文件

- `rebuild/build/Release/TTPlayerRebuild.exe`
- `rebuild/build/Release/AddIn/ttp_https.dll`（来自独立构建）
- `include/ttplayer/audio/disc_media.h`、`src/audio/disc_media.cpp`
- `include/ttplayer/audio/cda_source.h`、`src/audio/cda_source.cpp`
- `include/ttplayer/audio/disc_lookup.h`、`src/audio/disc_lookup.cpp`
- `src/net/http.cpp`、`src/ui/disc_lookup_dialog.cpp`
- `mbedtlsmin/include/ttp_https.h`、`mbedtlsmin/src/https_client.cpp`

来源：[MusicBrainz API](https://musicbrainz.org/doc/MusicBrainz_API)、[Disc ID](https://musicbrainz.org/doc/Disc_ID_Calculation)、[限流规范](https://musicbrainz.org/doc/MusicBrainz_API/Rate_Limiting)、[Microsoft CD 原始读取](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddcdrm/ni-ntddcdrm-ioctl_cdrom_raw_read)。
