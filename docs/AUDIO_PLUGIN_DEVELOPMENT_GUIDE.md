# 音频解码插件开发指南：供后续 Codex 新建插件时使用

更新时间：2026-10-20。依据本工作区的源码、构建脚本和实际测试整理。表中的版本是**仓库当前固定版本**，不表示已重新查询上游最新版。

## 1. 开始前先读什么

本工作区根目录是 `D:\Projects\Backup\TTPlayer`。`rebuild`、`ttpcomm`、各个 `ttp_*` 和 `rebuild/tests` 是不同 Git 仓库；不要把它们当成一个仓库提交。

推荐阅读顺序：

1. 本文：选择模板、了解旧 ABI 和交付要求。
2. `ttp_<目标>/include/ttp_<目标>_abi.h`、`src/plugin.cpp`：导出、工厂和接口顺序。
3. `src/reader.cpp`、流适配、标签文件：实际读取和保存语义。
4. `CMakeLists.txt`、`cmake/legacy_windows.cmake`、`build.ps1`、`.github/workflows/build.yml`：独立构建和发行。
5. `ttpcomm/sdk/AUDIO_FORMAT_PROBE.md`：新增格式识别协议的正式约定。
6. 播放器的 `src/plugins/plugin_manager.cpp`、`src/audio/audio_engine.cpp`、`src/audio/extension_correction.cpp`：宿主如何使用插件。

先检查各仓库 `git status --short` 和适用的 `AGENTS.md`。工作区中已有用户修改或已暂存修改时，只改本任务需要的代码，不整体回退，也不把测试数据顺手提交到播放器仓库。

## 2. 现有仓库的职责与模板选择

| 仓库 | 当前实现和依赖 | 适合参考的部分 | 不宜直接照搬的部分 |
|---|---|---|---|
| `ttp_aac` | FAAD2 2.11.4；原生 MP4 解析；AAC Reader、MP4 Reader、Decoder、Nero Encoder 工厂 | 一个 DLL 注册多个 Reader；容器与编码分离；压缩格式交给 Decoder | Nero 配置依赖旧编码组件；MP4 不是“把所有音频当 AAC” |
| `ttp_ogg` | libogg 1.3.6、libvorbis 1.3.7、Opus 1.6.1、opusfile 0.12 | **通常首选模板**：单 Reader、多编码、标签、图片、显式提交、独立构建 | Opus 的 pre-skip、尾部裁剪、增益、映射与 Vorbis 不同 |
| `ttp_flac` | libFLAC 1.5.0；恢复的 TTA 实现 | 两个工厂；无损 PCM；多值标签、封面、统一保存 | TTA 历史浮点分支和外部标签规则不是通用模板 |
| `ttp_ape` | Monkey's Audio SDK 13.30 | Reader + Encoder；APEv2；读写流与可靠保存 | APL 指向外部文件，不能把 APL 的后缀改成 APE；还有额外历史导出 |
| `ttp_webm` | 自有 EBML/WebM 容器处理，静态 Vorbis/Opus 核心 | **新容器首选模板**：`document`、`ebml`、`codec`、Reader 分层；容器标签/图片保存 | DocType、lacing、SeekHead/Cues、时间基等必须按目标容器重新实现 |
| `ttp_ac3dts` | a52dec 0.8.0、libdca 0.0.7 | Reader 与 Decoder 独立；原始帧同步、字节序和声道 | 返回压缩数据时必须有配套 Decoder；DTS-WAV 不能仅凭内部 DTS 同步字改后缀 |
| `ttp_mod` | 默认 libmodplug 0.8.9.0；可选 libopenmpt 0.8.9 | 多子格式；旧引擎与现代引擎构建切换 | 无签名 MOD 不宜自动改名；换引擎会改变混音和播放行为 |
| `ttp_mpc` | Musepack r475 | 小型 Reader、SV7/SV8、APEv2/ReplayGain | SV7 与 SV8 的识别和标签位置不同 |
| `ttp_asf` | 自有 ASF/MP3 路径；Windows Media SDK、DMO 等系统组件 | 容器 Reader 与系统 Decoder 配合；动态探测系统能力 | 不能把系统编解码器可用性视为所有 XP 都相同；ASF 内部不只 WMA |
| `ttp_rm` | 裁剪的 FFmpeg 9.0.2；原生 RM 标签实现 | **使用 FFmpeg 时的模板**：自定义 AVIO、错误桥接、最小解码配置 | 不要启用全部 FFmpeg 组件；RealMedia 写回不是通用文件重写 |
| `ttp_tak` | 裁剪的 FFmpeg 9.0.2；TAK 标签适配 | 单格式 FFmpeg 解码、APEv2、静态发行 | 不要重新引入旧 `tak_deco_lib.dll` 依赖 |
| `ttp_dts` | 裁剪 FFmpeg 9.0.2 DCA；独立 DTS / DTS-WAV Reader | 压缩流包装在 PCM 容器中的识别、保留容器后缀、缺失解码器保护 | 原版 WAV 工厂优先级另行验证；DTS-HD 待真实样本；只读标签 |
| `ttp_dsd` | 裁剪 FFmpeg 9.0.2 DSD/DST、swresample | DSF/DSDIFF 转 PCM、有效样本裁剪、FIR 历史与准确定位 | 不是 DoP/原生 DSD/ISO；DST 待真实样本；只读标签 |
| `ttp_vqf` | 裁剪的 FFmpeg 9.0.2；TwinVQ 标签适配 | TwinVQ 容器、旧编码支持、源代码附件生成 | 不要重新依赖旧 `tvqdec.dll` |

