# mp3PRO 编码：原版 ttp_enc、重建 ttp_enc 与 fhtpro.flt 链路对照

日期：2026-11-17。本文按用户进一步指定的范围，**只分析 mp3PRO 编码链**，不把普通 LAME MP3、WMA 或输入解码 DLL 的行为混入。

## 1. 结论与对象身份

**原版和重建版 ttp_enc 都是 mp3PRO 编码的适配器，实际 MP3/SBR 内核在 fhtpro.flt 内。** 重建版没有重写该内核，也不是对原版 ttp_enc.dll 改名。

最重要的实质差异：

1. 原版 `Encoder::Open` 已经调用滤镜 Open，Start／Finish 均为空成功；真正 Close、排空与标签写入在最终 Release／析构时发生。
2. 重建版 Open 保存配置和路径，Start 才打开滤镜，Finish 显式关闭／排空并回写标签，析构仅兜底。
3. 原版依赖 Cool Edit 注册表目录、ANSI 路径与外部预设；重建版采用插件同目录滤镜、Unicode API 槽适配和会话预设修补。
4. 原版对 float64 原地量化、忽略滤镜消费数；重建版用私有 PCM16 缓冲，验证 chunk、消费数并监测写文件错误。
5. 两者都使用 32768 倍数把公开 float64 输入量化到 PCM16；不能把宿主内部 half-scale 域误写为这里乘 65536。

| 工件 | 路径（相对工作区根） | SHA-256 |
| --- | --- | --- |
| 原版适配器，163,976 字节 | `AddIn/ttp_enc.dll` | `c0f116ea6c27f4a5f5836ca1a425642f5cb301ed47f0f24f2dbad9016556b8ff` |
| 当前重建适配器，708,608 字节，p3 | `ttp_enc/build/Release/ttp_enc.dll` | `0cb48acc27fd11f40971bd201ed87adecc7243e45c068435fcc421a6db012599` |
| 原始压缩滤镜，237,568 字节 | `rebuild/tests/mp3pro_encoder_analysis/fhtpro.flt` | `1dc878b79b2a6729a0cc773731028ba49721dd6a70902963cf8bb21780388827` |
| 既有解包副本，872,448 字节 | `rebuild/tests/mp3pro_encoder_analysis/fhtpro.unpacked.flt` | `3e2505340fd2906b6696b7fc36677f89c2056a1e71d0ba3e6d74a1aed293e166` |

原版基址 `0x60250000`，重建插件和滤镜首选基址均为 `0x10000000`，**同样的数值地址不能跨 DLL 混用**。

源码版本：`ttp_enc` HEAD `408925a7b03e49fac0f84e835f14c09fb6eda0fc`；`rebuild` HEAD `46503402f1bbba677fb01045ceaec750f849b2a5`。

本轮使用 REA/Ghidra 12.1.4 直接检查原版／重建 DLL、接口表、指令及滤镜副本内部代码；未执行目标、未改配置／注册表／原始 DLL。标记：

- **B**：本轮指定哈希二进制的 REA 静态观察。
- **S**：当前源码。
- **H**：既有测试／伪码／样本。
- **I/U**：解释性推断／尚未验证。

**解包限制：** 内核内部地址直接绑定上述既有解包副本；本轮没有重新解包并逐区段证明其与压缩原件全部同源。原件的导出目录／哈希已直接检查，不将“副本可反编译”冒充“原件内核已完整恢复”。

证据包：`../artifact/mp3pro_encoder_chain_rea_20261117.evidence.json`，含复用的前一阶段证据及本次新增原版／滤镜观察。

## 2. 三者如何连接

```text
                         同一个 CoolEdit 滤镜 ABI
原版 ttp_enc.dll ───┐
                   ├─ OpenFilterOutput
重建 ttp_enc.dll ───┘  WriteFilterOutput
                      CloseFilterOutput
                              │
                       fhtpro.flt 内部
                  PCM → MP3/SBR 编码 → MPEG 文件
```

两条实际宿主链：

