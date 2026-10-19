# TTPlayer 5.7.9 与当前重建版：剩余差异、功能缺口和简化建议

日期：2026-10-19。性质：只读审计及私有样本验证。**本文没有实施修复，也没有提交 Git。**

后续已按本报告开展实现。**下文是修复前的历史基线**；当前修复及本次运行结果见 [修复与验收记录](PLAYER_PARITY_FIXES_20261019.md)。不要把下文的缺口列表直接当作当前未完成列表。

Winamp 插件位深的补充问题单独见 [Winamp DSP PCM 位深分析](WINAMP_DSP_PCM_PRECISION_20261019.md)。

## 1. 结论与基线

重建版已经具备完整播放器的主要运行链，不再是只有界面和配置读写的骨架。但**不能认定已经与 5.7.9 完全一致**。本轮发现的问题主要集中在：

1. 独立后台任务／helper 与共享歌曲状态之间没有统一提交和通知协议。
2. 播放与转换重复组织 PCM 阶段，位深、重采样、插件和量化顺序出现偏差。
3. 同一种逻辑曲目在导入、媒体库、普通搜索和文件操作中经过不同的入口检查。
4. 少数原版宿主接口、编辑器交互和旧系统协议分支尚未闭合。

最先应修的是**标签并发保存丢更新**和**歌词选项丢未保存修改**；随后才是音频阶段和功能覆盖。代码简化应直接消除这些分叉，不宜先大规模重写整个窗口或迁移所有逻辑到 ttpcomm。

### 1.1 本轮身份

| 对象 | 版本／身份 |
| --- | --- |
| 原版 `TTPlayer.exe` | 5.7.9.0，PE32 x86，1,882,544 字节 |
| 原版 SHA-256 | `c7999ea5823469c1684148cac1bd196009824348ccac5cdd45978f8fb574e28c` |
| 重建 EXE | `rebuild/build/Release/TTPlayerRebuild.exe`，2026.10.19，4,119,552 字节 |
| 重建 EXE SHA-256 | `7108276a790f965c06420e2b4ae4e302f722f890b053d04663dd40d8a1612b2e` |
| rebuild HEAD | `8d4f354794c9984d1ee474fe405711452140ca02` |
| 当前 ttpcomm.dll SHA-256 | `cf999299ca192d6a865f6ef1fbb28d71d431658da96964ebdbffbd9017202f07` |

审计包含开始时已有的未提交修正：`archive_member.cpp`、`builtin_file_info.cpp`、`player_window_visual.cpp` 及 ttpcomm SDK 镜像。没有把它们回退到 HEAD 再分析。

已排除近期完成的四类修复：`5d6eaaa` 文件关联交接、`6be6e4d` 归档 PCM 提前播放、`ddc5774` 最小化歌词可见性、`8d4f354` ASIO／KS 时序兼容；也没有把 ttpcomm 的 `GAP_FIXES_20261019.md` 已修内容再次列为未完成。

### 1.2 证据等级

- **实证**：当前真实生产函数、真实文件提交或实际属性 helper 通过私有探针复现；不等同于整个播放器 GUI 在所有系统都已回归。
- **静态确认**：已连接原版入口与当前调用链，或确认当前内部矛盾；仍需针对场景做运行验收。
- **部分／待验证**：有明确实现边界，但尚无足够证据认定真实设备或某个插件必然失败。
- **有意差异**：用户选择或现代扩展，不当作还原缺陷。

原版依据为 `reverse/decompiled/TTPlayer.exe.pseudo.c`、函数和调用索引，以及新鲜 REA／Ghidra 分析。伪代码存在寄存器参数和类型恢复误差；关键 PCM 调用以实际指令补证。

## 2. 按优先级整理的差异

P1 为可能丢失用户修改；P2 为功能／兼容／数值行为错误。下列序号用于后续修复验收，不表示所有项目具有同样的验证强度。

