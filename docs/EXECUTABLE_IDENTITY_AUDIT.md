# TTPlayerRebuild 更名影响审计

日期：2026-09-28。对象：当前源码、`build/Release/TTPlayerRebuild.exe`（文件版本 `2026.09.28`）、原版 `TTPlayer.exe`、原版伪代码及本地 AddIn / DSP 二进制。

## 1. 结论

**更名不是当前程序普遍无法启动、解码失败或崩溃的已知原因，但更名确实会影响部分配置文件、迁移流程及外部入口。不能笼统认定“完全没有影响”。**

需要区分三个问题：

1. **发行名称从旧名称改为 `TTPlayerRebuild.exe`**：主体资源定位和工作进程已适配；歌词关联、OctiMax 配置及旧测试脚本存在具体影响。
2. **用户再把发行 EXE 任意改名**：基本启动仍可工作，但内置更新并不支持任意名称，歌词关联文件和任务栏身份也会随之改变。
3. **原版、旧构建、新构建同时存在**：单实例、共享数据、快捷方式指向和版本混用会产生类似“修复没有生效”的现象。这些不能全部归因于 EXE 字符串本身。

本次完成分析、隔离验证和记录，未修改生产实现、插件原文件或发行包。

## 2. 发现汇总

| 项目 | 证据与判断 | 用户可见影响 | 优先级 |
| --- | --- | --- | --- |
| OctiMax 配置名含宿主 EXE 名 | 恢复 DLL 的初始化调用链、伪代码和字符串确认；本次未完成实际持久化对照 | `octimax_ttplayer.ini` 变为 `octimax_ttplayerrebuild.ini`，原配置可能不被读取 | 高：插件配置兼容 |
| 歌词关联 `.rll` 随 EXE 名改变 | 当前实现及已有文档明确确认；现有设计不自动迁移原版文件 | 手动关联、不搜索歌词等记录不会随 XML 迁移过来；原记录仍在旧文件中 | 高：迁移体验 |
| 不同目录的重建版共用单实例标识 | **本机实测复现** | 启动 B 目录实际唤起 A 目录，B 的配置不会加载 | 高：多副本/版本验证 |
| 原版和重建版可并行、同目录数据未完全隔离 | 原版初始化汇编及重建源码确认；未制造实际数据覆盖 | 同时运行可能竞争播放列表、媒体库和皮肤配置 | 高：同目录并用 |
| 更新器固定发行文件名 | 源码确认，标准发行命名一致 | 手动改名后可能无法读取版本、替换、等待正确进程或重启 | 中：仅手动改名触发 |
| 部分测试工具仍使用 `ttplayer_rebuild.exe` | 脚本默认参数/输入路径确认；当前 Release 无此文件 | 工具找不到 EXE；有旧残留时可能测到旧程序 | 中：开发验证 |
| 旧快捷方式、关联、任务栏身份 | 源码确认按真实路径注册，未自动接管原版入口 | 双击歌曲或固定图标仍可能启动原版/旧构建 | 中：入口迁移 |
| 主程序和 AddIn 基础加载 | 隔离启动、子进程测试通过；63 个 PE 静态扫描 | 未发现必须叫 `TTPlayer.exe` 的普遍加载要求 | 未发现更名故障 |

“高”表示建议优先处理兼容策略，并非本次已复现崩溃或数据损坏。

## 3. EXE 名称直接影响的配置

### 3.1 歌词关联与主 XML 使用不同命名规则

[`EnsureLyricAssociationsLoaded`](../src/ui/player_window_lyrics.cpp) 获取当前 EXE 完整路径，然后执行 `replace_extension(L".rll")`。对应原版：

- `00401829`：取得模块路径并替换扩展名。
- `004C038B` 的启动流程及 `004BE9C8`：读取歌词关联。
- 相关恢复说明：[歌词关联实现](LYRIC_ASSOCIATION_RECOVERY.md)。该文档已明确“不修改/自动迁移原版 TTPlayer.rll”。