这些 DLL 都在当前 x86 工具链上构建。多数解码核心以 SSE2 为基线，**支持 XP 不等于支持所有 XP 时代 CPU**。

### 选择原则

- 简单单格式 PCM 解码：从 `ttp_ogg` 或 `ttp_mpc` 选择较接近者，保留 ABI、流和发行骨架，删除无关格式业务。
- 容器内可能有多种编码：优先看 `ttp_webm` / `ttp_aac`；单独设计容器、轨道和解码器对象。
- 使用 FFmpeg：优先看 `ttp_tak` 的精简配置，再参考 `ttp_rm` 的错误传播和标签逻辑。
- 需要编码器：参考 `ttp_ape`；不要把 Encoder 方法追加到 Reader 的虚表。
- 重建旧 DLL：以目标 DLL 自己的伪代码和二进制为 ABI 基准；模板只能提供工程结构。

## 3. 建议目录结构

```text
ttp_new/
  CMakeLists.txt
  build.ps1
  README.md
  .gitignore
  .github/workflows/build.yml
  include/
    ttp_new_abi.h
    ttpcomm/audio_format_probe.h
    ttpcomm/audio_format_probe_impl.h
  src/
    plugin.cpp                 # DllMain、入口、AddIn、工厂
    reader.cpp                 # 原版 Reader 适配，非整个容器实现
    common.h                   # HRESULT/异常边界、COM 引用、字符串分配
    io.h / stream_io.h          # IStream、短读、定位、取消
    document.* / container.*   # 复杂容器可拆分
    codec.* / decoder.cpp      # PCM 解码或独立 Decoder
    metadata.* / pictures.*    # 标签和封面
    ttp_new.def
    version.rc.in
  cmake/
    codec.cmake                # 固定版本、URL、SHA-256
    legacy_windows.cmake
    size_release.cmake
    check_legacy_imports.py
    version.cmake / version.ps1 / write_version.ps1
    resolve_release_version.ps1 / prepare_release.ps1
    build_ffmpeg.py             # 仅需要 FFmpeg 时
    package_source.py           # 当前发行方案需要源码附件时
  docs/
    ABI.md / RECONSTRUCTION.md / DEPENDENCIES.md
  licenses/ 或 LICENSES/ 或 docs/licenses/
  third_party/                 # 来源、许可证；完整源码是否存放按仓库现有方案
  build/ artifact/             # 忽略，不提交生成物
```

