# 原版音效插件实现与重建版差异分析

分析日期：2026-09-27。

## 1. 结论与证据范围

`D:\Projects\Backup\TTPlayer\Plugins` 是一套 **Winamp DSP 音效插件包**。其中 17 个顶层 DLL 均为 x86 PE32，均导出 `winampDSPGetHeader2`。它们接收解码后的 PCM，区别于 `AddIn` 中负责识别、解码音乐文件的输入插件，也区别于播放器内部均衡器和杜比环绕处理器。

原版实现了多插件顺序串联、即时启停、同实例配置、调整顺序、持久化及部分插件窗口吸附管理。**重建版的音频处理接口已经接通，但配置、生命周期和窗口管理尚未完整复原，不能认定这 17 个插件已全部按原版运行。**

本次证据分为三类：

- **已确认**：原版 `TTPlayer.exe.pseudo.c`、必要位置的原 EXE 反汇编、重建版源码、17 个 DLL 的 PE 信息、5 个未压缩接口的回调反汇编、5 个 `.reg` 文件原文。
- **有依据的风险或推断**：线程归属导致的插件窗口消息问题、不同配置实例无法实时同步参数、资源路径不同导致的兼容问题。报告分别说明触发条件。
- **尚未验证**：17 个插件逐一在 XP／Win7／Win10／Win11 上的运行、听感与输出 PCM 对照；压缩／保护 DLL 的全部内部算法；各授权字段的有效性。此次没有加载插件、执行附带 EXE 或导入／删除注册表。

注册表专项分析见 [PLUGIN_REGISTRY_FILES_ANALYSIS.md](PLUGIN_REGISTRY_FILES_ANALYSIS.md)。本文的地址均为对应文件的首选加载地址 VA，不是文件偏移，ASLR 后应换算。

## 2. 插件清单

功能／版本列优先记录随包 `Plugins/Readme.txt` 的说明，不代表已逆向验证所有算法或宣传效果。该 README 为 GB18030 编码，标题为“音效插件包 Build 1001”；目录内部分插件经过适配，不能直接套用上游原版插件的行为。

| DLL | 随包描述／主要用途 | 字节数 | 静态分析状态 |
| --- | --- | ---: | --- |
| dsp_compwide.dll | 1by1 compression/widening 1.1，压缩与声场扩展 | 8,192 | UPX |
| dsp_DEE.DLL | Dee 1.20，综合音效 | 86,016 | UPX |
| dsp_DeFX.dll | DeFX 0.97，综合音效 | 30,720 | PECompact 标记 |
| Dsp_Dfx.dll | DFX 9.304，综合音效及独立设置程序 | 1,015,808 | 可直接解析，header 0x21 |
| dsp_Dolby.dll | Dolby Prologic Surround Similar 5.0.1，立体声环绕 | 110,080 | 可直接解析，header 0x20 |
| dsp_enh.dll | Enhancer 0.17，增强器 | 45,568 | PECompact 标记 |
| dsp_izOzone.dll | iZotope Ozone 1.03，综合母带音效 | 3,022,923 | 可直接解析，header 0x20 |
| dsp_iZVinyl.dll | iZotope Vinyl 1.0，唱片风格音效 | 1,110,528 | UPX |
| dsp_jammix.dll | Jammix Enhancer 031，综合增强 | 242,176 | UPX |
| dsp_neq.dll | 250 段均衡器 1.43 | 69,120 | UPX；另导出调试符号 |
| dsp_OctiMax.dll | OctiMax 1.51，动态处理 | 203,264 | PECompact 标记 |
| dsp_skywalker.dll | Skywalker，组合音效 | 73,728 | 可直接解析，header 0x20 |
| dsp_so3d.dll | SOrient SoftAmp VirtualSound，虚拟声场 | 463,872 | PECompact 标记 |
| dsp_sps.dll | Nullsoft SPS 0.36，脚本式 PCM 处理 | 22,016 | PECompact 标记 |
| dsp_superequ.dll | Shibatch EQ 0.03，均衡器 | 85,504 | PECompact 标记 |
| DSP_VAPXP.dll | ViPER Audio Processor eXperience 1.1.36 | 241,664 | 可直接解析，header 0x20 |
| DSP_ViPER’s Audio.dll | ViPER Audio DSP 2.3.1 #1 | 243,200 | SEGX／SEGY／.SEGZ，接口代码非直接可读；另导出 ResetDSP |

