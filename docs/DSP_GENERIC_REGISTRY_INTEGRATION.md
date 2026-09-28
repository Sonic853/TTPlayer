# DSP 通用共享注册表接入

更新：2026-09-28。

## 完成的修改

1. **取消 DLL 文件名和哈希白名单。** 任意经播放器加载的 x86 DSP 都进入相同适配流程。原始压缩 DLL、恢复后的常规 DLL、改名 DLL 使用同一个共享注册表。
2. **兼容 `OriginalFirstThunk == 0`。** 优先读取运行时导入表，再利用磁盘名称表或 FirstThunk 元数据，对照实际函数地址确认目标，不把加载后的函数地址当作名称 RVA。
3. **处理被压缩程序覆盖的导入名称表。** 检查可执行节内 x86 间接调用、跳转和绝对地址加载引用的槽位，只接受与系统导出入口精确匹配的地址。Enhancer 不再使用固定的五个 RVA。
4. **接入动态查询和同目录私有依赖。** 覆盖 GetProcAddress 按名称／序号查询、LoadLibraryA/W/ExA/ExW，以及已加载的同目录依赖。只修改插件及其私有依赖的导入槽。
5. **失败时回滚。** 发现已识别但未实现的 Reg 接口时拒绝模块接入并记录 API 名称，撤销已改槽位和依赖引用。重复失败不会留下“已适配”的空记录。动态解析失败返回空函数地址，不静默回退为系统写入。
6. **仅保留内部诊断。** 按用户要求，“选项 → 音效插件”保持原来的列表列和列宽，不显示“注册表配置”列，接入失败、保存失败不在前端显示。已移除该列的刷新计时器、状态缓存和前端查询接口；内部诊断继续记录接入、保存、Init、Config、PCM、Quit 阶段的问题。
7. **修复只读恢复后的保存。** 旧代码可能把主文件的只读属性复制到 `.bak`，导致解除主文件只读后仍不能轮换备份。现在在创建临时文件和轮换备份前检查只读状态。
8. **固定绝对路径。** 共享 JSON、DFX 目录和兼容迁移目录使用绝对路径，避免插件改变进程工作目录后影响保存。

## 保持的行为

- 同一播放器配置下所有 DSP 共用 `PluginState/registry.json`、内存树、文件锁和保存队列。
- JSON 缺少路径／值时只读回退到系统注册表；写入、删除、REG 导入只修改 JSON。删除标记继续遮蔽相应系统回退项。
- REG 导入不按插件分类；旧 v1/v2 文件和 Ozone 目录继续兼容迁移。
- 哈希仅选取已验证的 **Ozone 退出修复**、**DFX 设置助手和目录修复**，不决定共享注册表资格。
- 原版 `FUN_00427edc` 调用 Init 后没有根据整数返回值判定失败。保留该行为，以及现有 DSP 调用线程、PCM 处理约定。

## 范围与限制

当前封装 RegOpenKey、RegCreateKey、RegQueryValue、RegSetValue 及其 Ex 版本的 A/W 变体，以及 RegCloseKey、RegFlushKey、RegEnumKeyEx、RegEnumValue、RegDeleteKey、RegDeleteValue、RegQueryInfoKey。

这是一层插件配置兼容机制，不能等同于完整注册表虚拟机或安全沙箱：

- 接管安装在 LoadLibrary 返回之后，不能追溯初次 DllMain／静态构造函数已经执行的访问。
- 自行使用 Nt*、预先缓存入口、独立子进程、其它目录依赖，可能绕过本层。DFX 已审计的设置助手继续通过现有 IPC 接入，没有扩展为任意子进程注入。
- 未支持的接口明确报错，不能保证所有未来第三方 DLL 的全部 Windows API 都已覆盖。
- INI、CFG、预设等普通文件访问没有重定向进 JSON。此前发现的 `plugin.ini` 共享和预设覆盖属于另一类文件行为。

## 验证

测试源码、夹具和日志仅保留在 `rebuild/tests`。生产 CMake／Actions 未新增测试入口，发行包沿用明确文件白名单。

