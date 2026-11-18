# mp3PRO 解码／编码链路：REA 与当前源码核对

分析日期：2026-11-17。范围：`rebuild` 当前宿主源码、独立 `ttp_enc` 工程，以及工作区已有的 mp3PRO 二进制与样本。

## 1. 结论与证据边界

1. **重建的是适配层，不是 mp3PRO 算法内核。** 解码使用 `mp3PRO.dll` 的 Winamp 输入 ABI；编码使用 `ttp_enc.dll` 对 `fhtpro.flt` 的 CoolEdit 滤镜 ABI 适配。两者不是同一 DLL，也不能互相代替。
2. **普通 MPEG 解码先成功，才有机会启用增强解码。** 重建宿主只把特定本地 MPEG-2 Layer III 送入可选增强路径，不会因后缀为 `.mp3pro` 而强制使用 DLL。
3. **编码器类别名不等于码流带 SBR。** 已有真实滤镜样本中，裸滤镜默认配置输出普通 MPEG-1／44.1 kHz／128 kbps；Pro 96 配置输出 MPEG-2／22.05 kHz／96 kbps 基础层，历史滤镜格式回读报告 44.1 kHz、Layer-3 Pro。重建适配器另有自己的默认 Pro 预设，不能把两种“默认”混同。
4. **已有真实编码样本，不等于已有端到端增强解码验收。** 当前找到的历史记录覆盖真实滤镜编码、格式回读、FFmpeg 基础层解码、mock 增强桥和真实输入 DLL 的普通 MPEG 回退；未找到真实 Pro 输出经 `mp3PRO.dll`、明确断言增强路由、全量 PCM／EOF／seek 的完整闭环记录。

本轮进行了 REA/Ghidra 静态反编译、接口表／指令／导入导出检查，以及源码和样本字节只读核对。**没有执行目标 DLL、播放器或既有测试，没有修改播放器配置、注册表或原始二进制。** 文中历史测试结果不冒充本轮实测。

证据分类：

- **S**：当前源码直接可见的行为。
- **B**：本轮 REA 对指定哈希二进制得到的静态观察。
- **H**：已有测试、日志或样本；需保留其当时的运行边界。
- **I/U**：算法解释／风险推断，或尚未验证的行为。

REA 原始 Evidence／Unknown 保存位置：`../artifact/mp3pro_chain_rea_20261117.evidence.json`。源码定位不自动证明任意同名 EXE/DLL 都由该源码构建。

## 2. 分析对象与身份

| 对象 | 路径（相对工作区根） | SHA-256 | 备注 |
| --- | --- | --- | --- |
| 输入增强 DLL | `mp3PRO.dll` | `7438aaad6a406d459bacbd79e9129a50f7d72e824d304d45cfeccff6517abb25` | x86 PE，294,912 字节；字符串为 THOMSON mp3PRO Decoder v1.3 |
| 原始编码滤镜 | `rebuild/tests/mp3pro_encoder_analysis/fhtpro.flt` | `1dc878b79b2a6729a0cc773731028ba49721dd6a70902963cf8bb21780388827` | x86 PE，237,568 字节；UPX0/UPX1 与 UPX! 标记 |
| 既有解包分析副本 | `rebuild/tests/mp3pro_encoder_analysis/fhtpro.unpacked.flt` | `3e2505340fd2906b6696b7fc36677f89c2056a1e71d0ba3e6d74a1aed293e166` | x86 PE，872,448 字节；内部编码函数分析来自该副本 |
| 当前重建编码插件 | `ttp_enc/build/Release/ttp_enc.dll` | `0cb48acc27fd11f40971bd201ed87adecc7243e45c068435fcc421a6db012599` | x86 PE，708,608 字节，2026.11.13p3；本轮直接用 REA 核对 |
| 较旧已部署重建插件 | `rebuild/build/Release/AddIn/ttp_enc.dll` | `96b59e438a1b5edddd8b5bc480bed0ea94b4fe9ec2e55c9f5ca120475a91dc3f` | 309,760 字节，2026.11.10；不是最新 p3 |
| 根目录旧编码适配器 | `AddIn/ttp_enc.dll` | `c0f116ea6c27f4a5f5836ca1a425642f5cb301ed47f0f24f2dbad9016556b8ff` | 163,976 字节；不是据文件名就能认定为当前重建插件 |

`rebuild` HEAD 为 `46503402f1bbba677fb01045ceaec750f849b2a5`。检查前已有修改／未跟踪内容，未回滚或覆盖。

`ttp_enc` HEAD 为 `408925a7b03e49fac0f84e835f14c09fb6eda0fc`。根 AddIn、最新插件 Release 目录、rebuild Release/AddIn 三处均没有同目录 `fhtpro.flt`。**当前重建适配器的同目录发现条件未满足**，不能由项目中存在测试滤镜就认定运行目录已启用编码。原版适配器可能另走旧的注册表发现方式；本轮没有重新检查系统安装注册状态。

**解包副本限制：** 本轮未重新执行解包，也未证明原始压缩文件与既有解包副本的全部区段逐字节对应。原始文件的导出目录是直接 B 证据；副本内部函数的观察只直接绑定副本哈希，不把它们伪装成原始压缩文件的直接反编译结果。

## 3. 解码：从文件到播放器 PCM

### 3.1 总体调用链

