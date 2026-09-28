# AddIn 音频插件与重建版专属 DLL 的列表存储分析

分析日期：2026-09-28。

范围：`reverse/decompiled/TTPlayer.exe.pseudo.c` 对应的原版，以及当前重建版源码。本文分析加载和记录机制；没有启动播放器或执行第三方 DLL，也不把磁盘上的文件名单当作本次运行成功加载的名单。

## 1. 结论

| 要确认的内容 | 原版 | 当前重建版 |
| --- | --- | --- |
| 是否保留成功识别的 Sound AddIn 模块记录 | 有，保存在进程内的模块表 | 有，`loaded_modules_` 与 `plugins_` |
| 是否记录模块提供的音频读取、解码接口 | 有，分开的 Reader/Decoder 注册表 | 有，`reader_factories_` / `decoder_factories_`，并带模块路径和枚举索引 |
| 是否有加载失败记录 | 本次追踪的扫描流程没有独立失败记录表；失败清理后继续枚举 | `PluginInfo` 记录多种加载、入口和实例创建失败，但不是所有组件的完整诊断表 |
| 是否有重建版专属 DLL 统一列表 | 不适用 | 没有；语言、HTTPS、皮肤、MAKI 分别管理 |
| 是否将成功加载清单持久化到 XML/JSON/注册表 | 未在相关加载、关闭和配置路径发现 | 未实现统一持久化加载清单 |
| 能否凭上述名单确认某首歌曲可播放 | 不能 | 不能，还需要实际创建 Reader/Decoder、初始化并读取音频 |

这里的“注册表”指程序内保存接口的表，不是 Windows 注册表。

## 2. 原版的实现

### 2.1 扫描并登记模块

核心调用链：

```text
CSoundLibrary_Initialize / 004CA48E
  ├─ 注册内置接口
  ├─ 004C8421：将 AddIn 目录追加到进程 PATH
  ├─ 004CABC0：枚举 AddIn\ttp_*.dll
  │    ├─ 创建 SoundAddInModule 包装对象
  │    ├─ 004C88D9：LoadLibraryW → ttpGetSoundAddIn → 实例
  │    ├─ 004C8954：按索引调用 AddIn 枚举接口
  │    ├─ 按类别登记 Reader / Decoder / Encoder / LyricSearch
  │    └─ 004CDCB8：将被识别的模块包装对象追加到模块表
  ├─ 整理格式、过滤器等信息
  └─ 004CB0F1：释放接口缓存及模块加载引用，保留登记描述
```

`004CABC0` 使用 `FindFirstFileW` / `FindNextFileW`，跳过目录。它不会把 AddIn 下所有 DLL 都当作音频插件：文件名先要匹配 `ttp_*.dll`，然后要有正确入口、实例和可识别类别。

扫描时的 `bVar3` 表示枚举到了已知类别；若没有任何已知类别，则走 `E_NOTIMPL` 分支。加载或创建失败进入清理分支，`Catch_004CB0DC` 返回继续枚举的位置。因此原版的模块表也不能理解为“每种音频能力均已实测通过”。

### 2.2 实际保存的是五张相关表

结合扫描插入函数、`004CB0F1` 的遍历和 `CSoundLibrary_Shutdown / 004CA828` 的销毁顺序，可以恢复以下结构关系。名称是按用途整理的说明名，原始源码字段名未恢复。

| SoundLibrary 内相对位置 | 内容 | 动态插件的插入函数 |
| --- | --- | --- |
| `+0x00` | ReaderCreator，文件读取/格式处理接口 | `004CDA07` |
| `+0x10` | DecoderCreator，压缩音频解码接口 | `004CDAD6` |
| `+0x20` | EncoderCreator，编码接口 | `004CDB93` |
| `+0x30` | LyricSearch，歌词搜索接口 | `004CDC47` |
| `+0x40` | SoundAddInModule 模块包装对象 | `004CDCB8` |