| ID | 优先级 | 差异 | 当前证据 | 建议落点 |
| --- | --- | --- | --- | --- |
| G01 | P1 | 属性与增益并发保存覆盖已提交标签 | 真实跨进程写入复现 | 文件修改协调与提交版本检查 |
| G02 | P1 | 更改歌词保存选项会丢未保存时间调整 | 当前核心调用复现 | 按变更种类应用选项，保护歌词草稿 |
| G03 | P2 | 普通属性保存不同步其它列表及主窗歌曲快照 | 真实属性保存复现 | 按歌曲身份发布字段变更 |
| G04 | P2 | 菜单评分与点击评分结果不一致 | 真实命令调用复现 | 同一个评分更新入口 |
| G05 | P2 | 自动输出位深固定退到 16 位 | 当前 PCM 探针及原版新鲜反编译 | 共用源位深解析规则 |
| G06 | P2 | 播放／转换的 DSP、SSRC、量化阶段不一致 | 部分数值实证，完整调用链静态确认 | 统一 PCM 阶段核心 |
| G07 | P2 | EXE 缺少三个原宿主功能导出 | 两个真实 PE 的导出表 | 逐接口补 ABI 和消费者测试 |
| G08 | P2 | XP 非 HTTP 网络协议无条件走 MF | 可达源码链静态确认 | 按协议选择旧 provider |
| G09 | P2 | KS 缺单声道复制及 8 位格式尝试 | 原版与当前格式合约对照 | 输出格式规划和 KS 转换 |
| G10 | P2 | 歌词查找替换的整词检查、选区范围和历史不完整 | 替换实证＋资源／调用链 | 恢复编辑器查找状态模型 |
| G11 | P2 | 归档歌曲的普通搜索跳过元数据补读 | 同一 Artist 的裸文件／ZIP 对照实证 | 逻辑曲目解析入口 |
| G12 | P2 | 媒体库目录不展开 CUE／归档，逻辑成员补读被跳过 | 有效 ZIP 及生产扫描函数实证 | 共用导入收集服务 |
| G13 | P2 | 非当前歌曲的显式歌词关联不进入删除／发送 | 完整调用链静态确认 | 通用歌词关联查询 |
| G14 | P2 | 下载生成 `.bin`，保存副本复用却拒绝它 | 纯离线策略探针复现 | 格式目录和完整副本资格 |
| G15 | P2 | HTTP 转码丢代理设置，取消不传入网络等待 | 参数和取消链静态确认 | 统一只读源配置与取消 |

### 2.1 G01：标签写盘会丢失另一笔已成功提交的修改

私有 MP3 的原标题为 `Original race title`。实际 ReplayGain 写入先读取旧文件并准备临时文件，在真实 `ReplaceFileW` 前暂停；实际属性 worker 进程保存 `User edit wins`，独立重读确认已经落盘；随后继续增益替换。

```text
concurrent_save interim_title=User edit wins
concurrent_save child=1 initial=0 waited=0 user_save=0 gain_save=0 final_read=0
concurrent_save final_title=Original race title
```

两次保存都返回 S_OK。暂停只安排了自然竞争的顺序，没有伪造写入结果。

根因是 [ReplaceWithTags](D:/Projects/Backup/TTPlayer/rebuild/src/audio/builtin_file_info.cpp:1652) 提交完整旧快照，在第 1676 行替换当前路径时没有检查源是否已经改变。原子替换只能避免写一半，不能避免后提交覆盖先提交。增益的进程内 mutex 不覆盖属性 helper。

用户可以在非模态扫描增益窗口工作期间打开属性；自动增益也有独立提交线程，故不是只能由探针构造出的不可达场景。

原版 `004AD25D` 复用歌曲 reader，`004A50B5` 保留元数据接口并更新共享对象，其对象结构不同；本轮**没有证明原版绝不会发生磁盘竞争**，因此将此项定为当前可靠性缺陷。

修复建议：统一标签、封面、内嵌歌词、手动／自动增益的跨进程提交协议。采用稳定路径协调，配合物理文件身份和版本检查；只有标签改变时重新读最新文件并合并字段差量，音频主体已换成另一首时拒绝旧增益。仅用 file ID 命名锁不足，因为原子替换会换 ID；也不能宣称对不参与协议的第三方程序提供了通用原子比较交换。

### 2.2 G02：无关歌词选项触发重载并清除草稿

当前 [ApplyOptionsRuntime](D:/Projects/Backup/TTPlayer/rebuild/src/ui/player_window_options.cpp:6388) 对整个歌词页 256 执行相同的重载分支。只要 `lyric_path_` 不空，就 `LoadLyricsFrom`，重新读磁盘并清掉修改标志。

实际生产函数探针：

```text
before unrelated lyric option: first_time=1500 dirty=1
after unrelated lyric option: first_time=1000 dirty=0
```