| 运行文件 | 主设置文件 | 歌词关联文件 |
| --- | --- | --- |
| 原版 `TTPlayer.exe` | `TTPlayer.xml` | `TTPlayer.rll` |
| 重建版 `TTPlayerRebuild.exe` | `TTPlayerRebuild.xml` | `TTPlayerRebuild.rll` |
| 将重建版手动改名为 `TTPlayer.exe` | 仍为 `TTPlayerRebuild.xml` | 改为 `TTPlayer.rll` |
| 将重建版手动改为其他名称 | 仍为 `TTPlayerRebuild.xml` | `<新名称>.rll` |

因此，旧版的手动歌词关联未带入，是可以解释的名称影响，并非原记录被删除。直接把 EXE 改回旧名称，会形成“新版 XML + 原版 RLL”的混合配置，也会让后续保存涉及原版关联文件。

主 XML 已有单独迁移逻辑：[`runtime_settings.cpp`](../src/settings/runtime_settings.cpp) 在新文件不存在时复制同目录 `TTPlayer.xml`，保留未知字段，之后保存只指向新文件；已经存在的新 XML 不会被旧文件覆盖。`.rll` 没有对应的迁移流程。这是现有隔离策略，需要作为产品兼容选择明确说明，不宜直接称为遗漏后静默合并。

### 3.2 OctiMax 通过宿主名称构造 INI

恢复后的 `Plugins-out/dsp_OctiMax.dll` 中：

- `FUN_1000B700` 及其跳板调用 `GetModuleFileNameA(NULL, ...)`。
- 路径处理后寻找 `.exe`（字符串地址 `10073850`），替换为 `.ini`（`10073848`）。
- 从路径提取宿主基本名称。
- 使用 `octimax_%s.ini`（`1007381C`）在插件所在目录生成自己的配置路径。
- 初始化流程调用该函数，把路径写入 `DAT_1007F8EC`；后续 `GetPrivateProfileIntA` / `WritePrivateProfileStringA` 使用该路径，读写 `WinampPlugin` 中的窗口位置、停靠、显示状态等。

由此得到：

```text
宿主 TTPlayer.exe          → Plugins/octimax_ttplayer.ini
宿主 TTPlayerRebuild.exe   → Plugins/octimax_ttplayerrebuild.ini
宿主 ttplayer_rebuild.exe  → Plugins/octimax_ttplayer_rebuild.ini
```

这是真实的“宿主名称决定插件配置位置”逻辑。即使 Init 和 PCM 处理都通过，也不代表配置继承一致。此前使用 `winamp_dsp_runtime_tests.exe` 的测试不能覆盖上述命名迁移。

当前通用注册表适配器处理 `Reg*` 访问，**不会把 `GetPrivateProfile*` / `WritePrivateProfile*` 的 INI 读写重定向到 `registry.json`**。因此共用 JSON 不会自动解决这一问题。

范围限制：本次由恢复 DLL 的可达初始化代码确认路径规则；Win7 补充运行请求未返回，未取得两种宿主名称下的配置保存对照，不能据此宣称已实测全部预设/音效参数的迁移。也没有修改许可逻辑。

### 3.3 其他 DSP 不应一律套用 OctiMax 规则

已核对的典型路径：

- `dsp_neq` 的 `004099EC` 从插件模块路径取目录，再拼 `plugin.ini`。
- `dsp_compwide` 的 `1000196C` 查找 **自身** `dsp_compwide.dll`，验证自身文件名并替换为 `plugin.ini`。这是插件 DLL 名约束，不是播放器 EXE 名约束。
- `dsp_sps` 的 `10003740` 从 `module->hDllInstance` 取得插件目录，拼 `dsp_sps` 子目录。
- DFX 当前兼容代码对辅助窗口的筛选包含进程 ID，不是按 `TTPlayer.exe` 查找播放器。

因此不建议全局伪造 `GetModuleFileName(NULL)`，也不应批量把插件中的名称替换成 `TTPlayerRebuild`。应针对已确认的配置命名规则处理迁移。

## 4. 单实例与同目录共存

### 4.1 原版使用 GUID，不是 `TTPlayer_Event`

原版伪代码的 `CSingleInstanceIpc_Initialize`（`004B5519`）本身接收名称参数，不能仅凭函数中的 `_Event` / `_Mapping` 后缀确定实际名称。

二进制静态初始化代码 `00515519` 实际传入：