```text
原版：
ttpGetSoundAddIn → Enum(2) → Pro Creator → Create
 → Open(path, format) 【已打开滤镜／创建输出】
 → Start              【空 S_OK】
 → Write(Buffer)      【必要时原地 float64→PCM16，再按 chunk 写】
 → Finish             【空 S_OK】
 → 最终 Release/析构  【CloseFilterOutput 排空，随后宿主标签回写】

重建：
宿主转换源 → 效果／可选 SSRC → normalized float64
 → ttpGetSoundAddIn → Pro Creator → Create
 → Open               【验证输入，保存绝对路径、格式、配置快照】
 → Metadata::Set
 → Start              【占用单会话、修补预设、打开滤镜】
 → Write              【私有 PCM16 量化、按 chunk 写、检查消费／I/O】
 → Finish             【Close 排空、标签提交、错误返回】
 → 析构兜底 → 宿主提交临时输出
```

此处的 `Write` 是输入 PCM，不是直接交出一个压缩数据 Buffer；`fhtpro` 自己持有输出文件句柄。

## 3. 原版 ttp_enc.dll：逐层链路

### 3.1 DLL attach、枚举与可用性缓存

B：`ttpGetSoundAddIn@602531e0` 创建 AddIn 对象，虚表为 `6026e900`；`Enum@60253278`：

```text
index 0 → MP3
index 1 → WMA
index 2 且 DAT_60276b78 != 0 → mp3PRO
其他 → E_INVALIDARG
```

Encoder category：`{5954A909-9E02-4650-995E-F7535EE2BD8B}`。

`602537b7` 的 DLL attach 分支：

- 定位已加载的 `ttpctrl.dll`，否则 EXE，缓存 `CreateStdContent`／`CreateStreamOnFile`。
- 形成插件自己的 INI 路径并读取设置。
- 调用 `602522bf` 检查滤镜，结果缓存到 `60276b78`。
- 可用时才读取 `[mp3PRO] Options`。

所以原版 Pro 可用性不是每次 Enum 都重新发现；在 DLL 已加载之后才修复目录／注册表，不保证现有枚举结果立即变化。

### 3.2 注册表发现与两种配置文件

B：`6025235d → 60252c79 → RegOpenKeyExA` 打开：

```text
HKLM\SOFTWARE\Syntrillium\CEPro2
```

先查询 `MAINDIR`；查询失败才回退 `INIDIR`。`6025231c` 追加 `fhtpro.flt`；`602522bf` 检查文件存在且不是目录。

细节：

- 注册表访问掩码为 `0xf003f`，即 KEY_ALL_ACCESS，不是只读掩码；权限不足可能影响发现。
- 全链为 ANSI／MAX_PATH 风格。
- 此处 MAINDIR／INIDIR 是**原适配器定位滤镜目录**的逻辑。

原适配器自己的配置是 `60253a3a` 从本模块路径把末尾扩展名改为 `.ini`，通常为 `ttp_enc.ini`；DLL 若改名，配置基名也随之变化。

`coolmp3.ini` 则属于**滤镜自己的预设库**。原适配器不构建／修补其 22 字段，也没有重建版的 profile API 重定向。滤镜自身如何选择用户目录、迁移旧 INI，不能算作原 ttp_enc 的实现。

### 3.3 加载函数与 Options

B：`6025275b` 惰性 LoadLibraryA，缓存：

| 全局地址 | 导出 |
| --- | --- |
| `602769a4` | HMODULE |
| `602769a8` | FilterGetOptions |
| `602769ac` | OpenFilterOutput |
| `602769b0` | WriteFilterOutput |
| `602769b4` | CloseFilterOutput |

Open／Write／Close 必须存在；Configure 可以缺失。原适配器不在这里替换滤镜导入 API。

Options 原始静态默认值在 `60274988`，本轮读到 `07 00 00 00`，即 **7**。不是码率 7，而是传给滤镜的预设编号。

B：`60252703` 的配置调用：

```cpp
FilterGetOptions(parent, filterModule, 44100, 2, 16, options);
```

返回 0 → S_FALSE；非零 → 更新全局 Options、设置 dirty `60276b74=1`。插件在 DLL detach、且 dirty 时才把设置保存到 INI，不是配置确认就立即保存。

### 3.4 Creator／Encoder 虚表

Pro Creator 虚表 `6026e950`：

| 槽 | 地址／作用 |
| --- | --- |
| 3 Create | `6025366d → 60253679` |
| 4 Name | `602536bc` |
| 5 Extension | `6025341a` |
| 6 Configurable | `60253709` |
| 7 Configure | `6025370e → 60252703` |

Create 分配 `0x40` 字节，调用构造 `60252400`。

Encoder 虚表 `6026e58c`：