```text
CreateDecodedAudioSource / RecoveredFileSource::Open
  ├─ 优先尝试已注册 Sound AddIn
  └─ 本地 MPEG 后备
       WrapMp3ProSource(system_source())
         ├─ fallback->Open
         │    ├─ 有 MF：MediaFoundationSource
         │    └─ 无 MF：Legacy Windows Source
         ├─ MPEG 类型 + 相邻帧头准入
         ├─ EXE 同目录 mp3PRO.dll
         │    winampGetInModule2 → In_Module
         │    安装宿主回调 → Play(ANSI 路径) → Pause
         ├─ Out.Open 的 PCM 采样率不同于基础 MPEG 采样率？
         │    ├─ 否：Stop，保留普通系统解码
         │    └─ 是：建立环形 PCM 桥，Seek(0)
         └─ DLL 生产线程 → Out.Write → ring → Mp3ProSource::Read
              → RecoveredProcessorChain
              → PcmOutputTransform / 可选宿主 Winamp DSP
              → waveOut / DirectSound / 其他已配置输出后端
```

源码：`src/audio/audio_engine.cpp:1290–1432`、`:2064–2107`、`:2773–2828`、`:3483–3598`；`src/audio/mp3pro_source.cpp:321–415`。

**它不是 packet Decoder。** ASF 内的 MP3、独立 MPEG packet Decoder、HTTP MPEG 流和这个文件名式 Winamp 输入增强层不能混为一谈。

### 3.2 准入条件与路由优先级

`Mp3ProSource::Open`（`src/audio/mp3pro_source.cpp:321–343`）的实际顺序：

1. 普通后备 `Open` 必须成功；失败不能靠增强 DLL 救回。
2. 普通 Reader 的 `DisplayFormat.format_tag` 必须是 `WAVE_FORMAT_MPEG` 或 `WAVE_FORMAT_MPEGLAYER3`。
3. `ProbeHeader` 跳过合法 ID3v2 头／v2.4 footer，读取之后最多 2 MiB，查找两个相邻、MPEG 版本和采样率一致的 Layer III 帧（`:27–81`）。
4. 首帧必须是 **MPEG-2，Layer III，基础采样率 < 32000 Hz**。MPEG-1、MPEG-2.5 和 Layer II 不进入增强层。
5. 文件名必须能无损转换为系统 ANSI 编码，或能取得可表示的短路径（`:84–109`）。
6. 进程内不能已有另一个 reader 占用输入 DLL。

因此：

- `.mp3` 可以走增强；`.mp3pro` 不能保证增强。
- 已成功打开文件的 Sound AddIn 优先，不会再包装为此增强 Reader。
- URL 不使用这套 ANSI 文件名接口；压缩包成员伪路径也不能当作真实本地文件交给它。
- CUE 的内层本地音频可复用增强来源，但这不是样本精确定位保证。

### 3.3 加载与 ABI

`LoadDecoder`（`:290–312`）只尝试当前 EXE 目录的 `mp3PRO.dll`，加载成功或失败均缓存；有效 DLL 保留到进程退出。依赖搜索限制为 DLL 目录和系统目录。不会调用 Winamp `Init`、`Quit`、`Config`。

| 输入 ABI 字段 | x86 偏移 | 作用 |
| --- | --- | --- |
| `version` | `+0x00` | 要求 `0x100`，是接口布局号，不是文件版本 1.3 |
| `Play` | `+0x38` | 接收 `char*` 文件名 |
| `Pause / UnPause` | `+0x3c / +0x40` | 控制 DLL 生产状态 |
| `Stop` | `+0x48` | 停止／回收生产线程 |
| `GetLength` | `+0x4c` | 时长 |
| `SetOutputTime` | `+0x54` | 异步 seek 请求 |
| `output` | `+0x94` | 指向宿主伪装的 Out_Module |

`In_Module` 共 `0x98` 字节，`Out_Module` 共 `0x50` 字节，全部为 **cdecl**；见 `src/audio/winamp_input_abi.h:8–82`。

B：`winampGetInModule2@0x10002120` 返回 `0x10033288` 静态接口表；REA 读取该表完整 152 字节，得到版本、函数指针和回调空槽。DLL 不是 `ttpGetSoundAddIn` 编码／解码插件。

`InstallCallbacks`（`mp3pro_source.cpp:251–270`）设置可视化为空操作、插件 DSP 为不启用／恒等操作，并接入 Out_Module。**真正的音效／可视化由重建宿主后续处理，避免在 DLL 内再处理一次。**

### 3.4 增强识别与输出格式

宿主调用 `Play → Pause`，等待 Out.Open 最多 500 ms。只有以下条件成立才采用增强 PCM：

```text
Play 成功，Pause 成功，PCM 格式合法，输出采样率非零，
且输出采样率 != MPEG 基础采样率
```

这里**不是要求恰好两倍**，也不是宿主直接解析 SBR 标志（`:339–364`）。

- Out.Open 验证采样率 `1..384000`、声道 `1..8`、位深 8/16/24/32；接收开始后拒绝变格式（`:133–154`）。
- 实际输入 DLL 的输出打开函数 `0x10002e40` 向 Out.Open 传入 16 位 PCM（B）。
- 展示格式使用增强 PCM 的采样率／声道／位深，但保留普通 Reader 的压缩平均码率，名称为 `mp3PRO Audio`（`:365–374`）。
- metadata／ReplayGain／封面仍由普通 Reader 和宿主内置标签读取提供，不依赖 Winamp 插件标题回调（`:468`；`audio_engine.cpp:1343–1370`）。