现有仓库的许可证目录不完全统一；新建时选一种清晰结构，并更新 install、发行脚本和说明。不要为“统一目录”擅自移动其他仓库文件。

测试代码、样本和测试驱动放在 `rebuild/tests/<新插件任务>/` 或 `rebuild/tests/audio/`。输出放 `rebuild/out/<任务>/`。播放器 Action 不运行这些测试，也不将测试 EXE、样本或报告加入发行包。

## 4. 原版 ABI 必须保持的约定

### 4.1 导出与工厂

常见入口为 `.def` 中的 `ttpGetSoundAddIn=_ttpGetSoundAddIn@4 @1`。以目标原 DLL 为准，保留额外导出和序号。进程内接口为 x86 `__stdcall`；不要在 ABI 上传递 STL、C++ 异常或由另一 CRT 释放的对象。

AddIn 在 IUnknown 的 0～2 槽后，通过槽 3 枚举 `(index, category, object)`。ReaderCreator 的槽 3/4/5 分别是 Create、Name、Extensions。原版的分类 GUID、枚举顺序、返回串格式和 `S_FALSE` 含义都要核实。

常用 GUID：

- Reader category：`476D15A5-D863-416A-8A59-A9C7D72CE04E`。
- Reader interface：`30C7C165-C0A9-4204-995D-456A764998FB`。
- Metadata：`7AD84E00-5FEF-4481-B532-FBCD677E67C2`。
- Thumbnail：`B5E770AF-DFB0-43E5-9B0C-3EE98E7B6248`。
- 可选 MetadataCommit：`3AB643C1-D8A4-4D49-9CBB-EF196B04B6E7`；实际常量以 ABI 头中的 `iid_metadata_commit` / `kMetadataCommitInterface` 为准。

新增能力用独立 IID 的 QueryInterface。不要修改旧接口的方法顺序，也不要给原版对象改 IID。所有接口返回同一个规范 IUnknown 身份；每次成功 QI 都增加引用，调用方负责 Release。

### 4.2 Reader 槽位

| 槽位 | 职责 |
|---|---|
| 0～2 | QueryInterface / AddRef / Release |
| 3 | Open(IStream*, flags) |
| 4～5 | Capabilities / Duration |
| 6～10 | Format / BufferSize / CodecName / Bitrate / BitDepth |
| 11～13 | SetCallback / Start / Stop |
| 14～15 | Read(Buffer*) / Seek(milliseconds*) |

宿主播放通常传 `flags=3`；信息读取传 `0`。位 0 允许解码，位 1 请求浮点输出。不能只把 WAVEFORMATEX 的 tag 改成浮点，却仍写整数；也不能发生原 AAC 的“数据为 float、格式却标成 PCM”问题。

Reader 可以输出 PCM，也可以输出压缩格式由匹配的 Decoder 解码。`CodecName()` 是显示字符串；`Format()` 可能已经是 PCM；二者都不能作为文件真实后缀的判据。

### 4.3 内存和流

- 返回给宿主的字符串、格式使用 `CoTaskMemAlloc`；宿主 `CoTaskMemFree`。格式包含 `sizeof(WAVEFORMATEX)+cbSize` 全部数据。
- Read 先检查容量，返回长度不得超过容量。PCM 必须按 block align 对齐；不能只依赖 Release 下可能被关闭的 `assert`。
- 正常 EOF、短读、格式不支持、损坏、取消、内存不足分别返回对应结果。`E_ABORT` 等取消/传输错误不能改成 EOF。
- 网络流和归档成员是 IStream。不要用 Stat 的显示名再打开物理文件，从而绕过取消、解密、缓存或只读约束。
- 只有确认可以保存和恢复位置时才能探测。非 seek 流保留流式读取路径。Clone 失败不表示允许擅自消耗原流。
- Seek 要清理解码器缓冲、预卷和 EOF 状态；时间转换检查溢出；Opus/WebM 的裁剪不能用 PCM 原始帧数简单替代。
- QueryInterface、Read、Open 的边界要拦住 C++ 异常并转 HRESULT；不能依靠播放器 SEH 来掩盖插件越界。

