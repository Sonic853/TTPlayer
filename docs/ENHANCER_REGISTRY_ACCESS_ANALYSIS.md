> 2026-09-28 实施完成：按本文取证接入 Enhancer。遵照用户后续要求，采用所有已适配插件共享的 registry.json，未创建独立 Enhancer 分区；读取缺失时回退系统，写入只落 JSON。当前实现及验证见 [共享注册表说明](SHARED_PLUGIN_REGISTRY_IMPORT.md)。第 9 节为修复前建议，数据分区方案已被本次要求替代。

# Enhancer 0.17 启用时“无法访问注册信息”分析

分析日期：2026-09-28。

## 1. 结论与证据边界

**直接原因已确认：`dsp_enh.dll` 在 Init 中对机器级注册表分支请求 `KEY_ALL_ACCESS`，调用返回 `ERROR_ACCESS_DENIED (5)`，插件随即显示“无法访问注册信息...”。**

目标分支：

```text
HKEY_LOCAL_MACHINE\SOFTWARE\Ioscasoft\Enhancer\Version 017
```

这里的“注册信息”是插件配置的注册表存储，不是注册码校验。这条失败路径涉及音效参数、窗口状态和皮肤路径。

**根据用户给出的实际操作路径补充验证后，已确认原版可以正常启用。**

操作路径：`D:\Projects\Backup\TTPlayer\TTPlayer.exe` → 主窗口右键 → 千千选项 → 音效插件 → 勾选 Enhancer v0.17。

用户完成勾选并保持窗口打开后，只读检查该进程，确认：

- 实际 EXE 和 DLL 均来自指定的 D 盘目录；同一份插件已完成 Init，具有有效配置句柄、插件窗口和消息钩子。
- 原版进程未提升权限，`TokenVirtualizationEnabled=1`。
- 当前用户的 VirtualStore 中存在 Enhancer 配置：14 个参数值及 1 个皮肤子键。未虚拟化的 HKLM 32 位视图中不存在该配置分支。
- 因此，原版这条成功路径依靠 Windows 的旧程序注册表虚拟化，避免直接写受保护的机器级注册表；重建版具有生效的 `asInvoker` 声明，且当前没有 Enhancer 专用注册表适配，缺少这项兼容行为。

**更正先前结论的范围：**先前由诊断工具启动原版，并在 XML 中预置启用插件，观察到了返回 5；那个结果仅覆盖对应的诊断启动路径，不能据此否定用户正常操作路径的成功。本次已获得成功进程的直接证据。不能再写成“原版在本机普通权限下也一定失败”。

原版 XML 中遗留的 C 盘插件目录，也不能用来判断本次加载文件；本次已通过实际进程模块路径核实。

本次完成分析、诊断和文档，没有修改生产程序或插件二进制。

## 2. 分析对象

| 项目 | 已核实值 |
| --- | --- |
| 文件 | `Plugins/dsp_enh.dll` |
| 大小 | 45,568 字节 |
| SHA-256 | `55eb2f2dece655a491141376916f4488a6714e221298fab26032d647fff2287f` |
| 架构 | x86 / PE32 |
| 插件描述 | Enhancer 0.17 / Enhancer v0.17 |
| DSP header version | `0x20` |
| 压缩特征 | PECompact |
| 本次加载基址 | `0x10000000` |

压缩文件中的原始代码字节不能直接当作插件业务代码。本次先仅执行 LoadLibrary，读取解压后的内存，再交叉验证入口、导入函数地址和初始化调用。

下述地址为本次加载基址下的 VA；适配代码应使用 RVA 并核对文件哈希、目标指针和代码特征。

## 3. 出错调用的精确位置

