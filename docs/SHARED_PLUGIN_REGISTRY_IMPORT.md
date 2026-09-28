# 共享插件注册表与 REG 导入

更新：2026-09-28。

## 当前规则

已接入兼容层的插件共用 EXE 旁的 `PluginState/registry.json`，使用同一个内存实例、文件锁和保存队列。JSON 内不再包含 `plugins.Ozone`、`plugins.Dsp_Dfx` 等分区，也不记录配置来自哪个插件或哪个 REG 文件。

键的身份由完整根键、子路径和值名决定。例如 HKCU 与 HKLM 下的同名路径仍是不同的键；两个插件访问同一个完整路径时读到同一份配置。

| 操作 | 行为 |
| --- | --- |
| 打开、读取 | 先检查 JSON；缺少键或值时只读访问系统注册表 |
| 创建、写入 | 只修改 JSON，不要求系统注册表写权限 |
| 枚举、查询数量 | 合并 JSON 和系统数据，同名值由 JSON 覆盖 |
| 删除值、删除空子键 | 在 JSON 中保存删除记录，屏蔽系统中的同名数据；不删除系统数据 |
| 再次创建已删除的键 | 建立新的文件配置，仍屏蔽该键原来的系统数据 |
| 导入 REG | 按完整路径合并，不按插件分类；同名值以最后导入的内容为准 |

值名比较不区分大小写，但保留写入时的拼写供枚举使用。Enhancer 的旧代码会枚举参数名，因此不能把返回的名称全部改成小写。

文件配置采用共享逻辑路径：`HKLM\Software\WOW6432Node\...` 归一到 `HKLM\Software\...`，适配本播放器的 32 位插件。系统读取默认使用 32 位视图；插件显式请求 64 位视图时，缺失数据从该系统视图读取，JSON 仍保持同一份逻辑配置。DFX 读取 Windows 版本信息会用到这个分支。

## 导入方法

1. 完全退出播放器。
2. 将一个 `.reg` 文件拖到 `TTPlayerRebuild.exe` 的文件图标上，或运行 `TTPlayerRebuild.exe "D:\配置\设置.reg"`。
3. 导入程序显示结果并退出；下次加载插件时生效。

一个 REG 可以同时包含多个插件或其他完整注册表路径。程序不会调用 regedit，不会修改系统注册表。正在运行的播放器、其他导入进程或不可写的文件会阻止导入。

支持 `Windows Registry Editor Version 5.00`、`REGEDIT4`；UTF-16LE BOM、UTF-8 BOM 和系统 ANSI 编码；字符串、DWORD、`hex:`、`hex(type):` 及 hex 续行。二进制、扩展字符串、多字符串和 QWORD 可用相应 hex 类型表示。支持 HKCU、HKLM、HKCR、HKU、HKCC 的完整根键名称、默认值 `@`、空键和重复赋值。

暂不支持 REG 删除脚本（`[-路径]`、`"值名"=-`）。运行中的插件通过删除 API 产生 JSON 删除记录属于另一个入口。

导入会先解析完整文件，再合并并原子保存；解析、容量或保存失败不会发布部分导入数据。输入文件和 JSON 均限制为 4 MiB。

## 格式与迁移

当前格式示例（仅含演示数据）：

```json
{
  "formatVersion": 3,
  "registryView": 32,
  "keys": [{
    "path": "hkey_current_user\\software\\example",
    "values": [{"name": "Enabled", "type": 4, "dataHex": "01000000"}]
  }],
  "maskedKeys": []
}
```

`type` 使用 Windows REG_* 类型编号；字符串以 UTF-16LE 字节保存。删除值表示为 `{"name":"旧值","deleted":true}`；`maskedKeys` 保存屏蔽系统回退的子树路径。