```text
名称：{2A85B11F-C71F-4fbd-9B99-EBC6613D63C4}
共享区大小：0x10（16 字节）
```

`004B5519` 的汇编进一步确认：事件使用 `<GUID>_Event`；虽然构造过 `_Mapping` 字符串，实际 `CreateFileMappingW` 传入的是原始 GUID 参数。不能只根据变量名称把映射名写成 `<GUID>_Mapping`。

重建版则是：

```text
事件：TTPlayer_Event
映射：TTPlayer_Mapping
请求映射大小：4096 字节
```

证据：[`main.cpp`](../src/app/main.cpp)、[`single_instance.cpp`](../src/app/single_instance.cpp)。

**原版和重建版不会因为这两套名称互相阻止启动。** 若将来要恢复原版 IPC，必须同时核对映射名称、结构、大小和转发行为，不能仅换一个字符串。

### 4.2 两份重建版会互相转发

重建版的标识不包含安装目录、EXE 完整路径或版本。实际测试：

1. 在隔离目录 A 启动当前发行 EXE，正常创建主窗口。
2. 从另一隔离目录 B 启动相同 EXE。
3. B 退出码为 `1`；A 继续运行；B 没有生成 `TTPlayerRebuild.xml`。

这与 `main.cpp` 的次实例分支一致：转发后直接退出，不创建自己的应用会话。原版也采用全局单实例，因此“只允许运行一个实例”本身可能是有意行为；但当前并排部署和多个构建目录下，它会让用户误以为打开了另一个版本。

本机还确认存在两份不同版本：

| 位置 | 文件版本 | 大小 |
| --- | --- | ---: |
| 项目根目录 `TTPlayerRebuild.exe` | `2026.09.23` | 2,774,016 字节 |
| `rebuild/build/Release/TTPlayerRebuild.exe` | `2026.09.28` | 3,505,152 字节 |

启动入口混用，加上全局单实例，是“更新后仍像旧版”的具体排查方向。该清单仅为本次本地审计快照。

### 4.3 XML 分开，不代表所有数据分开

当前 Action 发布说明让用户把新 EXE 放到原版同目录。两者仍会访问同目录的：

- `PlayList/`：[`LoadStoredPlaylist`](../src/ui/player_window_playlist.cpp)。
- `Music.library`：[`player_window_library.cpp`](../src/ui/player_window_library.cpp)。
- `Skin/Default.xml`、`Skin/<包名>.skn.xml`：[`ResolveSkinProfilePath`](../include/ttplayer/ui/player_runtime_policy.h)。
- 插件自身的 `plugin.ini`、预设等文件。

新旧版可并行且配置保存时机不同，存在最后保存者覆盖先前修改、皮肤布局相互影响等风险。本次没有用真实用户数据复现覆盖，也没有认定已经发生损坏。单独修改 EXE 名称无法解决数据共享策略。

主窗口类均为 `TTPlayer_PlayerWnd`。只按类名查窗口的旧外部工具可能找错实例。原版 `Updater.exe` 伪代码中确有在检测原版事件存在后，直接按该类名查找窗口并发送 `WM_CLOSE` 的路径。重建版 `TTPUpdater.exe` 则额外检查进程完整路径，不能把旧更新器风险套用到新更新器。

## 5. 更新、关联和任务栏

### 5.1 标准发行名下更新规则一致，手动改名不受支持

[`update_package.cpp`](../src/update/update_package.cpp) 与 [`updater/main.cpp`](../src/updater/main.cpp) 固定使用 `TTPlayerRebuild.exe`：

- ZIP 条目白名单、版本资源 `OriginalFilename` 校验。
- 当前版本读取、运行进程路径匹配。
- 替换目标、`.bak` 回退文件、更新后启动路径。

Action、CMake 输出和打包脚本目前使用同一名称，未发现标准发行包在这里存在新旧名混用。

如果只把 EXE 手动改回 `TTPlayer.exe`，启动可能成功，但更新器仍寻找 `TTPlayerRebuild.exe`；若旁边保留另一份同名旧文件，还可能检查/更新另一份文件。这不是“改回旧名即可解决兼容”的场景。另一个目录已有重建版运行时，更新后的新进程还会受到上一节全局单实例转发影响。