`004CD743` 初始化公共接口描述，保存模块关联、枚举索引、接口缓存及说明数据；具体类别再补充格式描述等字段。模块包装对象负责 DLL 和 AddIn 实例的生命周期。`004C88D9` 可直接看到 `+0x1C` 的模块句柄和 `+0x20` 的 AddIn 实例。

这解释了两个数量差异：

- 一个 DLL 可以贡献多个接口，并可同时提供读取、解码、编码等类别；接口数量不等于 DLL 数量。
- 注册表还包含内置接口；接口数量也不能直接用作外部 DLL 数量。

### 2.3 登记过，不等于一直驻留内存

原版初始化末尾调用 `004CB0F1`：先释放四类接口的缓存，再调用模块的 `004C8A0F`，释放 AddIn 实例和 DLL 引用。后者经 `00413E17` 调用 `FreeLibrary` 并清空句柄。

模块描述和类别描述此时仍然存在。后续获取接口时，例如 `004CD79E`，会通过 `004C8954` 再调用模块；若 AddIn 实例不存在，则再次进入 `004C88D9` 创建。

因此原版保留的是“已发现、识别并可用于后续创建的模块/接口目录”。不能仅从 Windows 当前已加载模块列表判断原版是否记住了该 DLL；其他引用也可能使 DLL 在释放这一份引用后继续驻留。

退出时 `004CA828` 才销毁五张表。本次追踪没有发现将这些表序列化成成功 DLL 名单的流程。

## 3. 重建版的实现

### 3.1 已有的容器与对外接口

定义位于 `include/ttplayer/plugins/plugin_manager.h`。

| 成员/接口 | 保存什么 | 判断时的限制 |
| --- | --- | --- |
| `plugins_` / `Plugins()` | 候选 DLL 的路径、入口/实例/登记状态、类别数量、HRESULT 和错误说明 | 同时含成功、失败以及皮肤 ABI 探测记录 |
| `loaded_modules_` | 保留的 Sound AddIn 模块句柄、实例及部分依赖状态 | 不是纯音频解码 DLL 表；模块路径通过其他描述关联 |
| `reader_factories_` | Reader 工厂、模块索引、枚举索引、缓存接口 | 包括没有扩展名限制的通用 Reader |
| `reader_formats_` / `ReaderFormats()` | 对外显示的格式描述、文件模式、DLL 路径 | 通用 Reader 的空模式不加入这里，不适合单独用于完整盘点 |
| `decoder_factories_` / `DecoderFactories()` | Decoder 工厂，以及对外的名称、DLL 路径 | 一个 DLL 可有多个工厂；不能覆盖所有带解码能力的 Reader |
| `encoder_factories_` / `EncoderFactories()` | 编码工厂及名称、扩展名、配置能力、DLL 路径 | 是编码能力，不应算作音频解码成功 |
| `lyric_providers_` / `LyricSearchProviders()` | 歌词搜索接口及来源 DLL | 与音频解码是不同类别 |

`SoundLibraryRuntime` 持有 `PluginManager`，启动时加载运行目录的 AddIn。后台任务可通过 `RetainForBackground()` 保留独立的管理器快照及 DLL 引用；文件信息子进程还会按需建立自己的管理器。因此这些表不是跨进程共享、自动同步的全局清单。

### 3.2 加载成功有多种层次

`PluginManager::Load()` 的主要顺序是：

1. 清空上次状态，枚举候选文件并按文件名排序。
2. `LoadLibraryW` 成功，说明 Windows 能加载 DLL 和当时必需的依赖。
3. 找到 `ttpGetSoundAddIn`，设置 `has_legacy_entry`。
4. 工厂调用成功且实例非空，设置 `instance_created`。
5. 枚举类别，使用 `QueryInterface` 获取相应接口，登记工厂并增加各类计数。
6. 枚举到已知类别时设置 `registered`，保留模块和实例。

需要特别注意：