只是切换“用行压缩方式保存以减少大小”，此前尚未保存的 500ms 时间调整就被丢弃。原版 `00490276` 中这些复选框修改各自配置，不做同样的普遍文件重载；拖动歌词设置才发相应重绘消息。

修复应把“保存策略”“外观”“自动显示”“搜索／加载规则”分开应用。只有确实需要换文档时才处理保存／保留草稿语义，不应靠每次重载文件刷新所有选项。

### 2.3 G03／G04：按值保存的 Track 缺少统一通知

原版 `0041E974` 对同歌曲查找并复用 `CPlayItem`，媒体库引用同一对象；重建版用按值快照可以保留，但必须发布统一身份变更。

普通属性的 [完成分支](D:/Projects/Backup/TTPlayer/rebuild/src/ui/player_window_playlist_properties.cpp:2304) 只改开始时记录的那一列表／行：

```text
ordinary_properties ... list1=Saved title list2=Old title opened=Old title
```

另一列表、停止状态下的 `opened_track_` 仍旧。以这些快照为数据源的主窗和外部媒体信息也可能继续旧值；本轮没有分别读取每种外部媒体客户端。

普通 [评分菜单](D:/Projects/Backup/TTPlayer/rebuild/src/ui/player_window_playlist.cpp:6531) 又绕过鼠标评分使用的同步辅助函数：

```text
ordinary_menu_rating handled=1 list1=5 list2=1 library=1
direct_click_rating list1=4 list2=4 library=4
```

建议引入 `SongIdentity + MetadataDelta`，更新所有列表、媒体库、打开歌曲快照，并使标题／排序／媒体会话缓存失效。不能只把菜单改为调用现有媒体库同步函数：它在媒体库关闭或没有对应条目时会早退，仍不能覆盖所有场景。

### 2.4 G05／G06：自动位深与 PCM 阶段漂移

原版 `004AB1C4` 在 OutputBits 为 0 时读取 reader 位深，再验证 8／16／24／32。当前 [PcmOutputTransform::Open](D:/Projects/Backup/TTPlayer/rebuild/src/audio/pcm_output_transform.cpp:111) 把 0 直接当未知值，退为 16。探针对 8／16／24／32／64 输入都得到 16；其中非法原始位深回退可以保留，24／32 也一律丢失才是错误。

原版主要阶段：

```text
解码 → RG／EQ／环绕 → SSRC → 必要的16位量化 → Winamp DSP → 设备位宽
```

当前播放：Winamp DSP 在 SSRC 前；当前离线转换：先 SSRC 再 RG／EQ／环绕。顺序改变会影响依赖采样率的滤波、非线性限幅、状态和尾部。

本轮探针记录 `DSP bits=16 rate=44100 final_bits=24 final_rate=48000`；当前插件前手写 `llround` 与已恢复量化器在 ±0.5、±2.5 LSB 处有 1 LSB 差异。当前 dither 又主要在后面的最终编码执行，原版是在插件前按设置选择量化方式。

源码落点：[播放插件桥](D:/Projects/Backup/TTPlayer/rebuild/src/audio/audio_engine.cpp:1890)、[离线转换阶段](D:/Projects/Backup/TTPlayer/rebuild/src/ui/playlist_transforms.cpp:1724)。原版依据：`004AC166 → 004B1375 → 004B1BF4`，量化 `004AA360 / 004AAFAF`。

这四个问题应作为同一小范围音频阶段重整来修复，复用 ttpcomm 已有数学实现。高于 16 位的 Winamp 插件扩展属于下一步能力，不是“原版已经高位深、重建版漏做”的结论。

### 2.5 G07：宿主导出只恢复了两个

直接读取两个 PE 的导出表：

| 序号 | 原版导出 | 当前重建 EXE |
| ---: | --- | --- |
| 1 | CreateSoundBuffer | 缺少 |
| 2 | CreateStdContent | 已有 |
| 3 | CreateStreamOnFile | 已有 |
| 4 | CreateStreamOnInet | 缺少 |
| 5 | GetSoundCodecName | 缺少 |
| 6 | _set_security_error_handler | 缺少，旧 CRT 私有兼容项需另评估 |

当前 [exports.cpp](D:/Projects/Backup/TTPlayer/rebuild/src/app/exports.cpp:1) 与 PE 一致。原版地址分别为 `004C5171`、`004DA091`、`004C51D3`、`004C5269`、`004C4FD1`、`00509736`。