本次未下载或安装更新，以上更新影响为源码路径确认。

### 5.2 文件关联已使用真实 EXE 路径

选项页以 `GetModuleFileNameW(NULL, ...)` 得到当前路径，传给 [`FileAssociationBackend`](../src/settings/file_association.cpp)。打开命令、图标和所有者记录都基于该路径。

注册标识采用独立的 `TTPlayerRebuild.Audio`、`Software/TTPlayerRebuild/Capabilities` 和 `RegisteredApplications/TTPlayerRebuild`。这是有意避免冒充原版的产品身份，不是错误引用。

旧快捷方式、原版的系统关联不会因新文件出现在同目录就自动换目标；关联建立后再移动/改名 EXE，也会留下旧路径。应通过明确的迁移或重新关联解决，不能期待文件系统改名自动更新所有外部入口。

### 5.3 任务栏和 SMTC

[`taskbar_icon.cpp`](../src/ui/taskbar_icon.cpp) 的 AppUserModelID 为产品前缀加规范化完整 EXE 路径的哈希；重启命令使用真实路径，固定快捷方式只匹配同一路径的目标。移动或改名后身份会变化，旧原版固定项不会自动变成重建版入口。

[`system_media_controls.cpp`](../src/ui/system_media_controls.cpp) 通过当前 HWND 的 `GetForWindow` 创建 SMTC，没有按 `TTPlayer.exe` 获取宿主的逻辑。未发现本程序 SMTC 初始化依赖旧 EXE 名。第三方壁纸若额外按应用 ID/进程名筛选，需要单独核对其筛选条件；本次没有对所有壁纸作验证。

## 6. 为什么基本加载不依赖旧名称

- [`RuntimePath`](../src/app/runtime.cpp) 取实际 EXE 所在目录，再定位 `ttpcomm.dll`、`ttpres.dll`、`AddIn`、`Skin` 等；不根据旧名称寻找主程序。
- DSP、文件信息、输出设备工作进程使用实际宿主路径；DSP 扫描的递归子进程同样如此，见 [`worker_process.h`](../include/ttplayer/app/worker_process.h)。
- 扫描 63 个 PE（含 20 个 AddIn 文件、19 个原始 Plugins 文件、12 个恢复插件，以及根目录/发行 EXE 等），未发现以 `TTPlayer.exe` 或其他 EXE 为目标的普通/延迟导入描述符。
- `ttp_clienc.dll` 的 `60206176..602061A9` 先找 `ttpctrl.dll`，失败后调用 `GetModuleHandleA(NULL)`，再解析 `CreateStdContent`、`CreateStreamOnFile`。这里解析的是导出函数，不是要求宿主叫 `TTPlayer.exe`。
- 重建 EXE 保留了上述两个未修饰名称及序号 `2`、`3`，见 [`exports.cpp`](../src/app/exports.cpp)。重建 AAC 也是先找 `soundcore.dll`，否则取当前进程模块。
- `ttpres.dll` 中找到的 `TTPlayer.exe` 位于版本资源的 `OriginalFilename`，不是加载主程序的命令。
- OctiMax 中的 `winamp.exe` 搜索命中实际是 `octivu_winamp.exe` 的子串，不能单凭该命中断定插件要求播放器名为 Winamp。

边界：无静态 EXE 导入不等于所有动态路径都与文件名无关，OctiMax 就是反例；压缩/自解压代码也不能只靠字符串扫描排除。原版 EXE 还有 `CreateSoundBuffer`、`CreateStreamOnInet`、`GetSoundCodecName` 等导出，当前重建 EXE 导出表并未全部复刻。部分 AddIn 中存在相关函数名字串，这类 API 完整性要另行跟踪调用及回退分支；不能把可能的缺失导出问题归因于更名，改回文件名也不会补出函数。

## 7. 测试工具的旧名称残留

以下是确定读取当前构建路径的代表，不是仅用于历史记录的文本：