初始化失败、没有升采样、缺 DLL／导出／回调等，退回已打开的普通解码。**启用之后的 Read／seek 超时则报错，不是在歌曲中途无缝切换回另一个采样率的普通 Decoder。**

### 3.5 推式 DLL 到拉式 Reader 的缓冲桥

DLL 在自己的线程推送 PCM；播放器／转换引擎通过 `Read` 拉取。`Bridge` 以 mutex、condition_variable 和环形缓冲连接两端。

缓冲容量以替换前的基础 PCM 计算：

```text
C = max(0x48000, 2 × base.nAvgBytesPerSec + 576 × base.nBlockAlign)
再按增强输出 nBlockAlign 向上对齐
```

`0x48000` 是 **294,912 字节**，不是 48,000。基础 22.05 kHz／双声道／16 位时，公式选择该最小值；以 44.1 kHz／双声道／16 位输出衡量约为 1.67 秒缓冲（`:352–360`）。

- `Out.CanWrite` 报告剩余容量；seek／stop 时提供推进旧 producer 到状态轮询的特殊返回值（`:188–195`）。
- `Out.Write` 验证 PCM 帧对齐、支持环形回绕，满时等待；seek／stop 时丢弃旧块／解除等待（`:156–185`）。
- `Read` 按帧对齐，必要时临时恢复 producer，等数据／EOF 最多 1 秒；取走数据后通知 producer（`:386–415`）。
- `Out.IsPlaying` 在 DLL 的排空轮询中标记 finished，但始终返回 1，保持 DLL 线程可接受之后的 seek；仅当 finished 且 ring 空时，Reader 才报告 EOF（`:198–206`、`:410`）。
- `CanOverlapPlayback=false` 防止切歌重叠期间新曲因抢不到单例而悄悄失去增强（`:469–472`）。

### 3.6 暂停、定位、停止

**暂停：** 记录宿主暂停状态，再调用插件 Pause／UnPause。暂停 seek 的预填充可以临时运行 producer，但 Read 结束后恢复 producer 暂停；输出设备暂停仍由宿主负责。

**定位顺序**（`:418–445`）：

```text
clear ring → seeking=true → SetOutputTime(ms) → 临时 UnPause
 → 等待 DLL 回调 Out.Flush
 → Flush 清 ring、清 seeking／finished
 → 再 Pause，后续 Read 取得新位置 PCM
```

B：实际 DLL 的 `0x10002620` 只有“读取参数 → 写 `0x10038920` → RET”，不执行同步定位。线程 `0x10002a40` 稍后读取请求，将位置按整秒取整，调用 `Out.Flush(+0x44)`，重置状态并 `SetFilePointer`。因此等待 Flush 是重建桥的防旧样本加固，不是原版空 Flush 的逐字复刻。

**剩余定位边界（I/U）：** `Mp3ProSource` 没有覆盖 `LastSeekPosition`，Out.Flush 的实际位置参数也被忽略。非整秒请求、非整秒 CUE 切分的实际落点／丢弃样本是否正确，需要专门验证；不能由整秒 mock 测试推导出 sample-accurate seek。

**停止：** 先设置 stopping、唤醒阻塞 Write，再调用 DLL Stop，避免 Stop 等待 producer、producer 又等 Reader 的死锁。若外部 Stop 发生 SEH，宿主保留停止态 bridge 和占用标记，禁止下一首复用该单例，防止旧线程写进新曲（`:486–503`）。

入口 SEH 不是沙箱；500 ms／1 s 的条件变量预算也无法限制外部 DLL 内部 Play／Stop 自身永久阻塞。

### 3.7 DLL 内部的可见解码链

下表的地址均属于上述哈希的原始 `mp3PRO.dll`；语义名是解释，不是恢复出的原函数名。

| 地址 | 静态可见行为 |
| --- | --- |
| `0x10002350` | In_Module 的 Play 指针；其中 `0x10002483` 传入线程入口，`0x1000248a` 调用 CreateThread |
| `0x10002a40` | 读取压缩块、处理异步 seek／EOF，调用输入供给与解码入口，查询输出可写容量 |
| `0x10006800` | 将剩余压缩数据交给内部 decoder；由线程调用 |
| `0x10005e20` | 调度基础 Layer III 解码与增强处理，更新输出采样率／声道和增强状态，完成格式转换／部分头尾裁剪 |
| `0x1000ec30` | 基础 Layer III 解码流水线，包含粒度／声道循环、多个频域处理及合成 helper |
| `0x1001b320` | 处理增强侧信息／位流、声道相关状态与异常返回 |
| `0x1001b930` | 根据增强状态调用侧信息处理，再逐声道调用增强处理与输出合成 |
| `0x10017ac0 / 0x10017d70` | 分析侧／合成侧候选；可见每声道 PCM、频带状态、间接滤波函数调用与重叠状态维护 |
| `0x10002e40` | 将实际 PCM 格式交给宿主 Out.Open |
| `0x10002f90` | 将内部输出切块，经可视化／可选 DSP 回调后调用 Out.Write |