| 槽 | 地址／作用 |
| --- | --- |
| 0／1／2 | QI／AddRef／Release |
| 3 Open | `60252816` |
| 4 Start | **`60252910`** |
| 5 Finish | **`60252910`** |
| 6 Write | `60252915` |
| 7 Extension | `60254db6` |

`60252910` 的实际指令：

```asm
xor eax, eax
ret 4
```

即 Start／Finish 均只是 S_OK，不启动、不排空、不关闭。不要将普通 LAME 分支的收尾结论套给 Pro。

原对象主要字段：

```text
+00 Encoder vtable
+04 Metadata vtable（接口指针为对象+4）
+08 滤镜 HGLOBAL 输出句柄
+0c 复制的 WAVEFORMATEX*
+10 是否 float 输入
+14 保存的 UTF-16 输出路径
+18 标签名／值容器
+38 chunkBytes，初值 0x1200
+3c 引用计数
```

本轮读取的 QI 常量确认 Encoder IID 与当前 ABI 一致；Metadata IID 为 `{7AD84E00-5FEF-4481-B532-FBBD677E67C2}`，原 Pro 不暴露流式编码入口。

### 3.5 Open：验证后立即打开滤镜

B：`60252816`：

1. path／format 空 → E_POINTER。
2. 只接收 PCM16 或 IEEE float64；extensible 通过子格式 GUID 判断；声道必须 1 或 2。
3. 复制格式，记录是否浮点，保存宽字符路径。
4. 加载滤镜。
5. `60251cd7` 用 `WideCharToMultiByte(CP_ACP, flags=0)` 转输出路径，没有检查是否发生有损默认字符替换。
6. 直接调用：

```cpp
OpenFilterOutput(
    ansiPath, inputRate, 16, inputChannels,
    0, &instance.chunkBytes, globalOptions);
```

没有看到当前重建版对 valid bits、BlockAlign、AvgBytesPerSec 的同等级校验。返回句柄只按非零判断，未额外拒绝 HGLOBAL(1)，也未验证 chunk 非零、上限和帧对齐。

### 3.6 Write：原地量化，忽略滤镜消费数

B：`60252915` 获取 Buffer 槽 5 Data：

```cpp
if (floatInput)
    bytes = ConvertFloat64ToPcm16(data, data, bytes);
while (bytes) {
    n = min(bytes, chunkBytes);
    WriteFilterOutput(output, data, n); // 忽略返回值
    data += n;
    bytes -= n;
}
```

`60252fb0` 的 PCM16 分支：

```text
sampleCount = inputBytes / 8
integer = doubleSample × 32768 → x87 FISTP
饱和到 [-32768,32767]
覆盖原 Buffer 前部，返回 sampleCount × 2
```

**两个结论：**

- float64 是公开输入格式，不意味着滤镜获得 64 位精度；原适配器先降为 PCM16。
- 原版这里是 32768，不是 65536；x87 FISTP 的舍入受控制字影响。不能未经运行上下文就声称与新版显式半整数远离零、NaN 归零逐样本相同。

Write 尾部 `RET 0xc` 表明 ABI 仍含 self、Buffer、DWORD*；伪码只显示两个参数是类型还原不足。第三输出参数未在这段代码中使用。

原版推进指针不参考滤镜实际消费结果，也没有底层短写监测；chunk 若为 0 而输入非空，这段循环存在不前进风险。

### 3.7 真实收尾：Release → 析构 → Close → 标签

B：

```text
最终 Release
 → 析构包装 602524f7
 → 析构体 6025252f
 → CloseFilterOutput(HGLOBAL)
 → 有缓存标签且宿主服务存在：
      CreateStreamOnFile(savedWidePath, 0x42, &stream)
      CreateStdContent(stream, 0, &content, 0)
      content.Set(key,value)
 → 释放容器、路径和格式
```

MP3/SBR 排空循环在**滤镜 Close 内部**；原适配器只调用 Close。原 Pro 的 Release 确实会触发此收尾，不能误用“旧普通 MP3 Finish 为空”的结果声称 Pro 不 flush。

标签不是通过 FilterWriteSpecialData 传入滤镜。原适配器没有独立 ID3v2.3 后备，析构也没有 HRESULT 可以把标签失败反馈给宿主。

### 3.8 原版并发边界