本地原 AddIn 中存在动态查询这些名字的字符串，例如 ASF／RM 同时出现 `CreateSoundBuffer` 和 `GetSoundCodecName`。**字符串存在不证明每次运行必调用，也不证明插件必失败**；当前替代读源路径还可能掩盖缺口。

建议逐个确认调用约定、返回对象、内存所有权和失败值，写消费者验证后补前三个功能导出。不要把导出一个空函数当 ABI 还原，也不要为了导出表相同盲目复刻旧 CRT 的安全错误处理。

### 2.6 G08／G09：旧协议和 KS 格式仍不完整

**XP 网络协议**：[OpenSource](D:/Projects/Backup/TTPlayer/rebuild/src/audio/audio_engine.cpp:1397) 对非 HTTP／HTTPS 的 URL 无条件构造 Media Foundation 源。XP 缺 MF 时在连接前就失败；原版 `004E8AC3 / 004E8BD6` 有 MMS source filter、代理和 DirectShow 加载分支。应按协议恢复 provider，不能只把所有 URL 强行改成另一个构造器。本轮没有有效 MMS 服务器，不能承诺已停服端点恢复。

**KS 格式**：[格式校验](D:/Projects/Backup/TTPlayer/rebuild/src/audio/kernel_streaming_sink.cpp:118) 只接受 16／24／32 位，格式尝试也不做 mono→stereo。原版 `004AB1C4 / 004AC2B1` 对相关输出先复制单声道；`004E029E` 会先尝试请求位深，而不是提前拒绝 8 位。只接受双声道的 KS pin 和支持 8 位的设备因此有兼容缺口。此为静态合约差异，本轮未接真实 KS 硬件。

最新 ASIO 的单声道扩展已经实现，不重复列缺项；原 ASIO 自身也不支持低于 16 位，不能把 KS 的 8 位要求照搬到 ASIO。

### 2.7 G10：歌词编辑查找／替换仍不是原版的完整交互

当前 [HandleLyricFindRequest](D:/Projects/Backup/TTPlayer/rebuild/src/ui/player_window_lyrics.cpp:3657) 的查找调用传 `FR_WHOLEWORD`，但“替换当前选中文字”的检查只比较文字，未检查词边界。

实证：文本 `catalog cat`，选中第一个单词里的 `cat`，启用全字匹配后替换为 `dog`，结果为 `dogalog cat`。

另外，原资源 202 和 `0044E2C9 / 0044660F / 004467CC / 00446CCB` 包含当前选区／全文范围、组合框历史及同窗口切换；重建版调用系统 FindTextW／ReplaceTextW，缺少相应控件和范围模型，ReplaceAll 固定从文档开头扫描。

这是**歌词编辑器**的资源 202，不要误当成已经恢复的播放列表查找资源 203。

### 2.8 G11／G12：逻辑曲目入口分叉导致漏功能

普通列表搜索在 [元数据补读](D:/Projects/Backup/TTPlayer/rebuild/src/ui/player_window_playlist.cpp:5376) 前先 `GetFileAttributesW(track.path)`。对于 `archive.zip|member.wav`，逻辑路径不是 Windows 文件，必然被跳过。

同样的 Artist，本轮实际 reader 可以从 ZIP 成员读取，裸 WAV 普通搜索找到，ZIP 成员搜索找不到并仍保留 `duration=-2`。reader 的 S_FALSE 前缀结果也要按现有策略处理，不能永久缓存成已验证完整元数据。原版搜索 `0048287B → 004AE7FD → 004ADA76` 可以进入 ZIP／RAR 逻辑路径。

媒体库 [扩展收集及补读](D:/Projects/Backup/TTPlayer/rebuild/src/ui/player_window_library.cpp:782) 又过滤 CUE／归档，并对 `|` 和非零 subtrack 直接返回。有效 ZIP 的生产扫描探针得到 0 项。原版新增受监视目录的路径 `004833B6 → 0047444A → 004742DD` 可以分类并分派 CUE／ZIP／RAR；这不等于原版每一种单文件通知都无条件展开。

建议共用有预算、取消、密码请求和只读策略的导入收集／逻辑曲目解析服务。快速定位的“只查已加载缓存”是有意语义，不能为修普通搜索而把所有鼠标／键盘定位都改成同步解码。