通用夹具验证：改名 DLL、两个 DLL 共享数据、无原始导入名称表、系统只读回退、写入隔离、动态名称／序号解析、私有依赖、卸载恢复、重启读取、不支持 API 的连续拒绝及回滚、只读错误与保存重试。

真实样本为 `Plugins` 的 17 个原始 DSP 和 `Plugins-out` 的 12 个恢复 DLL，执行接入、Init、44.1/48 kHz PCM、Quit、保存和卸载。所有可写测试都使用独立副本。Ozone 使用用户已有 REG 的测试副本；ViPER 的首次默认配置提示只由测试工具记录并确认。

最终结果：

| 检查 | 本机 | XP 5.1.2600 | Win7 6.1.7601 |
| --- | --- | --- | --- |
| 注册表模型／并发持久化、覆盖层、DSP 生命周期、通用接管夹具 | 4 项通过 | 4 项通过 | 4 项通过 |
| 17 个原始＋12 个恢复 DLL：接入／Init／PCM／Quit／保存 | 第一轮及针对性复测通过；DFX 完整配套另在 VM 验证 | 29 项通过 | 29 项通过 |
| Enhancer 原始／恢复版：配置恢复及 Skin File 默认值读写 | 由 VM 覆盖本次增量 | 2 项通过 | 2 项通过 |
| DFX：设置窗口应用、助手异常退出／重启、Ozone 共存 | 由 VM 覆盖本次增量 | 1 项通过 | 1 项通过 |
| Ozone：进程重启、配置迁移到新目录 | 由 VM 覆盖本次增量 | 1 项通过 | 1 项通过 |
| VM 合计 | — | **37/37** | **37/37** |

两台 VM 共 **74 项通过**。独立目录为 `C:\DSP-Generic-Verified-20260928`；结果日志为 `tests/artifacts/dsp_generic/vm/XP-verified.log` 和 `Win7-verified.log`。每个真实插件处理两种采样率、各 8 块 PCM，并检查缓冲区边界。这是移除前端状态列之前的后端回归记录，不代表全部预设、所有配置窗口、长时间听感或 DSP 算法等价证明，也不代表三系统逐像素 UI 比较。

中间失败日志保留且不计入成功结果。测试修正包括：补齐 Ozone 已有登记数据、记录确认 ViPER 首次提示、ZIP 夹具采用存储模式和 ASCII 测试文件名、异常依赖夹具导出真实入口、DFX 配套数据及系统只读元数据。实现修正包括失败接入回滚、只读备份重试、动态依赖错误可见性、绝对配置路径。

Release 和更新器均通过 XP／Win7 静态导入审计；主程序为 x86、子系统版本 5.01。测试源码、DLL、日志及上述登记测试数据不包含在发布包中。

移除前端状态列后，重新完成 Release 构建、XP／Win7 静态导入审计及发行包内容与校验和验证。本次界面调整没有重跑上述虚拟机测试，后端通用接管与保存机制保持不变。

该次更新的 EXE 位于 `build/Release/TTPlayerRebuild.exe`。用户关闭正在打开的程序和压缩包后，发行包已恢复至标准路径 `build/Release/TTPlayerRebuild-2026.09.28.zip`，校验和已同步更新。临时 `updated` 目录和本次打包残留已清理，发行包保留移除前端状态列的修改。

## 独立 DSP 进程的评估

本次保留进程内执行，维持原版窗口、定时器和 Winamp 消息交互。SEH 不能隔离 CRT abort、ExitProcess 或永久阻塞，本次没有改变这一限制。

可选进程隔离需要单独验证共享内存 PCM、采样率／清空协议、配置窗口与播放器消息代理、故障后旁路和状态恢复，并测量 WaveOut／DirectSound／WASAPI 下的延迟。当前没有这些延迟数据，因此不默认切换架构，也不以强杀线程代替隔离。

## 参考

- [原版伪代码](../../reverse/decompiled/TTPlayer.exe.pseudo.c)：`FUN_00427edc` 的初始化及模块保存。
- [Microsoft PE 格式](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format)：名称／序号导入及加载后的地址表。
- [GetProcAddress](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getprocaddress)：按名称和序号查询入口。
- [共享 REG 导入与迁移](SHARED_PLUGIN_REGISTRY_IMPORT.md)。