| 地址 | 含义 |
| --- | --- |
| `0x10005DF0` | `winampDSPGetHeader2` |
| `0x10011340` | DSP header |
| `0x10011350` | 第一个 DSP module |
| `0x100066F0` | Init |
| `0x10005430` | 创建／读取配置分支 |
| `0x10005470` | 调用 `RegCreateKeyExA` |
| `0x10005476` | 检查 API 返回值 |
| `0x10005930` | 失败分支 |
| `0x1000593C` | 调用 `MessageBoxA` |
| `0x10011440` | “无法访问注册信息...”字符串 |
| `0x10011474` | `SOFTWARE\Ioscasoft\Enhancer\Version 017` |
| `0x101C49DC` | 保存配置键句柄的全局变量 |

关键汇编：

```asm
1000545B  push 000F003Fh       ; KEY_ALL_ACCESS
...
1000546B  push 80000002h       ; HKEY_LOCAL_MACHINE
10005470  call [1000F000h]     ; RegCreateKeyExA
10005476  test eax, eax
10005478  jne 10005930h
...
10005930  push 0
10005931  push 10011468h       ; "Error..."
10005936  push 10011440h       ; 中文错误文字
1000593B  push 0
1000593C  call [1000F144h]     ; MessageBoxA
```

按真实控制流整理的逻辑：

```cpp
void LoadConfiguration() {
    status = RegCreateKeyExA(
        HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Ioscasoft\\Enhancer\\Version 017",
        0, classBuffer, REG_OPTION_NON_VOLATILE,
        KEY_ALL_ACCESS, nullptr, &configurationKey, &disposition);

    if (status != ERROR_SUCCESS) {
        MessageBoxA(nullptr, "无法访问注册信息...", "Error...", MB_OK);
        return;
    }

    if (disposition == REG_CREATED_NEW_KEY)
        WriteDefaultParameters();
    else {
        EnumerateDwordParameters();
        ReadSkinFileSubkeyDefaultValue();
    }
}

int Init(Module* module) {
    PrepareControlsAndState();
    LoadConfiguration();
    CreatePluginWindowAndTimers();
    return 0;
}
```

`Init` 没有把注册表错误向上传递：本机日志为 `status=5 → MessageBoxA → Init returned=0`。因此“列表已勾选”“Init 返回成功”不足以证明设置已正常载入或能够保存。

## 4. 插件到底存储了什么

初始化配置读取是 `RegEnumValueA` 枚举 DWORD；新键会写入 14 个初始 DWORD。退出路径 `0x100068D0 → 0x100050C0` 再次保存配置。

| 分类 | 名称 |
| --- | --- |
| 音效参数 | `Volume`、`Harm Bass`、`Harm Bass range`、`Drum Bass`、`Drum Bass range`、`Dry Signal`、`Harm Treble`、`Harm Treble Range`、`Ambience`、`Ambience Range` |
| 增强开关 | `Boosted` |
| 窗口状态 | `Window X`、`Window Y`、`Window State` |
| 皮肤路径 | **`Skin File` 子键的默认字符串值** |

皮肤路径使用旧式 API：

```cpp
RegQueryValueA(configurationKey, "Skin File", buffer, &bufferBytes);
RegSetValueA(configurationKey, "Skin File", REG_SZ, path, length);
```

它不是根键中名为 `Skin File` 的普通命名值。未来适配时若错误映射成 `RegSetValueExA(key, "Skin File", ...)`，可能消除了弹窗，却丢失皮肤设置的正确读写语义。

这条失败检查发生在创建插件窗口前，不是读取 `Enhancer/enhancer.set` 或 BMP 文件失败引发的。预设与皮肤资源仍须保留，但补拷这些文件不能修复当前这次注册表拒绝访问。

## 5. 与原版伪代码、重建版加载流程的比较

原版 `reverse/decompiled/TTPlayer.exe.pseudo.c`：

- `FUN_0042822F`：LoadLibraryW，获取 `winampDSPGetHeader2`，校验版本大于 `0x1F`。
- `FUN_00427EDC`：寻找第一个 module，写入 `module+4` 的父窗口和 `module+8` 的 DLL 实例，调用 `module+0x10` 的 Init。
- 这段宿主加载路径没有针对 Enhancer 的 HKLM→HKCU 重定向、预建注册表键、权限提升或吞掉该错误的代码。
- 原版并未检查 Init 的整数返回值；而此插件即使弹出注册表错误仍返回 0，单纯增加 Init 返回值检查也不能解决问题。