`0x10005e20` 只有在基础采样率低于 32 kHz 等条件下建立／使用增强状态，增强成功后更新输出采样率。`0x10002f90` 在本桥禁用插件 DSP 的情况下，单次写出 `channels × 0x480` 字节，即 16 位 PCM 的 576 帧；不要把它误写成每声道 1152 帧。

**算法层解释（I）：** mp3PRO 可理解为基础 MP3 低频层与 SBR 边信息组合，增强解码在滤波器组域重建高频并合成全带 PCM；不是简单把低采样率 PCM 重复插值。上述私有 helper 的逐级 QMF／包络／噪声处理完整语义和所有错误分支，本轮没有恢复到数学等价证明。

## 4. 编码：从转换菜单到 mp3PRO 文件

本节 `ttp_enc/...` 路径相对工作区根，其余 `src/...` 路径相对 `rebuild`。

### 4.1 总体链路与三层接口

```text
播放列表“转换格式”
  → ShowPlaylistConverter
  → PluginManager 枚举 EncoderCreator / 配置 / 创建
  → LegacyEncoderSession
  → CreateDecodedAudioSource → 源 PCM
  → 离线 ReplayGain / EQ / 环绕 → 可选 SSRC → normalized float64
  → Encoder::Open → Metadata::Set → Start
  → Encoder::Write：每轮最多 4096 帧，量化为 PCM16
  → ProSession::write：按滤镜 chunk 再分块
  → fhtpro!WriteFilterOutput → 内部 MP3/SBR 编码 → 滤镜自己 WriteFile
  → Finish：CloseFilterOutput 排空 / 修补文件头 / 关闭
  → 通过宿主标签服务写 metadata，或 ID3v2.3 后备
  → 成功才提交 .ttconvert-{GUID} 临时输出到最终文件
```

这里存在三层不同 ABI：

1. **宿主→编码插件：** TTPlayer COM 风格 Encoder，x86 stdcall。
2. **插件内部：** C++ `Instance → ProSession`。
3. **ProSession→滤镜：** CoolEdit 的 `Open/Write/CloseFilterOutput`，WinAPI/stdcall。

它不经过输入解码 DLL，也不经过 LAME；LAME 版本选择只影响普通 MP3 工厂。编码源文件若恰好是 mp3PRO，可以由同一个生产解码工厂先增强解码，再重新编码；这是有损转码，不是复制原始增强码流。

### 4.2 UI、枚举和选择

- `src/ui/player_window_playlist.cpp:6066–6094`：收集曲目并进入转换窗口。
- `src/ui/playlist_transforms.cpp:890–917`：内置 Wave、合并后的插件编码器、可选独立 LAME DLL；`:1063–1080` 调用配置。
- 同文件 `:1129–1163`：一个后台线程逐首转换，不是多曲并行。
- `src/plugins/plugin_manager.cpp:2127–2294`：扫描 `ttp_*.dll`，加载 `ttpGetSoundAddIn`，枚举并按 Encoder category 查询接口。**宿主不直接扫描 `.flt`。**
- `ttp_enc/src/plugin.cpp:20–32,47–64`：插件内部编号 MP3=0、WMA=1、Pro=2；仅 `pro_available()` 成立才返回 Pro 工厂。名称为 `mp3PRO 编码器 (Cool Edit Pro 2)`，扩展名仍是 `mp3`。

插件内部的 2 不能当成宿主全局索引 2，后者还合并其他插件。

### 4.3 Encoder ABI 和会话状态

`ttp_enc/include/ttp_enc_abi.h:7–42`：前三槽为 IUnknown；返回字符串由 CoTaskMemAlloc 分配。

| 接口 | 后续槽位 |
| --- | --- |
| AddIn | 3 Enum |
| EncoderCreator | 3 Create；4 Name；5 Extension；6 Configurable；7 Configure |
| Encoder | 3 Open；4 Start；**5 Finish；6 Write**；7 Extension |
| StreamEncoder | 8 OpenStream |
| Metadata | 3 Count；4 At；5 Get；6 Set |
| Buffer | 3 SetLength；4 Capacity；5 Data |

Encoder category 为 `{5954A909-9E02-4650-995E-F7535EE2BD8B}`；Encoder IID 为 `{04B9A359-7FF7-4EE9-9CFB-AEBB3A331DC7}`。

`LegacyEncoderSession`（`src/plugins/plugin_manager.cpp:1869–1948`）先查询流式入口，不支持则调用路径式 Open；随后 Start、WritePcm、Finalize。**Pro 不暴露 StreamEncoder IID，只接受输出路径**（`ttp_enc/src/encoder.cpp:131–160`）。

Instance 状态为 `Fresh → Open → Active → Finished`，失败可进入 Failed；Open 保存格式、绝对路径和配置快照，Start 才真正打开滤镜输出（同文件 `:109–184`）。

### 4.4 滤镜发现、配置文件和 ABI 参数

`ttp_enc/src/mp3pro.cpp:159–173,203–206`：

- 固定检查／加载 **`ttp_enc.dll 同目录/fhtpro.flt`**，不是 EXE 根目录的 `mp3PRO.dll`。
- 枚举阶段只检查文件存在且不是目录；配置／Start 时才检验能否加载、导出及 IAT 适配是否成功。
- 不需要旧版 MAINDIR/INIDIR 注册表定位或 `flt.dat`。