## 5. 新增格式识别接口

正式头文件来自 `ttpcomm/sdk/include/ttpcomm/`。独立插件携带同内容副本到 `include/ttpcomm/`；播放器副本在 `third_party/ttpcomm-sdk/include/ttpcomm/`。这只是头文件协议，**没有新增 ttpcomm.dll 导出或强制运行时依赖**。

### 工厂 Probe

工厂实现 `ttp::format::Probe`，IID `7E892943-481A-43DB-A89B-5993F50B6049`。宿主统一读取有界文件头，再询问所有支持该 IID 的 Reader 工厂。Probe 本身不打开路径、不 seek、不联网、不解码整首歌、不弹窗、不改文件。

已有识别规则可复用 `ProbeSupport` 和 `ProbeFamilies()`；全新格式可以自行实现 `ProbeFormat`，不必为了一个新插件修改所有旧插件。

```cpp
class NewFactory final : public ReaderCreator, public ttp::format::ProbeSupport {
    DWORD ProbeFamilies() const noexcept override { return ttp::format::Flac; } // 示例
    // QI 中返回 Probe*，成功 AddRef；旧 category / IUnknown 分支保留。
    // 同一个最终类必须覆盖所有 IUnknown 基类的 AddRef / Release。
};
```

尤其注意 AAC 这种 `FactoryBase<ReaderCreator>` 多重继承：继承另一个基类的实现并不自动满足新增 IUnknown 基类的纯虚函数，需要显式转发 AddRef/Release。

### Reader SourceInfo

Reader 实现 `ttp::format::SourceInfo`，IID `B58EE1A2-812C-4920-920E-5BE9379F3132`。Open 成功之后报告**外层容器**、实际编码、建议后缀与合法别名。未打开返回 `E_UNEXPECTED`，无法可靠判断返回 `S_FALSE`。

```cpp
HRESULT Open(IStream* stream, DWORD flags) override {
    const auto probed = PrepareFormatInfo(stream, supported_families);
    if (FAILED(probed)) return probed;  // 包括恢复流位置失败
    const auto opened = OpenImpl(stream, flags);
    if (opened == S_OK) SetSourceCodec(actual_codec_from_parser);
    return CompleteFormatInfo(opened);
}
```

此例使用 `InfoSupport`。新格式也可自己保存结构化结果并实现 SourceInfo。不要凭扩展名把未知头部填成确定格式。

### 置信级别和边界

- `NoMatch`：没有证据；`NeedMore`：前缀不完整；`Tentative`：弱证据；`Strong`：结构性头部匹配；`Opened`：读取器已成功打开。
- Strong 仅用于路由。单凭同步字不能提示更名。MP3/ADTS 要核对连续帧；Ogg 要区分 Vorbis/Opus；EBML 要验证 WebM DocType。
- MP4 内是 AAC 仍建议 `.mp4`，接受 `.m4a` 等别名；WebM 内是 Opus 仍建议 `.webm`，接受 `.weba`。
- APL、无可靠签名的 MOD、未知容器保守返回无结果。
- 压缩包内歌曲更名必须交给宿主的外层压缩包逻辑；OSZ 不是错误后缀 ZIP。

本轮共用实现通常读 8 KiB，Ogg 必要时扩展至最多 64 KiB；合法 ID3 前导最多跳过 1 MiB。未知长度或不可恢复位置的流不做新的前缀采样，继续原有流式路径。不要宣称有界识别能覆盖任意巨大标签、任意私有封装或所有无签名文件。

同步并检查副本：

```powershell
python D:\Projects\Backup\TTPlayer\ttpcomm\tools\sync_audio_format_sdk.py
python D:\Projects\Backup\TTPlayer\ttpcomm\tools\sync_audio_format_sdk.py --check
```

## 6. 标签、图片与保存

在“可播放”之外单独列出：只读标签、修改、删除、图片枚举、图片修改、可靠提交，各项不要互相推定。