原模块确有 ATL／窗口辅助 CriticalSection，但所查 `Configure → load → Open → Write → destroy` 主链没有编码会话 busy 或专用互斥。不能把窗口／线程列表的锁写成滤镜编码锁，也不能由每实例有 HGLOBAL 推导内核可重入。

这是有限静态观察，不是对所有 CRT／库内部锁的否定。

## 4. 重建 ttp_enc：保留 ABI，改写适配生命周期

### 4.1 宿主到插件

当前宿主 `rebuild/src/ui/playlist_transforms.cpp:1531–1696`：

- 创建 Encoder、获取输出扩展名，创建独立解码源。
- 源 PCM 经可选 ReplayGain／EQ／环绕、重采样，转为 normalized float64。
- 在目标同卷的 `.ttconvert-{GUID}` 临时目录 Open 编码器。
- Open 后、Start 前 SetMetadata；循环请求最多 64 KiB 源数据，调用 WritePcm。
- Finalize 后销毁编码器；所有操作成功才提交最终目标。

`ttp_enc/include/ttp_enc_abi.h:18–39` 保留原 Encoder 槽序：Open=3、Start=4、Finish=5、Write=6。Pro 仍是路径式，不是 StreamEncoder。

### 4.2 发现和配置

S：`ttp_enc/src/mp3pro.cpp:159–219`：

- 只从 **ttp_enc.dll 同目录**找 `fhtpro.flt`。
- 枚举阶段仅检查文件存在；实际加载／导出／API 槽兼容性延迟到配置或 Start 检查。
- 不要求 Cool Edit 注册表发现，也不要求 `flt.dat`。
- `[mp3PRO] Options` 默认仍为 7。
- 配置目录优先插件可写目录，否则 `%APPDATA%\TTPlayer\ttp_enc`。
- 配置确认即保存；不等待 DLL detach。

### 4.3 为什么新增会话预设修补

S：`mp3pro.cpp:175–200,223–234`：

```text
读取 coolmp3.ini / [Configurations] / Key<Options>
 → 不是22字段则采用重建默认Pro预设
 → 更新本次输入采样率／声道相关字段
 → 特定stereo→mono预设适配码率与声道字段
 → 写 Key2147480000
 → OpenFilterOutput(..., options=2147480000)
```

重建默认字符串：

```text
44100,2,22050,0,100,96000,2,1,1,0,1,0,0,0,0,0,44100,2,1,0,0,0
```

**原因不只是“方便保存配置”：** 滤镜配置解析会校验预设绑定的源 rate/channels；编号不存在或与源格式不符会退回滤镜默认选择，而裸滤镜默认可能是普通 MP3。原适配器只原样传 token；重建版主动重绑本曲格式以保留用户压缩选项。

特定 stereo Pro 96 kbps 预设适配 mono 时，当前代码减半码率；注释明确旧内核的 96 kbps mono 可能静默走普通 MP3。

22 个 CSV 字段映射并不等于连续 22 个结构字段：滤镜解析存在跳槽。本文不把未证实字段强命名，尤其不能仅见第三项 22050 就认定它必然是最终 MPEG 基础采样率。

### 4.4 格式、量化和块大小

S：`ttp_enc/src/encoder.cpp:93–107,229–247`、`pcm.cpp:47–85`：

- 接收 PCM8/16/24/32、float32/64、部分 extensible；1–2ch，8–192 kHz，验证帧对齐和字节率。
- 每轮最多 4096 帧，写入私有 PCM16 scratch，不覆盖调用方 Buffer。
- 浮点非有限值归零，削波，乘 32768，显式半整数远离零舍入；没有 dither。
- 再由 ProSession 按滤镜返回 chunk 切块。

**8–192 kHz 是适配器输入验证范围，不是滤镜支持范围。** 没有调用 FilterUnderstandsFormat／GetSuggestedSampleType2，也没有在 ProSession 中自动重采样；用户需要的 SSRC 属于宿主前置层。

真实历史 Pro96 的 chunk=`0x1200=4608` 字节；双声道16位时是1152个输入PCM帧。宿主64 KiB、量化4096帧、滤镜chunk是三个不同层级。

### 4.5 Unicode 输出和 I/O 错误如何接入

S：`ttp_enc/src/mp3pro.cpp:44–157`：

**滤镜仍自己写文件，重建插件没有给它增加一个 Open 参数式写回调。** 插件对滤镜模块内部 API 槽做适配：