重建版 `src/audio/winamp_dsp.cpp` 同样填写 module 的父窗口与 DLL 实例，然后调用 Init。使用独立消息线程以及 LoadLibraryExW 的依赖搜索方式，不改变插件中这个硬编码的 HKLM 目标及 `KEY_ALL_ACCESS`。

先前在诊断工具启动的原版、预置 XML 启用插件的路径中，于 `0x10005470` 前后观察到：

```text
root=80000002
access=000f003f
RegCreate result=5
```

这不是用户后来完成的手动勾选测试的返回值。手动勾选后已经读取到有效的配置句柄。

### 5.1 “右键 → 千千选项 → 音效插件 → 勾选”的处理链

`FUN_00490CB8` 是音效插件页的消息分派：列表控件 ID 为 `1064 (0x428)`。

1. `NM_CLICK (-2)` 记录点击行到页面对象 `+0x58`。
2. `LVN_ITEMCHANGED (-101)` 调用 `FUN_0049911C`。
3. `FUN_0049911C` 排除列表填充阶段，核对行号与状态图像；需要启用时调用 `FUN_00428471(..., true)`。
4. `FUN_00428471` 建立活动模块记录，再调用 `FUN_00427EDC`。
5. `FUN_00427EDC` 填写父窗口和 DLL 实例，调用插件 Init。

因此勾选会实际初始化插件，并非等到“全部保存”才启用；本次有效句柄与插件窗口也排除了“只改变了勾选外观、未真正启用”的猜测。这条宿主调用链没有额外给 Enhancer 注册表写权限的分支。

没有证据表明是 DSP ABI 参数偏移、音频格式、WaveOut/WASAPI 切换或 PCM 算法触发当前提示。

## 6. Manifest 与系统权限的关系

原版 EXE 的 manifest 中，`requireAdministrator` 位于 XML 注释内，未生效。重建版则具有生效的：

```xml
<requestedExecutionLevel level="asInvoker" uiAccess="false" />
```