- [`probe_about_date.ps1`](../tools/probe_about_date.ps1)：默认从 `build/Release/ttplayer_rebuild.exe` 复制。
- [`run_release_playback_sandbox.ps1`](../tools/windows_sandbox/run_release_playback_sandbox.ps1)：将上述旧路径作为必须存在的输入。
- [`probe_skin_switch_aux.ps1`](../tools/probe_skin_switch_aux.ps1)：只在 `ExecutableName == ttplayer_rebuild.exe` 时进入重建版的特定分支；只改调用参数为新名仍可能选错分支。
- 多个旧版媒体库、任务栏、全屏、窗口状态脚本也保留该名称。

当前 Release 中没有 `ttplayer_rebuild.exe`。大小写不敏感不能解决中间多出的下划线。因此这些默认入口会找不到文件；若遗留旧文件，可能误测旧版本。

部分沙箱脚本有意把输入复制为固定的测试别名，不能全局盲替换。后续应把“构建源文件路径”“隔离副本名称”“原版/重建版测试分支”分开，通过显式参数判断版本类型。遵守现有要求：测试代码只保留本地 `tests`，不上传、不进入 Action 测试流程。

## 8. 已完成验证与范围

本机 Windows 11，系统构建号 `26200`，同一发行二进制 SHA-256：

```text
15eca39b385d9f017a230780fbba8715285c2621f0c2c81e7c1494d0ad8a64ad
```

在 `tests/artifacts/executable_identity_audit/` 下创建独立副本，使用合成 XML 和测试 DSP，不使用用户歌曲、插件配置或播放列表。工作目录与 EXE 目录不同。

| 隔离 EXE 名 | 启动并退出 | 新 XML 生成且旧 XML 字节未变 | DSP 扫描及递归子进程 |
| --- | --- | --- | --- |
| `TTPlayerRebuild.exe` | 通过，退出码 0 | 通过 | 通过，返回 1 个测试 DSP |
| `TTPlayer.exe` | 通过，退出码 0 | 通过 | 通过，返回 1 个测试 DSP |
| `播放器 审计.exe` | 通过，退出码 0 | 通过 | 通过，返回 1 个测试 DSP |

另实测不同目录的次实例转发：A 正常运行，B 返回 1，B 未创建设置。测试结束正常退出自己的 A 进程。

本地证据：

- `tests/executable_identity_audit.py`、`binary_identity_inventory.json`：PE 清单、导入/导出、名字串位置、哈希。
- `tests/executable_identity_runtime.py`、`runtime_identity_results.json`：发行 EXE 改名副本及跨目录实测。
- `binary_identity_evidence.txt`：原版 IPC 构造参数、映射调用及 CLI 编码器宿主解析的汇编证据。

Win7 的补充准备调用长时间未返回，已中止工具等待；没有取得本次 OctiMax 改名运行结果，也没有进行本次 XP/Win7 全量回归。此前插件可加载、可处理 PCM 的测试不作为本次配置迁移已通过的替代证据。没有进行完整音频格式、更新安装、真实关联修改或所有外部集成测试。

## 9. 建议后续顺序

1. **明确并补齐状态迁移**：为 `.rll` 提供保留旧文件的导入方案；为 OctiMax 验证旧/新宿主名称下的 INI 后，设计只在目标缺失时的一次性迁移，已有目标不覆盖。不要把全目录 INI 混合迁移，也不要把许可文件纳入普通设置迁移。
2. **统一实例与数据隔离策略**：建议重建版实例标识按规范化安装目录区分，使不同构建目录可以独立测试；同一数据目录的新旧版并行则需要防止共享数据竞争。仅放开单实例而不处理共享数据不完整。
3. **维持正式 EXE 名称稳定**：更新器可以增加早期身份检查和明确处理，或完整实现动态目标路径支持；不建议先手动改回旧名来规避插件配置问题。
4. **清理旧验证入口**：修正真正的构建输入路径，用显式版本参数代替从文件名推断版本；测试仍只放本地 `tests`。
5. **验证用户外部入口**：检查快捷方式、文件关联、固定任务栏项实际指向的版本，避免仍打开项目根目录旧 EXE。按当前发布要求以 `build/Release` 为发行来源。

总体建议：保留 `TTPlayerRebuild.exe` 的正式名称，修复已识别的迁移和身份边界，比简单改回 `TTPlayer.exe` 更可控。