| API | 作用 |
| --- | --- |
| GetPrivateProfileStringA / GetPrivateProfileIntA / WritePrivateProfileStringA | 将 coolmp3.ini 访问重定向到选定的 Unicode 可写路径 |
| CreateFileA | 将 `TTPLAYER_MP3PRO_OUTPUT.mp3` token 映射到真实路径的 CreateFileW |
| WriteFile | 保持系统写入，另记录目标句柄失败／短写 |

扫描普通 IAT 后，还查找压缩滤镜解包后 `FF15/FF25` 间接 call/jump 使用的原 API 槽，五类替换必须全部找到。卸载前恢复仍属于本适配器的槽。

因此“Unicode 输出”来自兼容层截获，不是旧滤镜天然升级为 Unicode；LoadLibrary 的初始化也早于 patch，不能据此宣称旧滤镜无配置／文件副作用。

### 4.6 Start、Write、Finish

S/B：Start：

```cpp
OpenFilterOutput(
    "TTPLAYER_MP3PRO_OUTPUT.mp3",
    inputRate, 16, inputChannels, 0,
    &chunkBytes, 2147480000);
```

拒绝 null／HGLOBAL(1)，要求一帧≤chunk≤1 MiB、且整帧对齐。

Write 对每块检查：

```text
consumed = WriteFilterOutput(handle, pcm16, n)
必须 consumed == n
必须无捕获到的目标 WriteFile 错误
```

注意返回值是**输入PCM消费字节数**，不是压缩输出长度。原版没有这些检查。

Finish：

```text
CloseFilterOutput → 记录／返回收尾I/O错误 → 释放busy
 → 重开输出写标签
 → 返回HRESULT
```

有宿主标签服务时继续委托宿主；无服务时支持独立 Unicode ID3v2.3 后备。真实错误可传播到宿主临时输出事务，不必等析构才发现文件是否可用。

### 4.7 p3 二进制已核对的对应地址

| 重建 DLL 地址 | 行为 |
| --- | --- |
| `10007d7a` | 同目录滤镜加载、导出解析、API槽适配 |
| `100085b4` | ProSession::start |
| `10008793` | ProSession::write |
| `10008817` | ProSession::finish |
| `10007928` | token → Unicode CreateFileW |

本表函数语义由当前源码与反编译交叉确认；不是原二进制中自带这些 C++ 名称。

## 5. fhtpro.flt：真正的编码与输出内核

### 5.1 API边界与清栈

原版和重建版传入的主要 ABI 一致，x86 stdcall：

```cpp
HGLOBAL WINAPI OpenFilterOutput(
    LPCSTR path, DWORD rate, WORD bits, WORD channels,
    DWORD arg5, DWORD* chunkBytes, DWORD options);
DWORD WINAPI WriteFilterOutput(HGLOBAL, const void* pcm, DWORD bytes);
void WINAPI CloseFilterOutput(HGLOBAL);
DWORD WINAPI FilterGetOptions(
    HWND, HMODULE, DWORD rate, WORD channels, WORD bits, DWORD options);
```

Open 的 bits→channels 与 Configure 的 channels→bits 次序不同；第五参数两适配器都传0，本文保留为语义未核定的 arg5。

Write 实现以 `RET 0xc` 清栈。自动伪码将多个导出还原为 void／将核心函数参数丢失，不能抄作真实函数原型。

### 5.2 Open：选预设、初始化上下文、打开文件

解包副本：

```text
OpenFilterOutput @1000d073 → 实现10018213
 → 1000fda9 / 跳板1000d0c8：按Key读预设并检查源格式
 → 1000f4fd：解析CSV配置
 → 1001fbc0：创建编码上下文
 → 10021920：选择压缩格式、采样率、声道／码率等
 → 10021d10：进一步编码参数
 → GlobalAlloc输入／输出缓冲
 → CreateFileA(path, GENERIC_WRITE, ..., CREATE_ALWAYS, ...)
 → 返回HGLOBAL与chunk
```

`1001fbc0` 直接检查的输入采样率集合：

```text
8000,11025,12000,16000,22050,24000,32000,44100,48000
```

这个集合仍不保证每个 rate／bitrate／mono／Pro 组合都有效。

Open 根据 Pro 模式选择 chunk：普通模式 `0x4800`，Pro模式 `0x1200`；配置失败可能在滤镜内部弹出错误窗口。重建版线程后台调用并不把滤镜变成无UI、可强制终止的沙箱。