共 5 个 UPX、6 个带 PECompact 标记的 DLL、1 个使用特殊节布局且接口无法直接还原的 DLL。对这些文件，压缩状态下的导入表不能证明完整依赖或完整 API 使用范围；随机字节反汇编不能作为算法证据。

目录还包含：DFX 1513 个文件、DSP_SPS 23 个预设、Enhancer 7 个文件、JammiX 24 个文件、Ozone 13 个文件、“预设配置”9 个文件。**只复制 17 个 DLL 并不等价于安装这套插件包。**

## 3. 原版 Winamp DSP ABI

### 3.1 导出入口与结构布局

`FUN_0042822F` 使用 `LoadLibraryW` → `GetProcAddress("winampDSPGetHeader2")` → 调用入口，要求 header 非空且版本大于 `0x1F`。

```cpp
// 32 位布局；回调使用 __cdecl。
struct Header {
    int version;                    // +0x00
    const char* description;        // +0x04，ANSI
    Module* (*getModule)(int);      // +0x08
};

struct Module {
    const char* description;        // +0x00
    HWND hwndParent;               // +0x04
    HINSTANCE hDllInstance;         // +0x08
    void (*Config)(Module*);        // +0x0C
    int (*Init)(Module*);           // +0x10
    int (*ModifySamples)(Module*, short*, int frames,
                         int bits, int channels, int rate); // +0x14
    void (*Quit)(Module*);          // +0x18
    // 插件可以继续带私有字段，例如 Ozone 的 userData 在 +0x1C。
};
```

关键点是 **不能在结构头部插入 userData**，否则全部回调偏移都会错。也不能说插件完全没有 userData：Ozone 二进制明确使用 `Module+0x1C`。宿主应操作 DLL 返回的对象，不应重新分配一个只有前 28 字节的替代对象。

17 个 DLL 均未导出 `DllRegisterServer`；加载依据是 Winamp DSP 入口，不是 COM 注册。附带 `.reg` 文件不会改变 ABI，也不是让宿主“发现 DLL”的注册步骤。

### 3.2 加载与初始化

`FUN_00427EDC` 的流程：

1. 用上述 loader 取得 DLL 和 header。
2. 将 header 描述转小写，若包含 `pacemaker` 则拒绝。
3. 从 `getModule(0)` 到 `getModule(9)` 查找第一个非空返回值；找到后停止。一个 DLL 只选其中一个模块，并非全部启用。
4. 如当前包装器已有模块，先卸载。
5. 写入 `hDllInstance`、宿主父窗口，然后调用 `Init(module)`。
6. 把实例放入受临界区保护的活动记录。
7. 查找 `DFX_WINDOW`、`Dee2` 特殊窗口，加入原版窗口管理。

**原版没有根据 Init 的返回整数判断成功。** 重建版保留了这一行为，只有异常等条件会判失败。这是历史兼容行为，不能据此认为非零返回一定安全，也不能把“任意改变返回值处理”当作纯粹还原。

PaceMaker 被排除是明确事实；它与变速／变长输出的兼容性有关是合理推测，伪代码中没有作者原因说明。

## 4. 发现、选项页与持久化

### 4.1 目录扫描的真实边界

相关函数：`00428626`、`0042875F`、`004288DB`、`004289FC`、`00428372`、`004281C6`。

- 枚举 `dsp_*.dll`，不递归搜索子目录。
- 在配置目录与程序 `Plugins` 目录之间进行扫描，路径不同则分别枚举；用不区分大小写的完整路径去重。
- `0042875F` 还通过 `004C8421` 向进程 `PATH` 追加路径组合，服务于旧 DLL 的依赖查找。这个操作不等于改变当前工作目录，也不等于设置 DFX 注册表中的资源路径。
- **候选文件入表和 header 校验是两个阶段。** `00428626 → 004288DB → 004289FC` 先把路径加入管理列表；`00428372 → 004281C6 → 0042822F` 才为未启用项目加载 header 以获取描述。
- `00498C2C` 不依据 `00428372` 的返回值跳过插行，反汇编 `00498CA2` 调用后的指令也确认这一点。因此，“原版只有 header 验证成功才显示一行”的旧文档描述过于严格：无效候选可能仍显示文件名／空描述。

重建版隔离扫描只返回验证成功的 DLL，是主动筛选行为。保留隔离扫描有价值，但如果追求原版完整可见性，宜保留失败候选并显示失败原因，而不是把筛选后的列表等同于原版枚举结果。

### 4.2 选项页 259