- `Load()` 返回 `S_OK` 不代表所有 DLL 成功，空目录或部分插件失败也可以返回 `S_OK`。
- `registered` 的条件是“识别到类别”，早于该类别接口的 `QueryInterface` 成功判断。可能出现 `registered == true`，但读取/解码数量为零。
- `RegisteredPluginCount()` 直接返回 `loaded_modules_.size()`，包含编码器和歌词搜索模块，不能显示为“解码器 DLL 数”。
- 皮肤探测也可能设置 `registered`，但在探测后释放模块引用，真正生命周期归皮肤管理器负责。
- 具体文件的打开和解码失败通过会话调用结果返回，不会回写为 `PluginInfo` 中的“最近播放验证状态”。

如果需求是列出“已经成功登记音频读取或解码接口的 DLL”，现有结构可按以下条件筛选：

```cpp
for (const auto& plugin : manager.Plugins()) {
    const bool has_audio_factory =
        plugin.reader_count > 0 || plugin.decoder_creator_count > 0;
    if (!plugin.skin_provider && plugin.has_legacy_entry &&
        plugin.instance_created && plugin.registered && has_audio_factory) {
        // plugin.path：已登记音频接口的 DLL 路径。
        // 这仍不表示每个文件、每种编码参数都已经播放验证成功。
    }
}
```

不能只检查 `decoder_creator_count`：部分 Reader 已直接提供 PCM，不需要另一个独立 DecoderCreator。反过来，Reader 登记成功也不能证明其所有外部解码依赖、格式分支均可用。

### 3.3 保存周期与原版差异

重建版正常登记后直接保留 DLL 与 AddIn 实例，类别接口按需获取并缓存；没有照搬原版“启动登记完成后统一释放模块加载引用、使用时重新创建”的全部生命周期。

`Shutdown()` 按 Reader、Decoder、Encoder、歌词搜索的顺序释放缓存，然后释放模块与实例，清空所有信息向量。后台快照持有的独立引用可能延长实际 DLL 生命周期。

`plugins_` 的写入集中在扫描、快照复制和清空流程。当前未找到将它写入运行配置、日志清单或 JSON 的产品功能，也没有统一展示全部组件状态的界面。

## 4. 重建版专属 DLL 分别存在哪里

| DLL | 当前负责方 | 已存的运行时状态 | 是否进入统一 Sound AddIn 清单 |
| --- | --- | --- | --- |
| `ttp_i18n.dll` | `OptionalI18nRuntime`、`i18n::State` | 启动加载引用；语言组件模块、API、翻译目录实例；`Available()` | 文件名在音频扫描中明确排除 |
| `ttp_https.dll` | 歌词 `Provider`、更新器 `Http::Impl` | 各自的模块/API，更新器另有流式下载 API | 文件名在音频扫描中明确排除 |
| `ttp_waskin.dll` | `SkinPluginModule::Discover`、`PlayerWindow::skin_plugins_` | 通过 ABI 检查的皮肤模块集合，以及当前皮肤实例 | 音频扫描只留下 `skin_provider` 探测记录，不保留为 Sound AddIn 模块 |
| `ttp_maki.dll` | waskin 的 `MakiLibrary` | 模块句柄、VM API，由皮肤一侧管理引用 | 加载后若发现 `ttpGetMakiVM` 就跳过音频登记；没有统一 VM 成功记录 |

补充限制：

- 音频扫描通过导出识别 MAKI，所以若 DLL 本身加载失败，还可能留下普通 `PluginInfo` 加载失败记录；并不是任何情况下都完全忽略这个文件。
- 皮肤模块有专门的成功集合，但 `SkinPluginModule` 未保存一个可直接查询的 DLL 路径字段。它保存的皮肤目录/后缀属于皮肤资源命名空间，不是 DLL 文件位置。
- 语言模块成功加载不等于语言目录成功打开；HTTPS DLL 的 API 有效不等于一次网络请求成功；MAKI 的可用性探测也不等于某套皮肤脚本已成功执行。
- 这些组件可能按需初始化，没有初始化不能统一标成加载失败。
- `ttp_aac.dll` 虽然已有重建源码，仍实现原版 `ttpGetSoundAddIn` ABI，按普通音频插件登记。当前 `PluginInfo` 没有来源、产品版本或哈希字段，不能仅凭文件名判断它是原 DLL 还是重建 DLL。
- `ttpcomm.dll`、`ttpres.dll` 是单独管理的基础运行组件，不属于 `AddIn\ttp_*.dll` 的音频工厂清单。
- `Aac.dll`、`aacenc32.dll`、`NeroIPP.dll`、`tvqdec.dll` 等辅助依赖，也不是各自独立登记的 Sound AddIn。它们可能由插件或专用依赖管理代码加载。