### 5.3 Write：字节消费和内部浮点

```text
WriteFilterOutput @1000d096 → 实现10018a5c
 → 合并上次未消费尾部
 → 100203a0：编码调度
      → 10020ca0：输入消费／PCM16转内部float／必要时声道合并
      → 可选内部重采样／延迟缓冲
      → SBR支路
      → 基础Layer III支路
 → WriteFile(实际生成的压缩字节)
 → 返回本次已消费输入字节数
```

`10020ca0` 的16位分支明确把 signed short 转成 float；不是保留原宿主 float64 全精度。所以重建插件接收 float64，不代表最后是“64位mp3PRO编码”。

短输入、滤波状态和成帧需求可能使一次 Write 消费PCM却暂不产出压缩字节，尾部还需 Close 排空。

### 5.4 MP3/SBR 的汇合点

```text
100203a0 主调度
  ├─ 增强启用时：
  │    10024a00 → 1003b820及其helper
  │    全带分析／增强侧信息状态
  └─ 10023460 基础编码调度
       → 1002ee80 / 1002df70 / 1002f460 等helper
       → 1002d9f0 → 1004a970：增强payload长度／对齐预算
       → 1002f900：量化／位预算等调度
       → 1002cbb0：MPEG侧信息、压缩数据与扩展数据成帧
            → 1002d760：MPEG头位写入
            → 1004aa10：扩展描述／payload位写入
```

直接可见的事实（B）：

- `10023460` 向量化／成帧路径传递增强侧信息相关状态，不是完全独立地先生成一份最终普通MP3文件。
- `1004a970` 根据payload bit长度、额外字段和字节对齐计算占用；长度超过 `0xfe` 时使用扩展长度表示。
- `1002cbb0` 写12位／9位／8位等MPEG Layer III侧信息字段，调用压缩数据写入helper，并在同一成帧流程调用扩展写入。
- `1004aa10` 根据状态写4位类型 `0xc/0xd/0xe/0xf`，写8位长度、可选扩展长度与flag，再复制payload bit并补齐；不是在文件尾写ID3标签。
- 副本内可见 `relsbrenclib` 的 `qmf_enc.c`、`sbr_main.c`、`env_est.c`、`tran_det.c`、`nf_est.c`、`invf_est.c`、`code_env.c` 等来源字符串。

算法解释（I）：全带PCM分成基础低频编码和高频重建参数估计；基础层保持MP3兼容，SBR边信息随同一兼容码流携带。QMF／瞬态／包络／噪声等名称用于解释算法结构，**不等于本轮已给全部私有helper恢复原始函数名或数学公式**。

私有 `1003b820` 很大，完整静态 dossier 已保留，但native value-flow在默认3000 operations截断；未据此声称全部跨函数数据流或逐bit等价已验证。

### 5.5 Close：真正排空、修补头和关闭

```text
CloseFilterOutput @1000d064 → 实现10018d4c
 → 重复100203a0，直到无更多压缩输出
 → 每轮WriteFile
 → 可选旧ID3v1分支
 → 若需要信息帧，10021dd0生成／回写文件头
 → 10020370销毁上下文
 → CloseHandle／GlobalFree
```

可选滤镜ID3v1能力，不等于 ttp_enc 的 Metadata 通过这个分支写入。两个适配器自己的标签回写都发生在滤镜 Close 之后。

## 6. 原版／重建差异总表

| 项目 | 原版 ttp_enc.dll | 当前重建 ttp_enc |
| --- | --- | --- |
| 实际内核 | 外部fhtpro | 仍是外部fhtpro |
| 滤镜发现 | HKLM MAINDIR→INIDIR，attach缓存 | 插件同目录，枚举时文件检查 |
| 加载／输出路径 | ANSI，输出路径可能有损转换 | Unicode加载，输出token→CreateFileW |
| Options默认 | 7 | 仍为7 |
| 预设处理 | 原样传token | 22字段后备、源格式重绑、mono适配、会话键 |
| 配置保存 | dirty后DLL detach保存 | 配置确认即保存，可写目录回退 |
| Open | 已打开滤镜 | 保存状态 |
| Start | 空S_OK | 实际打开滤镜 |
| Finish | 空S_OK | 显式Close／标签／结果反馈 |
| 关闭时机 | 最终Release／析构 | Finish，析构兜底 |
| PCM入口 | PCM16／float64 | 更多整数／浮点格式 |
| float64量化 | 原地，x87 FISTP | 私有缓冲，明确舍入／非有限规则 |
| Write返回处理 | 忽略 | 要求consumed==n |
| chunk／句柄 | 初值0x1200，缺额外验证 | 拒绝HGLOBAL(1)，校验范围／对齐 |
| 磁盘错误 | 未见专用捕获 | CreateFile／WriteFile错误／短写监测 |
| 标签 | 析构委托宿主，无独立后备 | Finish委托宿主／ID3v2.3后备／错误传播 |
| 会话并发 | 主链未见编码级互斥 | Filter mutex＋busy，配置与编码互斥 |
| 输入Buffer | 浮点时覆盖 | 保持调用方输入不变 |