| 控件 | ID | 原版行为 |
| --- | ---: | --- |
| 目录编辑框／浏览 | 1028／1023 | 更新扫描目录 |
| 插件列表 | 1064 | 勾选、描述、文件名三列；资源 0x815B 提供列文字 |
| 配置 | 1015 | 仅活动模块可用；直接调用现有实例的 Config |
| 上移／下移 | 1042／1045 | 调整管理列表顺序 |

`0049911C` 根据勾选状态立即调用 `00428471` 加载／卸载目标模块；选中行本身不应重建链。配置按钮与双击走 `00428972 → 004280BE`。

`0042892A → 00428A7F` 对调的是路径与活动实例记录，**没有为排序执行全链 Quit／Init**。音效顺序决定声音结果，例如“压缩→混响”与“混响→压缩”不同，不能只移动显示行。

原版管理器 `DAT_00546C64` 是全局实例；`0042855B` 导出活动路径。配置使用 `Plugin/@Folder`、`Modules_Count`、`Modules_N`，保存的是启用模块的完整路径及顺序。原目录 `TTPlayer.xml` 当前的 Folder 仍为 `C:\Program Files (x86)\TTPlayer\Plugins\`，属于迁移前路径；只移动目录不会自动更新全部绝对路径。

## 5. 音频处理链

概括相关路径，不把所有可选格式转换细节展开为单一固定次序：

```text
输入插件解码 PCM
    ↓
播放器内部处理：ReplayGain／EQ／Surround 等及所需采样率转换
    ↓
转为有符号 16 位交错 PCM
    ↓
活动 DSP[0] → 活动 DSP[1] → … → 活动 DSP[n-1]
    ↓