- v1 Ozone 无根键路径自动补上 HKCU。
- v2 Ozone、DFX 分区合并，包括两者共同的祖先键。
- 原 `PluginState/Ozone/registry.json` 在共享配置没有 Ozone 数据时导入，源文件保持原样。
- 旧 `registry-import.reg` 可在共享配置为空时导入，不再限制为 Ozone。
- 成功替换前保留 `registry.json.bak`；后续保存会更新这个备份。

## Enhancer 修复

原版没有有效的 UAC 执行级别声明，系统可以把 Enhancer 的 HKLM 写入虚拟化到当前用户的 VirtualStore。重建版的 `asInvoker` 清单关闭了这项隐式虚拟化；插件使用 `KEY_ALL_ACCESS` 创建 HKLM 键，失败后弹出“无法访问注册信息”。

Enhancer 现使用通用导入表／已解析入口识别，覆盖创建、查询、设置、枚举等业务调用。不再依赖文件哈希或固定导入槽 RVA，原始压缩版和恢复后的 DLL 使用相同共享存储；不改写磁盘 DLL。

`Skin File` 是子键的默认字符串，不是父键的同名值。已按这一语义实现 ANSI/Unicode 基本查询、写入 API，包含缓冲区大小和结束符处理。卸载后释放插件遗留的文件注册表句柄。

共享文件尚无 Enhancer 配置时，只读合并机器配置和当前用户的旧 VirtualStore 配置，用户虚拟化值优先。64 位系统显式读取 VirtualStore 的 64 位视图及 `WOW6432Node` 路径；32 位系统使用对应原生路径。JSON 已有配置保持优先。

详细取证见 [Enhancer 初始化分析](ENHANCER_REGISTRY_ACCESS_ANALYSIS.md)。

## 适配范围与测试

已改为通用 DSP 接入，不再按文件名或哈希限制为 Ozone、DFX、Enhancer。原始及恢复 DLL、合法的 `OriginalFirstThunk == 0`、动态接口解析和同目录私有依赖使用同一适配流程。哈希仅选取已验证的 Ozone／DFX 特定修复。未支持的 Reg 接口拒绝接入并记录内部诊断；接入、保存失败不在前端显示，音效插件列表没有“注册表配置”列。完整范围和限制见 [通用接入说明](DSP_GENERIC_REGISTRY_INTEGRATION.md)。

DFX 的 `PluginState/Dsp_Dfx` 目录仍用于普通配置文件和缓存，不是注册表数据分区。它的安装位置键仍在加载时按当前插件及播放器目录更新。

测试仅位于 `rebuild/tests`，不加入发行包和 Actions。覆盖共享读写、值名枚举、迁移备份、混合 REG、保存失败、系统读取回退、JSON 优先级、删除及重启、真实插件初始化及退出。通过隔离分支核对系统配置未被修改。

2026-09-28 验证结果：

| 项目 | 本机 | XP 虚拟机 | Win7 虚拟机 |
| --- | --- | --- | --- |
| JSON 覆盖优先、系统只读回退、删除屏蔽及枚举 | 通过 | 通过 | 通过 |
| Enhancer 真实 Init/Quit、两次加载、Volume 参数恢复、Skin File 默认值读写 | 通过 | 通过 | 通过 |
| Enhancer 机器配置与 VirtualStore 用户配置的迁移优先级 | 通过 | 通过 | 通过 |
| DFX + Ozone 同时启用及 PCM 处理 | 通过 | 通过 | 通过 |
| DFX 设置助手显示/隐藏、异常退出后重启和卸载 | 通过 | 通过 | 通过 |
| REG 拖到 EXE 的导入入口、旧 JSON、混合格式及保存失败 | 通过 | 未单独重跑入口 | 未单独重跑入口 |

上述插件测试检查隔离的系统配置分支未被插件写入。它们验证本次注册表适配及生命周期，不代表逐项验证插件的全部音效参数或算法与原版完全一致。Release 播放器及更新程序还通过 XP/Win7 静态导入检查。