## 7. 验证边界与实际部署

1. **并非逐字／逐bit复刻。** 生命周期、输入覆盖、舍入规则、默认预设修补、错误传播都存在明确改变。同一内核不代表输出在任意配置和输入下逐bit相同。
2. **“mp3PRO编码器”可能输出普通MP3。** 裸滤镜默认或无效／格式不匹配预设可回退普通选择；当前重建默认字符串与裸滤镜默认不是一回事。
3. **最新插件并未部署到所有运行目录。** 根 `AddIn/ttp_enc.dll` 是原版；`rebuild/build/Release/AddIn/ttp_enc.dll` 是2026.11.10旧重建，哈希 `96b59e438a1b5edddd8b5bc480bed0ea94b4fe9ec2e55c9f5ca120475a91dc3f`；最新p3在独立工程Release目录。这三处旁边均未找到fhtpro，测试目录中的滤镜不会自动满足当前重建的同目录发现条件。
4. **不能宣称最新p3已经重跑Pro。** `ttp_enc/docs/VALIDATION.md:39–45` 明确本次p3验证未重跑mp3PRO；既有Pro结果属于更早构建。
5. **标签策略要分宿主。** 当前rebuild `CreateStdContent(write_type=0)` 落入APEv2写入路径，不自动采用用户全部MP3标签选项；不能把原版宿主标签策略验收直接算到当前rebuild。
6. **并发保护非万能。** 模块内busy不提供跨进程INI同步；同实例Write／Finish仍需串行。SetFilePointer／CloseHandle全部错误、超长输出和掉电没有完整保证。

已有真实滤镜Pro96样本：

```text
tests/mp3pro_encoder_analysis/pro96_without_dat/encoded.mp3
25,078 bytes
SHA256 5a066fa78b1a478e5f6919a551281ee11af1aaf7fb38e4d25b5b4f83347706ef
```

本轮帧头只读核对：MPEG-2 Layer III、22050 Hz、96 kbps；历史滤镜格式回读报告44100 Hz、Layer-3 Pro。它证明真实滤镜编码样本存在，**不是本轮运行编码，更不是高频重建音质／逐bit等价证明**。

## 8. 关键源码定位

相对工作区根：

- `ttp_enc/include/ttp_enc_abi.h:7–42`：槽序与IID。
- `ttp_enc/src/plugin.cpp:20–64`：工厂与Pro枚举。
- `ttp_enc/src/settings.cpp:158–195`：INI目录、Options=7、保存。
- `ttp_enc/src/encoder.cpp:93–107,142–252`：输入验证、状态机、量化批次和Finish。
- `ttp_enc/src/pcm.cpp:14–85`：PCM16量化规则。
- `ttp_enc/src/mp3pro.cpp:44–173`：五种API适配与加载。
- `ttp_enc/src/mp3pro.cpp:175–254`：预设、配置、Start／Write／Finish。
- `rebuild/src/plugins/plugin_manager.cpp:1869–1981`：宿主Encoder包装。
- `rebuild/src/ui/playlist_transforms.cpp:1445–1696`：输出事务与离线转换。

既有原版伪码输入哈希已与原件匹配：

`rebuild/tests/mp3pro_encoder_analysis/encoder/pseudo/ttp_enc.dll.pseudo.c`

其未收录某些短thunk不代表函数不存在；本轮用REA虚表读取和逐指令检查补齐了Start／Finish及Creator跳板。

## 9. REA证据索引