按输出格式转换 → 输出设备
```

证据：`004B1375` 调度内部处理，`004B1950` 与 `004B1B81` 处理增益等；`004AC166` 在启用全局 DSP 时进入 16 位边界并调用 `0042898D`，必要时再转换输出位宽。

### 5.1 单个回调的参数

`004280CD` 从字节数计算帧数：

```text
每帧字节数 = (bits / 8) × channels
frames = bufferBytes / 每帧字节数
ModifySamples(module, buffer, frames, bits, channels, sampleRate)
```

双声道的一个 frame 包含 L、R 两个样本，不能把样本总数当成 frames。多声道参数会传给插件，但这不保证插件支持多声道。

### 5.2 分块规则与输出长度

- `frames <= 0x3FFF`（16383）：调用一次。
- `frames > 0x3FFF`：先调用 `floor(frames/2)`，指针前移，再调用剩余部分。
- **仅对半一次**，不是循环切成不超过 16383 的块。例如 32770 帧会分成两个 16385 帧块。
- 原版忽略 `ModifySamples` 返回值，不按返回帧数调整输出长度。
- `0042898D` 顺序处理同一缓冲区，各插件串联，不是多线程并行混合。

因此重建版现在保留的对半规则是正确的；把它改成“每次 576 帧”或通用限长循环都不是原版行为。16 位 DSP 边界也意味着即使输出选 24／32 位，经过这些 DSP 的内容仍已发生 16 位量化。

### 5.3 SPS 变速预设的特别限制

`DSP_SPS` 有 23 个 `.sps`。可直接读取的预设使用 `[SPS PRESET]`、滑块参数和 `codeN_size/codeN_data` 保存脚本，不是播放器原生算法：

- 音量预设脚本包括 `spl0=spl0*slider1; spl1=spl1*slider1;`。
- pitch/tempo 预设使用 `skip`、`repeat`、`pos` 等控制重复／丢弃样本。

结合宿主忽略回调返回帧数的事实，不能保证这类改变输出长度的脚本按 Winamp 的完整契约运行。不能从“DSP 能加载”推导“所有变速预设等价”。完整恢复原版应先保留其限制；增加变长输出属于另一个需要缓冲区容量、时间轴、EOF 和播放速度设计的功能。

## 6. 5 个可直接核实的实际插件

| DLL | Config | Init | Modify | Quit |
| --- | --- | --- | --- | --- |
| DFX | 10001070 | 10001660 | 10001810 | 100011B0 |
| Dolby | 10001660 | 10001FE0 | 10001860 | 10001670 |
| Ozone | 5941A5C0 | 5941A390 | 5941A6C0 | 5941B6D0 |
| Skywalker | 10003160 | 100032E0 | 10003360 | 10003330 |
| VAPXP | 100026C0 | 10002440 | 100028A0 | 100026E0 |

这五个 DLL 的 `getModule` 都是仅索引 0 返回有效模块。

### DFX

Config 并非普通内嵌对话框：`10001070` 读取 `HKLM\SOFTWARE\DFX\11\top_folder` 的默认字符串，拼接 `Apps\dfxwsettings.exe`，再调用 `CreateProcessW`。目录内确实有 `Plugins\DFX\Apps\dfxwsettings.exe`，不能误判成文件缺失。

这解释了错误 `.reg` 路径为何会导致设置打不开：DLL 在 D 盘可以加载，而 Config 仍去 C 盘寻找辅助 EXE。注册表路径读取的作用有直接机器码依据，详见专项文档。

### Dolby

Config `10001660` 对全局 HWND 调用 `ShowWindow(..., 5)`；真正初始化在 Init。Modify 的前置检查包括双声道和 16 位，不满足则走旁路。它不是将任意输入自动转换为多声道 Dolby 编码的输出插件。

### Ozone

Init `5941A390` 创建处理对象并写到 `module+0x1C`，随后进入界面创建／显示路径。Config `5941A5C0` 先检查这个指针，空则直接返回。磁盘模块对象的该字段初始值为 0。

Modify 同样读取该字段：为空时只返回输入帧数。Quit 清空并清理对象。由此可以明确指出重建版“新进程不 Init，只 Config”的生命周期缺口，而无需实际导入授权资料。

### Skywalker

Config 使用 `CreateDialogParamA` 创建非模态对话框，随后返回。宿主必须保持 DLL 加载和消息循环，否则窗口可能刚出现即随进程退出。

Modify `10003360` 检测声道／采样率变化，变化时销毁并重建相关状态，再依次调用三个处理阶段，最后返回原输入帧数。这里是可确认的处理调度，不足以命名三个阶段的全部算法。

### VAPXP

Config `100026C0` 检查全局对象，存在才显示其窗口。Modify 明确检查 16 位、双声道及启用状态。停止状态下尚未 Init 的独立配置实例无法替代已初始化的活动对象。

## 7. 原版线程、同步与插件窗口

原版全局链与各模块包装器均使用临界区；卸载时先从可处理位置清除模块，再 Quit、FreeLibrary。`00428B16` 按管理列表顺序销毁模块。音频回调与实例变更不能同时无保护访问。

界面勾选通过选项页消息处理调用加载；处理回调则沿声音输出路径进入。不是每个 DSP 各分配一个并行音频线程。

DFX／Dee 的特殊窗口支持包括：

- `00427EDC` 查找 `DFX_WINDOW` 和类名／标题 `Dee2`。
- `00427E08 → 0043556C` 子类化该窗口，`0040BF67` 加入全局 HWND 集合。
- 集合在 `0044F804`、`004709A2` 的窗口移动／吸附计算中使用，也参与其它窗口协调。
- vtable 的 `00427E4B` 清理路径从集合移除 HWND。

所以“插件能发声”与“插件窗口能像原版参与吸附和生命周期管理”是不同层面的兼容目标。当前重建版没有这两个类名的对应集成。

## 8. 重建版核对结果

源码：`src/audio/winamp_dsp.cpp`、`src/audio/audio_engine.cpp`、`src/app/dsp_worker.cpp`、`src/ui/player_window_options.cpp`。

### 已恢复或有明确增强的部分

- ABI 回调偏移、最低 header 版本、PaceMaker 排除、0～9 选择首个模块。
- Init／Modify／Quit、16 位 PCM 边界、同缓冲区顺序串联、单次对半与忽略返回长度。
- AudioEngine 之间共享 DSP chain，换曲且配置未变化时不必重新初始化。
- mutex 串行化音频回调；SEH／C++ 边界在部分异常时摘除错误模块。
- 保留配置目录、完整 DLL 路径、顺序和勾选状态。
- 隔离扫描有每 DLL 1500 ms 预算，避免第三方 DllMain／header 卡住选项页。

### 尚未一致的部分

| 优先级 | 问题与代码证据 | 可见影响 |
| --- | --- | --- |
| 高 | `dsp_worker.cpp::Configure` 只 getModule(0)，没有 Init／Quit | Ozone、Dolby、VAPXP 等依赖初始化的配置入口无法按原版工作；也与运行时 0～9 选择规则不同 |
| 高 | 配置进程与实时 DSP 在不同进程，使用不同实例 | 调整只改了另一份内存；依赖内存共享参数的插件不会实时改变正在播放的声音 |
| 高 | Config 返回后直接 FreeLibrary，worker 随即结束，无非模态窗口消息循环 | Skywalker 类窗口可能立即销毁，模态／非模态插件不能一概处理 |
| 高 | `WinampDspChain::Update` 任何路径列表变化都 CloseAll 后重载 | 上移下移、增减一个插件会重置其它插件、窗口及滤波／混响状态；原版保留未变实例 |
| 中 | Update 从播放工作线程的处理器构造／revision 更新进入；AudioEngine::Configure 本身只更新 options | 停止时勾选不立即 Init／Quit；播放时若插件在 Init 所在线程创建 UI，线程消息处理须单独解决 |
| 中 | 勾选框表示期望启用，配置按钮只检查勾选；未绑定实际加载成功状态 | 失败的插件仍可能显示已勾选并允许配置；原版按钮依据活动实例 |
| 中 | 扫描返回成功候选；列表提交会重新生成活动路径 | 部分 DLL 探测超时但整体扫描成功时，缺失项目可能在后续提交中丢失；现有保护主要针对整体未完成／失败 |
| 中 | 初始化勾选匹配允许只比较 filename | 不同目录的同名 DLL 可能被同时标成选中；原版管理器按完整路径去重 |
| 中 | 无 DFX／Dee 窗口管理接入 | 吸附、关闭和窗口协调不能认为已一致 |
| 中 | runtime 用 LoadLibraryEx 的 DLL 搜索策略；配置 worker 的 CWD 为 DLL 目录 | 改善旁置依赖查找，但不等价于原版 PATH 设置，且不会修复 DFX 注册表绝对路径 |

这些差异中有为了故障隔离作出的设计选择，但“隔离配置”目前没有对应的参数同步和生命周期协议，因而不能同时获得原版同实例配置语义。

SEH 也不代表能安全恢复所有第三方错误：死循环不会自动变为异常，越界写可能破坏进程，插件自行 SendMessage／持锁行为仍需避免死锁。此处说明实际实现边界，不把现有插件判为恶意程序。

## 9. 建议实施顺序

1. **恢复活动实例的 Config。** 保留隔离发现；Config、Init、Quit 的线程归属和窗口消息循环按实例统一管理。先解决 Ozone／Dolby／VAPXP／Skywalker 四种已知行为，不只给 worker 临时补一个 Init。
2. **改为增量生命周期。** 按规范化完整路径维护实例；启停只影响目标项，排序只交换记录。实际加载状态回传选项页，停止时也可配置已启用插件。
3. **补非模态窗口与退出处理。** 保持实例和消息循环，保证退出时顺序销毁；再按原版证据补 DFX／Dee HWND 集合与吸附。
4. **修正扫描与路径语义。** 探测失败保留现有配置并给出状态，同名不同目录严格区分；资源路径检查独立于 DLL 加载。
5. **处理注册表迁移。** 将路径／界面默认值与个人授权资料分离，禁止把整包注册信息作为所有音效的必需安装步骤。具体方案见专项文档。
6. **做真实插件回归。** 先五个可直接分析的 DLL，再对压缩 DLL 分别分析运行时接口；仍保持本地测试，不加入发行 Action。

若最终采用跨进程 DSP，需要把实际音频处理与配置一起放进同一个持久 worker，并处理 PCM 传输、延迟、状态、窗口和崩溃恢复；只把 Config 单独隔离并不能保持原版行为。对于当前还原目标，优先恢复原版同实例契约更直接。

## 10. 验证覆盖与未完成项

现有 `tests/winamp_dsp_runtime_tests.cpp` 使用 mock，覆盖回调参数、首个非空模块、分块、共享链互斥、旧 header、PaceMaker、Modify 异常和缺失 DLL。它不覆盖本报告发现的真实插件配置对象、非模态消息循环、注册表路径或真实资源加载。

本次只执行静态分析，不将上述 mock 的既有覆盖表述为 17 个真实插件通过测试。后续最小矩阵应包括：停止时勾选／配置、播放中即时调参、非模态窗口关闭重开、交换顺序保持状态、移除单个插件、44.1／48 kHz 与声道切换、主窗口最小化／托盘／退出，以及从不同安装路径启动。

本地分析脚本和完整 PE／导出／回调清单在 `tests/analyze_dsp_static.py`、`tests/artifacts/dsp_static/inventory.json`，不作为发行包或 Action 输入；文档不依赖这些文件才能阅读。此次没有修改生产代码或注册表文件。