Windows 对符合条件的旧式 32 位程序提供注册表虚拟化；显式声明 requestedExecutionLevel 会禁用这种兼容行为。其适用条件还包括进程类型、权限和注册表位置等，不能把“没有 manifest 声明”等同于“所有 HKLM 写入必定成功”。参考 [Microsoft：Registry Virtualization](https://learn.microsoft.com/windows/win32/sysinfo/registry-virtualization)。

### 6.1 用户正常操作路径的直接证据

本次已运行的原版进程中，以下字段有效：

| 项目 | 观察值 |
| --- | --- |
| EXE | `D:\Projects\Backup\TTPlayer\TTPlayer.exe` |
| DLL | `D:\Projects\Backup\TTPlayer\Plugins\dsp_enh.dll` |
| Init 保存的 module | `0x10011350` |
| 配置键句柄 | 非零，且可复制后查询键名 |
| 插件窗口、消息钩子 | 均非零 |
| TokenElevation | `0` |
| TokenVirtualizationEnabled | `1` |
| 与读取诊断的用户身份比较 | 相同，仅记录布尔值，没有输出 SID |

以不启用注册表虚拟化的 64 位检查进程查询：

```text
HKLM\SOFTWARE\Ioscasoft\Enhancer\Version 017
  32 位视图：不存在

HKCU\Software\Classes\VirtualStore\MACHINE\SOFTWARE\WOW6432Node\Ioscasoft\Enhancer\Version 017
  64 位视图：存在，1 个子键、14 个值
```

对插件句柄执行原生键名查询，仍可看到 `\REGISTRY\MACHINE\SOFTWARE\WOW6432Node\Ioscasoft\Enhancer\Version 017` 的逻辑名称；不能据这个名称就声称配置物理写入了 HKLM。结合未虚拟化的 HKLM 查询及 VirtualStore 中的实际配置，才能识别 Windows 提供的虚拟化视图。

**迁移时的视图细节：**本机是 64 位系统，以上 VirtualStore 位置需使用正确的 64 位视图读取；先前对该显式包含 `WOW6432Node` 的路径使用 32 位视图，返回了“找不到”，不能把它解释成用户没有旧配置。未来 32 位重建版的迁移代码必须分别验证 64 位系统的 VirtualStore 视图和 32 位系统的路径。

### 6.2 保留但限定先前诊断结果

先前的诊断启动结果为：

- 重建版清单的诊断宿主：`virtualization enabled=0`，创建失败 5。
- 原版清单的诊断宿主：`virtualization enabled=1`，仍失败 5。
- 工具启动指定路径的原版、预置 XML 启用插件：`virtualization enabled=1`、未提升权限，仍失败 5。

这些数据如实保留，但不能代替用户通过正常界面启动／启用插件的结果。诊断上下文中为何仅有 TokenVirtualizationEnabled 仍不足以让调用成功，本次尚未追踪到最底层差异，不能随意归结为线程、时序或原版勾选代码有特殊权限。现在已有真实成功进程及 VirtualStore 证据，能够明确识别原版依赖的兼容机制与重建版的缺口。

XP 没有 Vista 引入的这套 UAC 注册表虚拟化。XP 测试成功表示该测试账户有足够的注册表访问权限，不代表 XP 的受限用户也能成功。不能简单按“XP 能用、新 Windows 不能用”划分。

## 7. 实际测试结果与限制

| 测试 | 注册表调用 | 提示 |
| --- | --- | --- |
| 本机普通权限，重建版清单的诊断宿主 | 返回 5 | 捕获到 |
| 本机普通权限，原版清单的诊断宿主 | 返回 5 | 捕获到 |
| 本机原版副本，硬件断点观察 | 返回 5 | 在弹窗前结束诊断 |
| 工具启动指定原版路径，预置 XML、硬件断点观察 | 返回 5 | 在弹窗前结束诊断 |
| 工具启动指定原版路径，预置 XML、不附加调试器 | 配置句柄为 0 | 实际捕获“无法访问注册信息...” |
| 用户按指定菜单路径手动勾选，随后只读检查实际进程 | 有效配置句柄；VirtualStore 中有参数与皮肤子键 | 用户确认没有提示，插件已实际初始化 |
| Win7，MCP 当前提升权限测试账户 | 返回 0，新建键 | 没有；Init 返回 0 |
| XP，MCP 当前测试账户 | 返回 0，新建键 | 没有；Init 返回 0 |

Win7 获取关联普通用户令牌的诊断入口未能完成启动，**没有把它计入普通权限通过测试**。表中的 Win7 结论仅覆盖提升权限测试进程。

诊断保护与记录：

- 宿主诊断捕获 MessageBox，并拦截 `RegSetValueExA/RegSetValueA` 的配置写入；它验证真实注册表创建返回值和插件 Init 分支，不是配置持久化验收。
- XP、Win7 测试仅清理此次新建且先前不存在的空分支，各级清理返回 0。
- 先前预置 XML 的受控测试在 finally 中恢复 `TTPlayer.xml`，内容逐字节一致，并恢复原时间戳。本次用户完成勾选后的设置保留，没有用旧备份覆盖用户新操作。
- 原版普通启动捕获到的模块路径就是用户指定目录下的 `Plugins/dsp_enh.dll`。
- 没有进行声音听感、预设重载或完整 DSP 算法一致性测试，不将本次 Init 成功扩大为完整兼容认证。
- 测试代码与产物仅位于本地 `rebuild/tests`；没有加入 Actions 或发行包。

主要本地证据位于 `tests/artifacts/dsp_static/enhancer/`：本次成功进程记录为 `ui-checked.json`，对应注册表位置、字段名和类型记录为 `ui-registry-metadata.json`（不含参数值）；早期诊断记录为 `modern.log`、`legacy.log`、`original-debug.log`、`original-exact-path.log`、`original-plain.json`、`win7-admin.log`、`xp-modern.log`、`disassembly.txt`。

## 8. 当前文件型注册表层缺少的内容

`PluginRegistry::Supports` 当前只接受经过哈希验证的 Ozone 和 DFX。Enhancer 会直接返回不支持，因此没有接管它的注册表调用，也没有读取 `PluginState/registry.json`。

这不是 Ozone/DFX JSON 导入不完整导致的；导入它们的 `.reg` 也不会解决 Enhancer 分支权限问题。

还不能只往 Supports 里增加 Enhancer 文件名／哈希：

1. 本 DLL 的内存业务导入槽为下表中的 5 项，而磁盘压缩壳可见的 ADVAPI32 导入仅有一个 `RegCreateKeyExA`。
2. 解压后的 PE 导入描述符仍指向压缩壳表，`OriginalFirstThunk == FirstThunk`，内容已经是解析后的函数地址。当前 Attach 将它当作名字 RVA 的遍历方式不能直接复用。
3. 当前 Replacement 没有 `RegSetValueA`、`RegQueryValueA`，皮肤子键读写会遗漏。
4. 当前配置分支识别按 Ozone/DFX 分流，需要增加独立 Enhancer 类型，而不是让它误入其中任一分支。

| 业务 IAT RVA | API |
| --- | --- |
| `0xF000` | `RegCreateKeyExA` |
| `0xF004` | `RegSetValueA` |
| `0xF008` | `RegSetValueExA` |
| `0xF00C` | `RegQueryValueA` |
| `0xF010` | `RegEnumValueA` |

## 9. 建议修复路径

**建议为这份经哈希确认的 Enhancer 增加插件范围的文件型注册表适配，配置并入现有 `PluginState/registry.json` 的独立 Enhancer 节点。**

实施顺序：

1. 增加明确的 Enhancer 类型、哈希白名单和唯一允许的配置子树。
2. 在 LoadLibrary 完成解压后、Init 之前，根据审计过的 RVA 及原函数指针接管 5 个业务导入槽；不直接遍历已被覆盖的名字表，也不扫描并替换整个进程。
3. 补齐 `RegSetValueA/RegQueryValueA` 的子键默认字符串值语义，保留 ANSI 转换、大小与结束符规则。
4. 保持 `REG_CREATED_NEW_KEY/REG_OPENED_EXISTING_KEY` 的区别，以及 DWORD 枚举返回值。它们决定插件是生成默认设置，还是恢复已有设置。
5. 在无文件配置的首次迁移中，只读检查该用户的旧虚拟化配置和 32 位 HKLM 配置。保留原版虚拟化视图中“用户覆盖机器值”的优先级；64 位系统须显式检查本次已验证的 VirtualStore 视图，不能套用 32 位 HKLM 的读取标志。迁移失败不能回退为机器级写入。
6. 生命周期结束时释放该插件持有的虚拟键并保存文件，复用共享存储的原子保存、文件锁和故障提示。
7. 验证普通权限启动无弹窗、修改参数后重启恢复、`Skin File` 子键、窗口位置、已有配置迁移、损坏／只读 JSON、多插件同时启用，并覆盖 XP/Win7/现代 Windows。

不建议通过要求整个播放器始终提升权限、删掉主程序权限清单、修改 HKLM 分支 ACL，或者只屏蔽 MessageBox 来完成长期修复。这些方法不能提供局限于 Enhancer 且可迁移的配置行为；隐藏弹窗也不能使失败的注册表读写变为成功。

相关设计背景：[插件文件型注册表实现](PLUGIN_FILE_REGISTRY_IMPLEMENTATION.md)、[共享注册表导入](SHARED_PLUGIN_REGISTRY_IMPORT.md)。