## 5. 容易误认为加载清单的磁盘记录

| 记录 | 实际含义 |
| --- | --- |
| 配置中的 `AddInIndex` | 歌词搜索相关的选择索引；重建版对应 `settings.lyric.add_in_index`，不是 DLL 名单 |
| `PluginState/registry.json` | DSP 插件文件型注册表兼容层的配置数据，不是音频插件加载清单 |
| `https-component.json` | 构建阶段暂存的 HTTPS 组件版本、来源和校验信息 |
| 发行包 `SHA256SUMS.txt` | 分发文件的校验记录，不代表运行时已加载 |
| 磁盘 `AddIn` 目录列表 | 文件存在性，不能证明架构、依赖、入口或初始化正确 |

## 6. 后续若需要统一展示或导出

音频部分可以复用 `PluginInfo` 和工厂信息，不需要重新扫描并额外执行所有 DLL。专属组件需要由各自加载方上报到一个统一的组件状态集合。

建议至少区分：未尝试、文件不存在、加载失败、入口/ABI 不兼容、接口已登记、功能已初始化、当前正在使用。音频格式/文件的实际播放结果应单独记录，不能用一次歌曲失败否定整个插件。

统一记录可包含组件类型、路径、版本、来源证据、失败阶段、HRESULT/Win32 错误码及能力数量。若允许导出磁盘报告，应记录时间、进程和运行目录，下次启动重新核验，不能把旧报告当成可靠加载结果。本文仅提出数据整理建议，没有改变当前加载流程。

## 7. 主要源码定位

原版 `reverse/decompiled/TTPlayer.exe.pseudo.c`：

- 第 189078 行：`CSoundAddInModule_CreateInstance`，入口 `004C88D9`。
- 第 189123 行：`CSoundAddInModule_Invoke`，入口 `004C8954`。
- 第 190907 行：`CSoundLibrary_Initialize`，入口 `004CA48E`。
- 第 191343 行：AddIn 扫描，入口 `004CABC0`。
- 第 191567 行：释放缓存/模块引用，入口 `004CB0F1`。
- 第 193717 行：公共工厂描述初始化，入口 `004CD743`。
- 第 194226 行：模块表追加，入口 `004CDCB8`。

重建版：

- `include/ttplayer/plugins/plugin_manager.h:47`：`PluginInfo`。
- `include/ttplayer/plugins/plugin_manager.h:253`：各类查询接口及计数。
- `src/plugins/plugin_manager.cpp:1053`：候选文件过滤。
- `src/plugins/plugin_manager.cpp:1832`：后台管理器快照。
- `src/plugins/plugin_manager.cpp:1897`：加载、识别、登记。
- `src/plugins/plugin_manager.cpp:2498`：实际创建和初始化 Decoder。
- `src/plugins/plugin_manager.cpp:2717`：关闭和清空。
- `src/i18n/i18n.cpp:191`：语言组件初始化。
- `src/lyrics/https_provider.cpp:10`、`src/update/update_http.cpp:50`：HTTPS 两处状态。
- `src/skin/skin_plugin.cpp:52`、`src/ui/player_window_skin_plugin.cpp:15`：皮肤发现和集合。
- `../waskin/src/maki_client.h:12`：MAKI 模块加载和 ABI 检查。

验证方式：静态追踪原版扫描/登记/释放/重新创建流程，对照重建版容器、写入点、消费者和配置持久化路径。本次没有修改程序或运行测试，不能据此给出当前某个运行进程的成功加载数量。