| 事实 | Evidence ID |
| --- | --- |
| 原版导出入口 | `ev_b1863c4c2b7e025cb20ae97b8158be7d369ec295dc3bd8dd0a7063c57fec667d` |
| 原版Enum、attach/detach、路径拼接 | `ev_a682dd4d964a4e63d490f7a807b33681880c450a2174e1352203c58042986357` |
| 原版注册表发现与Creator构造 | `ev_80c33e63b0efeedde78c0025fd942714a8d9d2ae19b8c739263796f06393087d` |
| 原版Creator虚表 | `ev_09643b1f31bcbf56158be50c602a796d683884e402551592e261cb7e93e387af` |
| 原版Options默认7 | `ev_e00534f1bebf37d91f2f4697533813e0a1a61bb7ca76b839026ea50d373287be` |
| 原版滤镜加载 | `ev_feae777372797546924933b0c5bb1e46451ccad1ff78becd4c2d33d86117e9e9` |
| 原版对象、Open、Write、析构标签链 | `ev_ea0c2cd300d83f6b906126b1293321da8917127130f45625dc7afa9794a2a8cf` |
| 原版Encoder虚表 | `ev_194122ffd9ee5ceb5f348987da728b7bc2c36ecdbda24ee4555abfac21466fd0` |
| 原版Start/Finish的XOR／RET | `ev_e358e8c997d0adc989c01133bc4e38babf10eac378264b06f958fb0f2f0a453c`；`ev_ac44164ada34296c0af8c23425397eef2641225a579b9802449c6aeccff8c83b` |
| 原版格式识别与float64转换 | `ev_b9ad1bba88af51b3eb379f63d24f0359b518c3db93ac760ce73e3ca39b94d9f0` |
| 原版配置UI、工厂创建、ANSI路径转换、RegOpen | `ev_753203087ed4dd598096789e86870fef70fe5f7525d9c01ea252844332a02495` |
| 原版Write忽略返回／RET0xc | `ev_2cba43ef09630c3ce4fe82f25e2b914003c84ef16d3f5ff4a15e6358e3c0211a` |
| 原版x87 FISTP | `ev_aee29fe36bb452cfea63fe1f5f5fbc9962a9f949a5422e2ff87f55692ead1b86` |
| 重建Start与Unicode输出 | `ev_b8c42a1667610da98bb8cbb57466fef574b1a7d3c6b1c8e5b2cb4088e58323d0` |
| 重建加载、Write、Finish | `ev_a4ae37ed0d68e5bfe1311eace6454c6bd6bd5a7038870baac989d1d5b46984c3` |
| 原始压缩滤镜导出 | `ev_89aa684130e7c6bd9247bf64392d2fc562f293223f2b66df7e6aca4088633f83` |
| 滤镜副本Open／Write／Close | `ev_97270750de9b9523ad53d1a347f9ea4cd1cc3e6c7cbd8de0d73988908621010f` |
| 滤镜预设选择、核心调度与配置 | `ev_68e25ea4c48071d70457ee8d71c060b8b1012abc8816982db9a986dd238a5379` |
| 滤镜基础／增强调度 | `ev_3059252aa2a970994e5fc5f856d85e95491eb53beab8a95a86fd47f5fbfb262d` |
| CSV解析、输入消费、量化与成帧完整batch | `ev_668cc168fc367a389d3433f0c61ff7439e4831d3535f5371d3660f2e5bf173e8` |
| MPEG成帧／侧信息文本 | `ev_a5adbf54de5e7dee04e2ad4c4e196d67be32af15a10b0bc0a0ba1a03433e907d` |
| 扩展长度／对齐预算 | `ev_9297a10c3d25ad44b603bea47064b7c4f0707655633200ed024c787b5b778c8e` |
| 扩展类型、长度、payload位写入 | `ev_6886e87d34e17794ed353d39295c6f546fee1990406bdd89b950a7618a19f84c` |

所有函数语义均限于已检查调用链；没有把静态调用图当成运行轨迹，没有将未恢复的私有类型、全内核数学语义或任意编码配置等价性描述成已证明。

导出包共109条Evidence、4项Unknown（含前阶段复用项），5,587,282字节；SHA-256为 `4c8181badd4b01bf3797500cd530d7a936e395fa6b6fc58d591d8932e24d6336`。本表24个不同Evidence引用均已核对存在。编码逐bit等价待验证项为 `unk_3b2daea84e2364198717fba93fd047110237ddaf9d3118ed8938d188a88297bd`。REA引擎已关闭，调查已保存并reset。