### 2.9 G13：持久歌词关联已存在，但两个文件操作入口还在猜文件名

当前 [歌词文件收集辅助代码](D:/Projects/Backup/TTPlayer/rebuild/src/ui/player_window_playlist.cpp:108) 仍保留“没有持久 map”的旧假设；实际上 `lyric_associations_` 已有加载和保存。

第 6102 行的删除和第 6626 行的发送只为当前播放行提供显式关联。给 A 关联另一个目录下的非同名 LRC，切到 B 后再操作 A，就不会使用 A 的真实关联。原版 `00485C92` 会逐首查询所选歌曲的歌词路径。

此项静态确认；本轮没有执行真实删除／发送。修复时先用私有文件验证计划中的操作集合，处理“不关联歌词”哨兵、CUE 子轨和只读归档，不扩大成压缩包写回。

### 2.10 G14／G15：网络调用方之间的策略不一致

**保存副本**：[http_download_queue.cpp](D:/Projects/Backup/TTPlayer/rebuild/src/audio/http_download_queue.cpp:129) 的私有扩展表漏掉部分已支持音频。`.aiff` 或无扩展 URL 生成 `.bin`，随后保存副本资格检查又拒绝 `.bin`。本轮离线探针证明同一实现内部不一致；另有有效流缓存时仍可能正常播放，不能笼统说断网必失败。

**转码**：[ConvertPlaylistTrack](D:/Projects/Backup/TTPlayer/rebuild/src/ui/playlist_transforms.cpp:1711) 新建默认 PlaybackOptions，只设文件缓冲，遗漏用户代理／缓存设置，也没把 stop_token 接到网络取消。依赖自定义代理的曲目可以播放却不能按同配置转码；Open／Read 阻塞期间取消最多要等网络超时。原版转换用公共读源链，但原 HTTP 取消粒度尚未在本轮做动态比较。

建议共用 SourceOptions／NetworkProfile；副本可用性以完整性和允许根目录为基础，格式扩展名不能同时充当唯一完整性证明。

## 3. 尚未完全恢复、但不能直接报成“新 BUG”的部分

### 3.1 明确的部分实现／待补范围

| 范围 | 当前状态 | 下一步所需证据 |
| --- | --- | --- |
| KS topology 硬件音量 | 当前使用包提交前软件音量，尚未恢复原 `004E061B` 的 volume-node 控制；部分淡入淡出刻意关闭 | 真实设备 node／通道验证，缺 node 时明确软件策略 |
| 旧宿主外部 ABI | G07 三个功能导出缺少；不影响所有新内置路径，但不等于旧 AddIn 全兼容 | 逐消费者初始化、对象接口和失败路径 |
| 歌词编辑查找／替换 | G10 缺范围、历史和严格整词替换 | 原资源 202 的交互对照 |
| 媒体库逻辑曲目 | 普通导入支持不代表目录监视／入库补读也支持 | G11／G12 的同样本多入口矩阵 |
| 旧网络协议 | HTTP 已丰富恢复；MMS 等另有 G08 | 受控服务端和 XP provider |
| 原版状态迁移 | 主 XML 有迁移，`.rll` 仍随 EXE 基名，未自动导入原版关联 | 明确导入政策，不覆盖已有新文件 |
| 整体皮肤等价性 | 已有多个阶段恢复和控制样本；不能从某几个皮肤通过推导全部 XML／图片／DPI 组合一致 | 版本化皮肤矩阵，极端布局／绘制／capture 边界 |

### 3.2 实测边界，不冒充未实现功能

- ASIO／KS 已有模拟驱动和既往旧系统测试；本轮未用实体声卡验证驱动占用、时钟、真实延迟及全部 topology。
- CDA 已有 TOC、扇区偏移、短读缓存、换盘身份、DTS 和在线查询路径；没有实体音频 CD／光驱，虚拟扇区通过不等同于坏盘和真实光驱全部通过。
- 本轮没有对所有 codec、所有损坏文件、所有插件预设进行穷尽比较。
- 本轮没有新跑 XP／Win7。Win11 代表用户指定的现代系统验证环境，不能写成实际执行过 Win10。

### 3.3 用户指定的有意差异