当前适配器使用的精确签名（`:9–12`）：

```cpp
HGLOBAL WINAPI OpenFilterOutput(
    LPCSTR path, DWORD rate, WORD bits, WORD channels,
    DWORD arg5, DWORD* chunkBytes, DWORD options);
DWORD WINAPI WriteFilterOutput(HGLOBAL output, const void* pcm, DWORD bytes);
void WINAPI CloseFilterOutput(HGLOBAL output);
DWORD WINAPI FilterGetOptions(
    HWND parent, HMODULE filter, DWORD rate,
    WORD channels, WORD bits, DWORD options);
```

**Open 与 Configure 的 bits/channels 顺序不同。** 第五参数当前传 0；本轮不将其未核定的语义强命名为长度或句柄。

`ttp_enc/src/settings.cpp:158–195`：`ttp_enc.ini` 的 `[mp3PRO] Options` 默认 **7**，选择 `coolmp3.ini` 的 `[Configurations] Key7` 等配置。保存目录优先插件可写目录，否则 `%APPDATA%\TTPlayer\ttp_enc`。

`mp3pro.cpp:175–200`：读取到的预设不是 22 个逗号字段时，用以下重建默认值：

```text
44100,2,22050,0,100,96000,2,1,1,0,1,0,0,0,0,0,44100,2,1,0,0,0
```

然后把字段 0/1/16 更新为当前源 rate/channels；mono 时调整相应声道字段。特定立体声 Pro 预设转 mono 时将目标码率减半，避免 96 kbps mono 使旧内核静默选普通 MP3。这里只完整检查字段数，没有完整校验全部数值／组合，不对 22 个字段强行赋予全部已确定语义。

配置窗口（`:208–219`）持滤镜锁，以 44100／2ch／16bit 打开原滤镜 UI。返回 0 表示取消；非零结果保存为 Options 编号。**这个 UI 模板不是正在转换音频的实际格式协商。**

### 4.5 PCM 格式、重采样和三种块大小

1. 宿主使用独立源读取 PCM；可选音效之后，`PcmOutputTransform` 输出 normalized float64，按用户转换设置执行可选 SSRC（`src/ui/playlist_transforms.cpp:1573–1612,1640–1662`）。
2. 插件输入验证允许 1–2 声道、8–192 kHz、整数 PCM8/16/24/32、float32/64 及部分 extensible 格式，严格核对对齐和字节率（`ttp_enc/src/encoder.cpp:93–107`）。
3. **这不是滤镜能力协商。** 重建插件并未调用 `FilterUnderstandsFormat` 或 `GetSuggestedSampleType2`，也没有在 ProSession 内自动重采样。滤镜 `0x1001fbc0` 直接可见的初始化采样率集合为 8000、11025、12000、16000、22050、24000、32000、44100、48000；不能把插件的 192 kHz 接收上限当成内核支持上限。
4. `Instance::Write` 每轮最多 4096 帧，量化为私有 PCM16，再调用 ProSession（同文件 `:229–247`）。float 非有限值归零、削波后乘 32768 并舍入；PCM24/32 保留高 16 位，没有 dither，不修改输入（`ttp_enc/src/pcm.cpp:47–85`）。
5. 插件始终给滤镜 16 位 PCM，未利用滤镜内部可见的 32 位输入分支。

块大小不能混同：

| 层级 | 大小／含义 |
| --- | --- |
| 宿主源读取请求 | 64 KiB；不是 MP3 帧 |
| Encoder 量化批次 | 最多 4096 PCM 帧 |
| 滤镜 Write 批次 | 由 Open 返回 chunk 决定；真实 Pro96 历史记录为 `0x1200=4608` 字节 |

44.1 kHz／双声道／16 位时，4608 字节是 **1152 输入 PCM 帧**。裸滤镜普通模式历史 chunk 为 `0x4800=18432` 字节。最后一块允许不足 chunk；不能假定每次 Write 恰好产出一个完整压缩帧。

内部 half-scale double 与插件 ABI normalized double 是不同边界，`ttpcomm/src/runtime_quantizer.cpp:215–225` 的输出转换包含乘 2，不能只凭插件 `×32768` 推断音量翻倍。

### 4.6 Start、Write、Finish 的直接二进制核对

本轮已对**当前 p3 插件**使用 REA，得到：

| p3 地址 | 对应源码行为 |
| --- | --- |
| `0x10007d7a` | 形成同目录 `fhtpro.flt` 路径；解析 Open、Write、Close、FilterGetOptions；执行模块导入适配 |
| `0x100085b4` | 检查 busy；写入会话预设；OpenFilterOutput；验证返回句柄和 chunk |
| `0x10008793` | 逐块调用 WriteFilterOutput；检查 I/O 错误和消耗字节数 |
| `0x10008817` | CloseFilterOutput；清理 acquired／输出路径／文件句柄／busy；传播收尾写错误 |
| `0x10007928` | 输出 token 对应真实 Unicode CreateFileW 的重定向 |

Start 的实际参数与源码 `mp3pro.cpp:223–234` 一致：

```cpp
OpenFilterOutput(
    "TTPLAYER_MP3PRO_OUTPUT.mp3",
    format.nSamplesPerSec, 16, format.nChannels, 0,
    &chunk, 2147480000);  // 0x7ffff1c0
```