- Metadata 保留字段大小写处理、多值、未知字段、编码以及删除语义；空值和删除不一定等价。
- Picture 保留 MIME、描述、类型、二进制长度；验证解码器与原版宿主对最大图片大小的限制。
- 能力位如 `0x4` 的可写含义必须符合实际。只读流、归档成员、网络流不能冒充可写。
- 原版通常在 Reader 最终释放时落盘，旧接口没有保存结果。可选 Commit 返回真实结果，并且只执行一次；提交后的状态和失败后的重试方式明确。
- 采用临时文件写入、校验、替换等现有事务路径，保留未知块；处理源文件被替换、分享冲突、短写和失败后清理。
- 不把新接口当成在压缩包里写回标签或修改用户歌曲的授权。本项目压缩包继续只读。

## 7. 构建、依赖和发行

### 独立构建

所有目标使用 MSVC + Win32；CMake 校验 `CMAKE_SIZEOF_VOID_P == 4`。现有插件一般 C++17，播放器 C++20。不要无意让插件依赖播放器的 C++20 或整个源码树。

```powershell
cmake -S D:\Projects\Backup\TTPlayer\ttp_new -B D:\Projects\Backup\TTPlayer\ttp_new\build -G "Visual Studio 18 2026" -A Win32
cmake --build D:\Projects\Backup\TTPlayer\ttp_new\build --config Release --target ttp_new --parallel 4
```

或者使用该仓库 `build.ps1`，先读参数。替换模板中的 target、输出名、版本变量、资源字符串、`.def`、工厂名、下载地址、release 脚本函数名和 workflow 分组；用 `rg` 查找遗留旧插件名。不要全局替换 ABI GUID 或上游库名。

### 旧系统和体积

- 复用固定 VC-LTL 5.3.1 与 YY-Thunks 1.2.2 构建配置，XP runtime、x86 thunk object 和 5.01 subsystem 一起配置。
- 使用 Release、`/Os`、`/Gy`、`/Gw`、`/GF`、链接删除未用代码及现有 size_release 设置。体积优化不得改变 PCM 格式、精度或检查行为。
- POST_BUILD 导入表检查必须通过；仍需在 XP/Win7 真机或虚拟机实际 LoadLibrary、Open 和 Read。
- 对 ASF 这样的系统解码路径，单独记录 WMP/DMO 等运行环境限制。导入检查不等于系统编解码器完整。

### 固定依赖

下载固定版本及 SHA-256，允许离线源码路径时同时检查版本。上游变更需要先评估工具链、CPU、线程、浮点、时长和标签变化，不能只改 URL。FFmpeg 模板通过 Git Bash/MSVC 配置裁剪的 demuxer/decoder，不能复用与另一插件配置混淆的生成目录。

许可证保存在仓库，不嵌入 DLL。当前 AAC、RM/TAK/VQF 等发行脚本还生成独立 source 附件；不要为了让二进制 ZIP 更小而删掉现有源码附件流程。

### Action 与播放器打包

- 独立 `.github/workflows/build.yml` 的 `release_a_version` 决定是否发布；日期版本在编译前固定，通常为北京时间 `yyyy.MM.dd[pN]`。
- 文件版本、产品版本、tag 和 ZIP 名保持一致。不是编完后再改 ZIP 文件名。
- 运行 ZIP 通常为 `ttp_new-版本.zip`，其中仅 `AddIn/ttp_new.dll` 和 `SHA256SUMS.txt`。源码包、许可证说明和 release notes 按仓库既有流程另行发布。
- 播放器 `cmake/package_addin_bundles.py` 的 `BUNDLES` 是独立 GitHub 仓库到插件名的映射；新增插件需登记真实仓库、资产名和校验要求。
- 本地改完 DLL 不等于播放器 Action 已拿到它。插件正式 release 发布后，播放器“最新稳定版插件包”流程才会下载新 DLL。

## 8. 最低测试矩阵与证据

### 必测内容