- 音乐窗暂不恢复，其完整页面和在线服务流程不列入当前修复承诺。
- 压缩包保持只读，不采用写回；这是约束，不是欠缺的保存接口。
- 随机索引的三份轮换、超过 5000 首或 XP／Win7 单份循环是用户指定设计，不恢复成旧算法覆盖它。
- WASAPI、SMTC、Discord、逐字歌词、Opus／WebM／OSZ、现代 HTTPS／MusicBrainz 等属于扩展能力。
- Winamp 高位深按插件适配属于扩展；原版正常调用同样为 16 位。
- Winamp 变长 DSP 返回帧数当前被忽略，与原版原地处理策略一致；若支持变速插件需另立队列和时间映射设计。
- MIDI 在原版本来就有独立非 PCM 播放路径，不经过所选 WaveOut／内置 EQ 链不能直接判漏实现。

## 4. 哪些重复逻辑值得简化

本轮 `src`／`include` 清单包含 260 个 C／C++ 源头文件，100,112 个物理行（含注释和空白）。最大的几个文件是 `player_window.cpp` 8,175 行、`player_window_options.cpp` 8,074 行、`player_window_playlist.cpp` 6,948 行、`player_window_lyrics.cpp` 4,742 行。

行数与 EXE 体积只能描述规模，不能证明实现臃余。以下建议基于已经发生的行为分叉。

| 可合并内容 | 当前分叉／收益 | 建议位置 | 应保留的边界 |
| --- | --- | --- | --- |
| 文件提交事务 | 普通标签／封面／歌词／增益各自提交旧快照，G01 | 无 UI 的身份／版本／替换基础可放 ttpcomm；字段合并留宿主 | 只读、取消、外部改文件、真实提交结果 |
| 歌曲身份更新 | 属性、评分、重命名、增益各自更新若干副本，G03／G04 | rebuild 的歌曲模型服务 | 媒体库关闭也能同步，避免过期 row |
| PCM 阶段与输出格式规划 | 播放／转换重复 RG、EQ、环绕、SSRC、量化，G05／G06 | 宿主公共音频管线；数学算法继续在 ttpcomm | 设备时钟、实时队列、离线取消仍分开 |
| 逻辑曲目解析／导入收集 | 文件／CUE／归档在不同调用方被扩展表和物理路径判断拦截，G11／G12 | rebuild 媒体服务；容器解码留 ttpcomm | 密码交互只在授权 UI；预算和归档只读 |
| 歌词关联解析 | 播放已用持久 map，文件操作仍猜同名，G13 | rebuild 歌词服务 | “不关联”哨兵、非当前歌曲、子轨 |
| 输出设备探测异步封装 | 已隔离子进程，但 UI 仍同步等待，见下文 | 宿主公共 helper 客户端 | 进程隔离不能为了少代码移除 |
| ID3 容器／封面读取服务 | 标签属性和预览分别组织文件读取、预算和封面选择 | ttpcomm 或宿主共享只读服务 | 首图／全部标签、预算策略由调用方指定 |
| HTTP provider 加载和请求配置 | 音频、查询、更新各有加载／代理／取消包装 | 公共 provider／会话层 | Range／ICY、更新校验、JSON 限额保持独立 |
| JSON 工具层位置 | CD 查询、插件 registry 已引用 `../update/json.h` | 移到通用 core／公共 SDK | 已共用一个解析器，不是要再写一个 |

### 4.1 输出页“有子进程”不等于 UI 不阻塞

[RunOutputDeviceProbe](D:/Projects/Backup/TTPlayer/rebuild/src/ui/player_window_options.cpp:610) 建立受控进程后，在第 663 行同步等待最长 4 秒，失败清理还可再等 1 秒。第 5024 行构建页面及第 6550 行选设备详情会调用它。

原版也有同步枚举，因此这是当前体验和结构改进，不应包装成已经证明的原版回归。可改为异步目录／详情请求，带页面代次号；过期结果不更新已关闭／切换的页面。复用创建 Job、超时、退出码和结果协议，不合并互不相同的任务状态。

### 4.2 不要为了“并入 ttpcomm”而搬错层

适合 ttpcomm 的是无窗口依赖的 PCM、容器、编码、标签、文件身份／安全提交原语。歌曲列表生命周期、选项草稿、歌词窗口显示状态、密码提示、SMTC 通知和 UI 线程调度仍应在播放器。

现有 `core/text` 已经是 ttpcomm 的薄包装，ID3 底层帧解析也已经共享，不能再说完全重复实现。主窗口分成多个 `.cpp` 仍共享大量状态，真正简化应是按责任拆小对象，并明确输入／输出和拥有者，而不只是继续分文件。