会话配置写到 `Key2147480000`，与用户选择的 Options 编号分开。返回 null／HGLOBAL(1) 均拒绝；chunk 必须至少一帧、不超过 1 MiB、且整帧对齐。

Write 返回值是**消费的输入 PCM 字节数**，必须等于本块 n，不是压缩输出字节数。Encoder::Write 的 `DWORD* written` 在 Pro 路径维持 0；当前宿主不靠它计消费或进度。

### 4.7 Unicode 文件输出不是传统回调参数

不能把这条路径画成“Open 传写文件 callback → 宿主 IStream 写 MP3”。**实际是滤镜自己创建文件并调用 WriteFile，重建适配器在滤镜模块内部替换 API 槽。**

`ttp_enc/src/mp3pro.cpp:44–157` 适配五种 API：

- GetPrivateProfileStringA、GetPrivateProfileIntA、WritePrivateProfileStringA：只把 basename 为 `coolmp3.ini` 的配置访问重定向到 Unicode 可写目录。
- CreateFileA：只把特殊输出 token 映射为真实输出路径的 CreateFileW。
- WriteFile：继续执行系统 API，同时记录目标句柄失败／短写，以免旧滤镜“消费成功”掩盖磁盘写错误。

为兼容压缩滤镜，先检查普通导入槽，再扫描已加载可执行页面中 `FF15/FF25` 间接 call/jump 所用的 API 槽；五种替换都必须找到。卸载前只恢复仍指向本适配器的槽（`:23–33`）。

这是**特定已知滤镜的进程内兼容机制**，不是对任意同名 DLL／任意 UPX 变体的通用保证。LoadLibrary 已先运行其初始化，之后 patch 并不能回溯阻止旧 DLL 的初始化副作用。

### 4.8 滤镜内核的可见编码链

以下内部函数直接绑定**既有解包副本**，保持第 2 节的来源限制：

```text
OpenFilterOutput @1000d073
  → 配置解析 1000fda9（导出跳板 1000d0c8）
  → 1001fbc0：创建编码上下文／检查输入率
  → 10021920：设置压缩模式／码率／声道等配置
  → 10021d10：设置其他编码参数

WriteFilterOutput @1000d096 → 实现 10018a5c
  → 100203a0：输入消费、缓存、可选重采样和编码调度
       ├─ 启用增强时 → 10024a00 → 1003b820 及相关 helper
       └─ 10023460：基础编码、缓冲与成帧／输出调度
  → WriteFile：写出实际产生的压缩字节

CloseFilterOutput @1000d064
  → 重复调用 100203a0 排空
  → 10021dd0：需要时生成／回写文件头
  → 10020370：销毁编码状态
  → 关闭文件和内存
```

REA 在副本中还直接观察到 `relsbrenclib/src/sbr_main.c`、`qmf_enc.c`、`tran_det.c`、`env_est.c`、`invf_est.c`、`nf_est.c`、`code_env.c` 等字符串，以及相应 SBR/QMF 错误文本。

**算法层解释（I）：** 全带输入经过分析滤波、瞬态／包络／噪声等参数估计；低频层编码为兼容 Layer III，高频重建所需边信息随兼容码流携带。它不是两个独立音轨，也不是先生成普通完整带宽 MP3 后再外挂一个 ID3 标签。具体边信息封装、位分配和全部私有 helper 的数学定义，本轮没有逐位恢复。

Ghidra 对滤镜多个导出把返回值误还原为 void，对 `0x100203a0` 的栈参数还原也明显失真。因此 ABI 以当前适配器声明和指令核对为依据；例如 Write 实现以 `RET 0xc` 返回，不能直接抄伪码中的 void／无参签名。

### 4.9 收尾、标签与最终文件事务

`ttp_enc/src/encoder.cpp:187–227`：

- Finish 先关闭滤镜，排空后才重开文件写标签；Failed 状态不回写标签。
- 正常／异常销毁都有收尾保护；重复 Finish 返回 S_FALSE。
- 优先从已加载的 ttpctrl.dll 或 EXE 取得 `CreateStdContent / CreateStreamOnFile`；通过宿主流和 Metadata 写标签。
- 无宿主服务时，使用 Unicode ID3v2.3 后备，流式复制到同目录临时文件再 ReplaceFile；不是把整首读进内存。

宿主在 Open 后、Start 前设置非空 metadata，过滤 `replaygain_*`，不经当前这条通用 metadata 流程复制封面（`playlist_transforms.cpp:1483–1500,1622–1628`）。

**标签策略差异（S/I）：** 当前 rebuild 的 `CreateStdContent` 直接转发 write_type；插件传 0，而 `builtin_file_info.cpp:2071–2106` 的 0／4 路径使用 BuildApe，没有自动带入用户 MP3 标签写入策略。因此不能把“插件委托宿主写标签”进一步宣称为“当前 rebuild 已完全遵循所有 ID3v1／ID3v2／APEv2 用户选择”。这是静态差异，尚无本轮输出标签布局实测。

宿主转换在目标同卷创建 `.ttconvert-{GUID}` 私有临时输出，Finalize 和标签均成功后才 MoveFileExW 提交；失败／取消丢弃临时文件，保留原目标（`playlist_transforms.cpp:1445–1480,1614–1696`）。