1. ABI：导出名/序号、枚举、旧槽位、QI 身份、引用计数、新 IID 缺失时的兼容路径。
2. 播放：整数/浮点、单/双/多声道、小于一帧缓冲、非整帧容量、短读、EOF、损坏帧、取消、重复 Seek。
3. 识别：正确后缀、无后缀、未知后缀、伪装成另一音频后缀、合法别名、仅假同步字、巨大/损坏 ID3、截断容器。
4. 信息：标签读取与播放选用同一工厂；不为检测修改文件；成功打开前不返回 Opened；源文件版本变化后不采用旧更名结果。
5. 保存：Unicode、只读、删除、多值、未知块、短写、锁冲突、显式 Commit、原版最终 Release 保存。
6. 原版宿主 ABI 和实际原版 GUI 测试分开记录。通过前者不能称已完成后者。
7. XP SP3、Win7、Windows 11 同一产物验证；Windows 10 若无环境明确标注，Windows 11 只能作为替代环境，不能声称在 Win10 实机运行过。

测试用显式 Check/Require 并返回非零退出码，设置 `SEM_NOGPFAULTERRORBOX` 防止测试崩溃留下阻塞弹窗；仍保留错误日志。对预期损坏文件设置超时。只在复制的样本上修改标签或更名。

### 本轮可复用测试入口

```powershell
cmake -S D:\Projects\Backup\TTPlayer\rebuild -B D:\Projects\Backup\TTPlayer\rebuild\build -DBUILD_TESTING=ON
cmake --build D:\Projects\Backup\TTPlayer\rebuild\build --config Release --target audio_format_probe_tests extension_correction_tests extension_mismatch_tests --parallel 4
python D:\Projects\Backup\TTPlayer\rebuild\tests\audio\format_probe_matrix.py --prepare
python D:\Projects\Backup\TTPlayer\rebuild\tests\audio\format_probe_matrix.py
```

生成包在 `rebuild/out/format-probe-20261020/bundle`。新任务应使用 `--bundle` 指向自己的输出目录。矩阵使用工作区已有样本，不会自动下载，也不修改原始样本；首次缺样本时应提供同编码的本地测试文件。

VirtualBox 的 `guest copy_in` 目录复制在本环境**不递归复制子目录**。复制 bundle 后还要分别复制 `AddIn` 和 `samples`，再运行 `run.cmd`。读取并保存 `result.txt`，检查 `FAILURES=0`，不要只把工具返回 `(ok)` 当成测试通过。

## 9. 推荐实施顺序

1. 核对目标旧 DLL 的工厂、ABI、外部依赖和格式范围；先写差异表。
2. 建独立工程，完成 LoadLibrary、入口、枚举、Create、QI 和释放测试。
3. 完成 IStream 适配、Open、PCM、容量检查、EOF、错误和定位。
4. 接入 Probe/SourceInfo，测试错误后缀、别名、容器边界及旧插件回退。
5. 补标签、封面、可写能力、Commit 和原版最终 Release 行为。
6. 接宿主信息读取/播放/增益扫描；保持网络与归档只读路径。
7. 完成旧系统、原版宿主与播放器对照，再优化体积。
8. 完善独立 Action、校验、来源许可证、必要源码附件、播放器打包登记和实施文档。

### 可直接给下一次 Codex 的任务说明

> 先阅读 `rebuild/docs/AUDIO_PLUGIN_DEVELOPMENT_GUIDE.md` 与 `ttpcomm/sdk/AUDIO_FORMAT_PROBE.md`，检查工作区当前修改。为 `ttp_new` 创建独立 x86 解码插件工程，选用最接近的现有仓库结构，但以目标原 DLL 的接口与行为证据为准。保持原版槽位、导出、引用计数和内存所有权；接入可选 Probe/SourceInfo。按播放、定位、标签、图片、保存分阶段完成并验证，不宣称未测试的兼容性。测试放 `rebuild/tests`，Action 不运行测试，依赖固定版本与校验值，许可证保留在仓库；输出真实构建、测试和剩余差异记录。