### 4.3 网络／XML 不宜机械合并

`src/net/http.cpp:19` 面向小型 HTTPS 查询；`src/update/update_http.cpp:49` 有流式更新下载、来源和完整性策略；音频还有 Range、续传、ICY 及多消费者缓存。可以共用 provider 加载、代理快照和取消，不能把三个业务硬改成同一组宽松规则。

皮肤 XML、配置 XML、服务器 XML 和 TTBL 也具有不同的兼容及未知字段保留要求。应共用基础解析安全边界，保留各自语义，避免“统一”后改变旧皮肤容错。

### 4.4 可以小步清理的状态和旧路径

- `player_window.h:1032` 的 `fullscreen_lyric_window_was_visible_` 只在 `player_window_visual.cpp:2203` 赋值，没有生产读取；恢复窗口已按最新歌词显示意图执行，可以移除死快照。另一个 desktop 快照只在赋值后立即判断，之后清零，可以缩成局部值。实际用于恢复主窗的 iconic／visible 状态仍需保留。
- 全屏菜单的播放状态门控和三种模式勾选，在 `player_window.cpp:5961`、`player_window_lyrics.cpp:3995`、`player_window_visual.cpp:2296` 重复。可共用更新状态的 helper，保留各自菜单结构、删项顺序和命令宿主。
- `lyric_runtime_policy.h:20` 的 `BuildLocalLyricCandidates`、`skin_package.cpp:237/330` 的两个 `ExtractTo` 没有生产调用。真正路径分别是异步歌词搜索和 `ExtractEntryTo`。应检查测试消费者后废弃或移为测试辅助，避免测试验证已经不用的替代实现。
- 搜索的字段合并逐个同名键 erase／append，而普通信息队列保留多值包、显示采用首值；共享元数据适配时应统一。此为静态实现差异，本轮没有多值 Reader 的额外动态用例，不列为已复现故障。

## 5. 已完成覆盖和旧文档需要纠正的说法

本轮核对当前代码后，以下不再列为“只有设置、没有实现”：

- HTTP 共用缓存任务、前台／后台消费者、预取、Range、部分缓存续传、旧 cachelist／任务导入。
- 在线歌词搜索 DLL、HTTPS、XML 服务器列表和旧播放器兼容接入。
- 普通／桌面／迷你歌词、托盘、最小化显示协调及先前补齐的鼠标交互。
- ZIP／RAR／新增归档、密码、内容签名、CUE、OSZ 音乐／背景读取和只读策略。
- 文件属性读写、封面、标签优先级、错误后缀提示、增益扫描及多任务扫描主体。
- 内置效果、Winamp 插件生命周期和文件型注册表兼容层。
- DirectSound／WaveOut／ASIO／KS，以及新增 WASAPI 共享／独占主体。

这表示有真实实现入口，不等于每个角落都与原版完全相同。G01–G15 正是对这些已有功能的跨入口、组合和失败边界检查。

例如 `MAJOR_FEATURE_RECOVERY.md:195` 把在线歌词、网络缓存／下载等仍写为仅序列化字段，已经过时；后续文档整理应给旧阶段报告加基线／被替代提示，并用当前问题清单维护状态，不继续沿用旧“未实现”列表。

### 5.1 原版状态与重建版状态的共享政策

`TTPlayerRebuild.xml` 有旧 XML 迁移；歌词关联仍按实际 EXE 名生成 `.rll`。没有新 RLL 时导入原版关联可改善迁移，但已有新文件不能静默覆盖。

当前重建版和原版使用不同单实例对象，因此可以并行；同目录又会共用 PlayList、Music.library 和部分皮肤／插件配置。本轮没有用用户数据复现覆盖，因此它是需要明确的数据共享政策，不能直接写成已发生损坏。原版全局实例名也不是按安装目录隔离，“不同重建目录只能运行一个”本身不是原版差异。

## 6. 建议实施顺序与验收标准

### 第一批：保护用户修改

1. **G01** 文件提交协调：属性 helper 与自动／手动增益按两种提交顺序并发，最终保留双方字段；外部替换音频主体不得写入旧增益。
2. **G02** 歌词草稿：未保存时间调整后切换保存策略、自动显示、颜色和布局，修改与 dirty 状态不丢；确实换文档时遵守原保存策略。
3. **G03／G04** 身份通知：同文件跨列表、入库／未入库、媒体库关闭、停止／播放状态，标题和评分全部同步。