### 4.10 并发和容错限制

- 编码与解码各有自己的单例状态；不是同一把锁。
- 编码滤镜为模块级全局 `Filter`。Start／配置／Finish 受 mutex 和 busy 保护；编码中其他 Start／Configure 返回 ERROR_BUSY（`0x800700aa`）。
- 配置窗口全程持锁；Write 不持 mutex，依赖同一会话串行调用，不能同时 Write／Finish。
- 固定会话键与共享 INI 没有跨进程锁，两个播放器进程可能互相覆盖配置。
- 输入验证上限不是内核能力；没有隐式 downmix，>2 声道被拒绝。
- 当前 I/O 适配记录 CreateFile／WriteFile 错误，不覆盖 SetFilePointer／CloseHandle 所有失败；没有据此证明超长文件、磁盘耗尽和掉电安全。
- API 槽扫描是逐字节模式识别，不是完整 x86 指令边界解码，不能保证任意滤镜版本都适用。

## 5. 已有真实样本及验证边界

本轮只读解析当前样本字节，核对相邻 MPEG 帧与历史 SHA-256：

| 样本（`tests/mp3pro_encoder_analysis/` 下） | 字节数／SHA-256 | 本轮实际帧头观察 |
| --- | --- | --- |
| `default_without_dat/encoded.mp3` | 33,019；`387c865e8ec3c13e70b1fe6d7574957fa2378d6a01522ce9aeecb9877addc574` | MPEG-1 Layer III，44100 Hz，128000 bit/s，2ch；首帧 418 字节 |
| `pro96_without_dat/encoded.mp3` | 25,078；`5a066fa78b1a478e5f6919a551281ee11af1aaf7fb38e4d25b5b4f83347706ef` | MPEG-2 Layer III，22050 Hz，96000 bit/s，2ch；首三帧 313／314／313 字节 |

历史 `probe_results.json:9–11` 中，Pro 样本经原滤镜格式回读显示：

```text
44100 Hz / 16 bit / 2ch
MPEG Layer-3 Pro, 96 Kbps (Int+MS J-Stereo)
```

这与基础层 22.05 kHz、增强输出 44.1 kHz 的结构相符，但该 probe（`filter_probe.cpp:83–89`）只 OpenFilterInput／查询描述／关闭，**没有读取解码 PCM**。生成输入是两秒 440/660 Hz 双声道正弦，不能作为高频 SBR 音质测试。

因此可确定：

- 当前确实有真实滤镜生成且被滤镜识别为 Pro 的样本；旧 `MP3PRO_RECOVERY.md:94` 只描述当时特定音乐目录，不能当成当前全工作区“没有样本”。
- FFprobe／FFmpeg 只读出基础 MP3 层，不证明 SBR 增强有效。
- `tests/mp3pro_source_tests.cpp` 的真实 DLL 分支用普通 MPEG-2 静音；增强 PCM／EOF／seek／CUE 的测试使用 mock。
- `ttp_enc/docs/VALIDATION.md:39–45` 明确最新 p3 没有重跑 mp3PRO；较早 p1 的 Pro 记录不能改称 p3 本轮通过。
- 尚未找到真实 Pro 输出 → 原始 `mp3PRO.dll` → 当前重建增强桥，且明确断言 `mp3PRO Audio`、全量读取与定位的闭环验收记录。

## 6. 问题清单与下一步验收

| 问题 | 本轮结果 |
| --- | --- |
| 解码／编码组件、ABI、部署目录与版本区分 | 已回答（S/B） |
| 解码准入、推拉缓冲、暂停、异步 seek、EOF 和清理 | 已回答到静态链路（S/B）；真实边界需运行验证 |
| 编码配置、PCM 量化、分块、Unicode 写文件、Finish／标签事务 | 已回答到当前源与 p3 原生适配器（S/B） |
| SBR 内核主调度与部分 helper 链 | 部分回答（B/I）；不是全算法恢复 |
| 原始压缩滤镜→既有解包副本的完整同源证明 | 未在本轮复现（U） |
| 最新宿主／最新插件真实增强 roundtrip 和音质等价 | 未验证（U） |

建议后续使用独立临时目录和独立进程执行，不覆盖本次或既有证据：

1. 用同一真实 Pro 样本做有／无输入 DLL 对照；打印并断言生产 Reader 的 `DisplayFormat.codec_name`，不能只看文件扩展名或 DLL 已加载。
2. 验证 22.05→44.1 kHz、全量 PCM 非空、块大小不影响输出、EOF 无丢尾、重复打开、暂停预填充与并发占用。
3. 加入非整秒 seek／CUE；核对真实落点及应丢弃 PCM，而不只检查 UI 请求时间。
4. 使用含 11 kHz 以上内容的素材和原版对照，区分真实 SBR 重建与普通升采样；对齐编码延迟后再比较。
5. 用 p3 插件实际输出，核对 Pro／mono 预设、用户标签策略、短写／磁盘满、取消后立即重开及双进程配置竞争。

现有 `tools/probe_pcm_integrity.cpp` 可作离线分块稳定性参考，但不打印／断言增强 codec 名；其默认所谓 MF 参考仍走同一工厂。既有二进制是 2026-09-23 构建，不能直接作为最新宿主验收。

## 7. REA Evidence 索引

下列完整 ID 均可在导出的 bundle 中查找。REA 输出为 Ghidra 12.1.4 的静态观察，不是原源码，不代表实际执行。

| 内容 | Evidence ID |
| --- | --- |
| 输入导出 getter | `ev_e262f72215c3e6d0b60063e7ead3eeb26d61166cf11844732c7ceee8daac0050` |
| 输入 In_Module 152 字节表 | `ev_e02276fee3d5dccba8bfb3e0f32aca510a38f2044719877c7c5d7a2057ce9653` |
| 输入 DLL 产品／检测字符串 | `ev_f2709ebbe638b16d6fc18813be43142f899807475c134b3e319337116002ce4b` |
| 输入线程主体（batch 中一项成功、一项函数未发现） | `ev_f775dbc51515ba83fff8406adda11ce092bed03b525a4926978a50675b2d8eb7` |
| 输入格式打开／PCM 写出／主解码函数 | `ev_02d898b5df23519c1c85bc870e6cd21709fd3e5390521400915fd60d9b64009d` |
| 主解码函数完整文本分页 | `ev_f752048ed3b180f4bf2848017345ed5bd9e34c752f6bea9301c7265371c9ffb3` |
| 基础解码／增强侧信息／增强调度 | `ev_53904482d12b5632713d0bb5bb0521eb2fb5e64cf751e061590984145aae5988` |
| 增强分析／合成 helper | `ev_245eacff94131f2db6a7b8d1b6f9b7e8623d1c73ce730fbeeb3c1be36533a1ed` |
| SetOutputTime 写请求指令 | `ev_65aed2211ce681d23dd5a940d850e3b1e1baf4ecce39123845a81657aa0d67a2` |
| Play 中 CreateThread 调用指令／对应导入 | `ev_331b7611c77ae4d81f5303cda8243727a27078d7c52397227594fcc51b85ba10`；`ev_ec60ced5694721b6599af83ce0ff6da0ed5415df136827939c3da4579dc466b8` |
| 原始压缩滤镜导出目录 | `ev_89aa684130e7c6bd9247bf64392d2fc562f293223f2b66df7e6aca4088633f83` |
| 原始压缩滤镜导入目录 | `ev_5541c11ba259e7dfb54607f9a7d5b1a510f6beceebc39daa8649c451933f660d` |
| 解包副本 Open／Write／Close／配置导出 | `ev_97270750de9b9523ad53d1a347f9ea4cd1cc3e6c7cbd8de0d73988908621010f` |
| 解包副本上下文初始化／编码主调度／配置／头回写 | `ev_68e25ea4c48071d70457ee8d71c060b8b1012abc8816982db9a986dd238a5379` |
| 解包副本增强与基础编码调度 | `ev_3059252aa2a970994e5fc5f856d85e95491eb53beab8a95a86fd47f5fbfb262d` |
| 解包副本 SBR 来源／错误字符串 | `ev_88b16a3439d6a3485b3f657c3cb6642548bf86dc2be9eb7377f445e4ae68fcb3` |
| 解包副本 Write 真实实现指令 | `ev_abd1894191c93475a4321bdf7213bcbd74ace53addbc47294d66901375c33921` |
| 解包副本 1003b820 完整函数 dossier | `ev_3f3031c4b16876ebda4718a43b34f45bf823e5ccc3971308bed6d9fafb12a378` |
| 根 AddIn 原版导出目录／哈希 | `ev_c4e90a8431a76edad7097ccc9c588c3bed21ae5d537ad0e8974ea06e6c9dc98b` |
| p3 插件导出入口 | `ev_a134b526cf0d028a3e35b0a5395559711f4591c1e62b332171760006d88fe731` |
| p3 输出 token 的重定向／Start | `ev_b8c42a1667610da98bb8cbb57466fef574b1a7d3c6b1c8e5b2cb4088e58323d0` |
| p3 加载／分块 Write／Finish | `ev_a4ae37ed0d68e5bfe1311eace6454c6bd6bd5a7038870baac989d1d5b46984c3` |

部分输入回调在 Ghidra 中有已解码指令却没有 FunctionManager 函数对象，本轮对 seek／Pause／UnPause 改用逐指令读取，不能把“函数查找失败”写成函数不存在。`1003b820` 的 dossier 保留了完整可见伪码／汇编和调用者／被调用者，但 native value-flow 被默认 3000-operation 上限截断；本轮没有据此声称完整跨函数数据流或无遗漏证明。

保存后已检查上述 23 个不同 Evidence 引用均存在于 bundle。导出包包含 61 条 Evidence 和 3 项未解决 Unknown，共 5,162,993 字节；SHA-256 为 `30338d52e2d062050c29c57308a20d93ac742f7f62ada0367ee53ec1d9f948e7`。原生引擎已关闭，调查已保存并 reset。

未解决项的稳定标识：

- 真实增强端到端：`unk_132fda80969bababf0a382a078f2fe331ec423b3c5b125e3cb58331daf5caca1`
- 非整秒 seek／CUE：`unk_6b70e7dd65c61ef7e96cf95fe8c1a07cce88356ceffd4b37d46a2b3c76ac8d91`
- 解包副本可重复同源证明：`unk_1bd11d94c6debe0a9c9fefc44ae9d6ba8f0fecc0c21ff45ae81508576e9f9cbd`