### 第二批：统一音频格式与阶段

4. **G05／G06** 自动位深、RG／EQ／环绕／SSRC／Winamp 桥、量化和尾部一次整理；采用当前已验证 ttpcomm 算法。
5. 对同一输入分块、seek／reset、半 LSB、各 dither 模式、44.1↔48k 做数值对照；先无 DSP，再模拟 DSP，再已知真实插件。
6. **G09** KS 单声道和 8 位合约，先模拟只接受特定格式的 pin，再实体设备。

### 第三批：补齐已有功能的调用入口

7. **G11／G12** 普通搜索、目录监视、媒体库元数据共用逻辑曲目入口；相同 CUE／ZIP／RAR 样本逐入口验证，扩展归档另列。
8. **G10／G13** 歌词查找范围与整词替换、任意歌曲显式关联的文件操作；删除／发送先用私有文件清单验收。
9. **G14／G15** 下载副本复用和 HTTP 转码代理／取消，用受控 HTTP 服务及阻塞／断流／认证场景验证。

### 第四批：旧宿主和旧系统兼容

10. **G07** 逐个恢复功能导出及对象合约，测试真正调用它的旧 AddIn；不能仅用 GetProcAddress 非空验收。
11. **G08** 无 MF 的 XP 协议选择、代理、取消和可用服务端；KS topology 音量在有硬件证据后单独做。

### 第五批：结构与文档整理

12. 异步输出设备页、公共 helper 客户端、格式目录、provider 配置和 JSON 工具层位置。
13. 整理当前问题清单、旧报告基线和实测矩阵，再决定 RLL 导入／新旧同目录共用状态的产品政策。
14. 高位深 Winamp DSP 以 DLL 哈希／模块／格式白名单单独扩展；不与“原版 16 位路径修正”混在同一次无边界修改中。

每批局部通过后再分别做 XP／Win7／Win11 的对应回归。无需为只改文档或无关页面重复完整媒体矩阵；也不能把旧跨系统记录直接充作新修复的通过证据。

## 7. 本轮实际完成的验证

| 验证 | 结果与范围 |
| --- | --- |
| 生产源码与二进制身份 | 冻结 260 个源头文件及两个 EXE 的哈希，最终检查未被本轮修改 |
| 原版 PCM 函数 | 新鲜 REA 分析 `004AC166`、`004B1BF4`、`004AB1C4`，保留指令与 Evidence ID |
| 音频微型探针 | 自动位深、DSP rate／bits、半 LSB、下载副本策略复现；未打开真实设备 |
| 数据微型探针 | 属性同步、评分、媒体库入口；真实子进程标签写入冲突复现；仅私有样本 |
| UI 微型探针 | 全字替换、未保存歌词丢失、归档搜索遗漏复现；不播放用户音频 |
| 本地真实插件静态分析 | Dolby／VAPXP 位深门槛、DFX 转换分支、Ozone 多字节 PCM；不是高位深运行通过 |
| XP／Win7／实际 Win10／实体 CD／声卡 | 本轮未执行；不得标记通过 |

完整私有证据在 `rebuild/tests/player_gap_audit_20261019/`：

- `inventory.json`、`evidence-index.json`：版本、源文件哈希、REA 身份和日志摘要。
- `audio/AUDIO_GAP_AUDIT.md`、`audio/probe.log`：音频 8 个细分问题和合约结果。
- `data/association_media/AUDIT.md`、`run-cross-process-fresh.log`：真实文件保存和属性 worker 证据。
- `ui/ui-gap-probe-expanded.log`、`ui/ui-gap-probe-result.json`：最终成功的 UI 探针。
- `ui/ui-audit.md`：原版资源 202、皮肤／命中／窗口覆盖边界及死状态核对。
- `rea_*.json`、插件指令文件和 `dfx-converters.txt`：新鲜原版／插件证据。

早期私有夹具错误和修正记录保留在测试目录，不作为生产缺陷；最终结论采用成功完成且条件明确的记录。所有测试代码继续留在 `rebuild/tests`，不上传、不加入 Action。

**完成的是当前基线的差异审计与建议，不是宣称 5.7.9 全部函数已经逐一动态等价验证。**
