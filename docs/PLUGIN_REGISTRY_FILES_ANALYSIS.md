# Plugins 目录内 REG 文件逐项分析

分析日期：2026-09-27。与 [音效插件实现分析](WINAMP_DSP_PLUGIN_IMPLEMENTATION_ANALYSIS.md) 配套。

> 后续修改：两份清理 REG 已缩小删除范围，三份添加 REG 已注明为历史安装快照；未导入或执行。本文各节仍描述修改前的原文件，当前实现及免系统注册表方案见 [DSP 修复文档](WINAMP_DSP_RECOVERY.md)。

## 1. 总结

目录中共有 **5 个 `.reg` 文件**，全部是 UTF-16LE（带 BOM）、`Windows Registry Editor Version 5.00` 格式。

它们分别写入 DFX 和 Ozone 的产品配置、安装路径、界面状态、授权登记资料。**它们不是 Winamp DSP 接口的注册步骤。** 原版 DSP 加载流程根据 DLL 导出 `winampDSPGetHeader2` 工作，没有在该流程中自动导入这些文件。

已确认的主要问题：

1. DFX 两个添加文件都写死在 C 盘的 TTPlayer 安装路径，与本次 D 盘目录不一致。
2. 两文件不仅区分注册表视图，还写入了不同窗口尺寸及部分状态；不能把所有差异归因于 x86／x64 系统。
3. DFX 删除文件会同时清理当前用户设置和两个机器级分支，不只是删除授权字段。
4. Ozone 删除文件删除的是整个 `HKCU\Software\iZotope`，范围大于 Ozone，会波及该分支下其它产品设置。
5. 添加文件是覆盖式合并，不是“只补缺失项”，也不是完整恢复出厂状态；未包含的旧值不会自动消失。

此次分析没有执行、导入、改写这些 `.reg` 文件，也没有读取本机已安装产品的私人注册表资料。文档保留字段名称和技术作用，不复制文件中的邮箱、姓名、授权码或机器标识值。

## 2. 文件清单与格式

| 相对 Plugins 的路径 | 字节数 | 节／键声明数 | 赋值数 | 作用 |
| --- | ---: | ---: | ---: | --- |
| `DFX/+添加 DFX 注册信息_x86.reg` | 6,692 | 48 | 35 | 写当前用户状态及机器级 DFX 配置 |
| `DFX/+添加 DFX 注册信息_x64.reg` | 7,496 | 50 | 37 | 写相同用户分支及 WOW6432Node 下机器级配置 |
| `DFX/- 清除 DFX 注册信息.reg` | 360 | 3 个删除节 | 0 | 删除用户分支及两种机器分支 |
| `Ozone/+添加iZOzone注册信息.reg` | 574 | 3 | 4 | 写 Ozone 用户登记资料与 Minimized |
| `Ozone/-移除iZOzone注册信息.reg` | 164 | 1 个删除节 | 0 | 删除整个 iZotope 用户分支 |

数量按文件文本统计，不代表已经访问注册表。DFX 所有赋值都是 `@="..."`，即子键的默认 `REG_SZ` 字符串；Ozone 有三个命名字符串值和一个 `REG_DWORD`。

### 2.1 容易误解的语法

```reg
[HKEY_CURRENT_USER\Software\DFX\9\11\LASTUSED_DFXG\allowSnapping]
@="1"
```

这表示创建 `allowSnapping` **子键**，并把该键的默认字符串设为 `"1"`。它不等价于在 `LASTUSED_DFXG` 中创建 DWORD 值 `allowSnapping=1`。将结构“简化”为命名值可能导致插件读取失败。

另外：

- `@=""` 是写入空字符串，不是删除默认值。
- 只有 `[键路径]` 而没有赋值，会确保键存在，不会清空该键已有内容。
- `[-键路径]` 删除整个键及子项；删除字段的语法与此不同。
- `.reg` 中 `C:\\Program Files\\...` 的双反斜杠是字符串转义，实际路径为单反斜杠。
- 当前路径是 `REG_SZ`；不能简单换成 `%ProgramFiles%` 并假设插件会自动展开环境变量。

导入、覆盖和删除语义参见 [Microsoft：使用 .reg 文件添加、修改或删除注册表项](https://support.microsoft.com/en-us/topic/how-to-add-modify-or-delete-registry-subkeys-and-values-by-using-a-reg-file-9c7f37cf-a5e9-e1cd-c4fa-2a26218a1a23)。

## 3. DFX 添加文件

### 3.1 注册表结构

两个文件的用户分支相同：

```text
HKEY_CURRENT_USER\Software\DFX\9\11
    CURRENT_SONG
    date_last_used
    LASTUSED_DFXG
    LASTUSED_DFXP
    last_dynamic_update
    recording
```

机器分支则不同：

```text
x86: HKEY_LOCAL_MACHINE\SOFTWARE\DFX
x64: HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\DFX
    11
        Date_Installed
        host_plugin_folder
        Quick_Uninstall
        REGISTRATION
        subvendor
        Top_Folder
        top_host_folder
    Installation
    Shared
```

`9\11` 与机器分支 `11` 是该插件采用的配置命名；不能看到 `11` 就断定是 DFX 11 插件，更不能据此统一改成相同版本数字。当前包说明是 DFX 9.304，DLL 的 Config 直接构造 `SOFTWARE\DFX\11\top_folder`；末级 11 的完整产品／宿主编号含义本次不作猜定。

### 3.2 CURRENT_SONG、时间和录音状态

| 子键 | 写入值 | 分析 |
| --- | --- | --- |
| `CURRENT_SONG\song_name` | 空字符串 | 清空此配置记录中的曲目名 |
| `CURRENT_SONG\source_url` | 空字符串 | 清空来源记录 |
| `date_last_used` | x86 `1778629992`；x64 `1778506148` | 已固化的历史时间字符串，不会随导入更新 |
| `last_dynamic_update` | x86 `1778629992`；x64 `1778506111` | 已固化的更新时间字符串 |
| `recording\raw_fullpath` | 只声明键、没有赋值 | 不会清除已有默认路径 |
| `recording\writting_on` | `"0"` | 名称按原文件拼写；按字段含义是关闭写入状态，非音频输出设备设置 |

如果把这些十进制时间按 Unix 秒解释，对应北京时间分别为：

- `1778506148` → 2026-05-11 21:29:08。
- `1778506111` → 2026-05-11 21:28:31。
- `1778629992` → 2026-05-13 07:53:12。

这里的时间解释是根据数值模式作出的判断，不证明插件把这些字段用于授权期限或某种特定校验。实际含义仍应追踪对应读写调用，不能仅凭名称断言。

### 3.3 LASTUSED_DFXG：界面状态

下表中的值都是默认字符串 `REG_SZ`。

| 字段 | x86 | x64 | 说明 |
| --- | --- | --- | --- |
| allowSnapping | 1 | 1 | 从名称推断为窗口吸附开关 |
| disableSongPresets | 0 | 0 | 从名称推断为禁用逐曲预设的反向开关 |
| displayHeadphoneMsg | 1 | 1 | 耳机提示状态 |
| windFlags | 0 | 0 | 窗口位置／显示标志 |
| windMaxPosX／Y | 0／0 | 0／0 | 最大化位置记录 |
| windMinPosX／Y | 0／0 | 0／0 | 最小化位置记录 |
| windNormPosLeft／Top | 100／100 | 100／100 | 普通窗口左上角 |
| windNormPosRight／Bottom | 360／420 | 372／516 | 普通窗口右下角 |
| windShowCmd | 1 | 1 | 按常见 WINDOWPLACEMENT 语义是普通显示；需以插件读值路径为最终依据 |
| windVisible | 未写入 | 1 | 两文件并非完全相同状态；未写入不等于 0 |

按上述矩形计算，x86 是 **260×320**，x64 是 **272×416**。这只是保存的窗口矩形，不能解释为系统位数必须采用不同 UI 大小，也不能证明插件最终不会由皮肤重新计算尺寸。

### 3.4 LASTUSED_DFXP：处理器状态

| 字段 | x86 | x64 | 说明 |
| --- | --- | --- | --- |
| byAll | 未写入 | 0 | 从名称看与总旁路有关；具体语义待插件读值调用验证 |
| longest_buffer_msecs | 0 | 0 | 缓冲时长相关记录 |
| temporaryBypassAll | 0 | 0 | 临时总旁路状态 |

它们没有包含全部音效旋钮、滤波器系数或全部预设内容。导入这些键不能替代 `DFX` 下的预设和皮肤资源。

### 3.5 安装路径与辅助程序：已由 DLL 证实

| 字段 | x86 固定值 | x64 固定值 |
| --- | --- | --- |
| `11\host_plugin_folder` | `C:\Program Files\TTPlayer\Plugins` | `C:\Program Files (x86)\TTPlayer\Plugins` |
| `11\Top_Folder` | `C:\Program Files\TTPlayer\Plugins\DFX` | `C:\Program Files (x86)\TTPlayer\Plugins\DFX` |
| `11\top_host_folder` | `C:\Program Files\TTPlayer` | `C:\Program Files (x86)\TTPlayer` |
| `Shared\Top_Shared_Folder` | `C:\Program Files\TTPlayer\Plugins\Dfx` | `C:\Program Files (x86)\TTPlayer\Plugins\Dfx` |

**直接证据：** `Dsp_Dfx.dll` 的 Config 位于 `0x10001070`，依次：

1. 用 `%s\%s\%d\%s` 构造 `SOFTWARE\DFX\11\top_folder`。
2. `RegOpenKeyExW(HKEY_LOCAL_MACHINE, ..., KEY_QUERY_VALUE, ...)`。
3. `RegQueryValueExW` 读取默认值。
4. 拼接 `%s\%s\%s`，后两段为 `Apps`、`dfxwsettings.exe`。
5. `0x1000118D` 通过导入调用 `CreateProcessW`。

实际包内有 `Plugins\DFX\Apps\dfxwsettings.exe`，还有 `CABARC.EXE`。所以具体问题是 **目标路径可能指错**，并不是这个包没有附带设置程序。仅修改播放器扫描目录、使用 LoadLibraryEx 或成功加载 DLL，均不会自动改正此注册表读取结果。

若在当前目录进行部署，四个路径应根据实际位置生成：

```text
host_plugin_folder = D:\Projects\Backup\TTPlayer\Plugins
Top_Folder         = D:\Projects\Backup\TTPlayer\Plugins\DFX
top_host_folder    = 实际宿主 EXE 所在目录
Top_Shared_Folder  = D:\Projects\Backup\TTPlayer\Plugins\DFX
```

注意：重建版 EXE 可能在 `rebuild\build\Release`，此时 `top_host_folder` 不能机械设成仓库根目录。最终发行包位置还会改变，上面是分析映射，不是已经执行的注册表修改。

注册表键不区分大小写，因此文件中的 `Top_Folder` 与 DLL 查询的 `top_folder` 不构成此处的拼写错误。

### 3.6 其它机器级状态与授权字段

| 字段／分支 | 文件内容性质 | 分析 |
| --- | --- | --- |
| Date_Installed | 默认值为空字符串 | 不是导入时自动填写安装日期 |
| Quick_Uninstall | `"0"` | 安装／卸载相关状态，不能单凭此值推导运行条件 |
| subvendor | `"1"` | 渠道／子发行方标识一类的数据，确切映射未验证 |
| REGISTRATION\email | 固定邮箱字符串 | 授权登记资料，不是音频算法参数 |
| REGISTRATION\machine_id | 固定机器标识 | 不是从当前计算机实时计算 |
| REGISTRATION\password | 固定字符串 | 授权相关资料 |
| REGISTRATION\serialNumber | 固定序列信息 | 授权相关资料 |
| REGISTRATION\regcount | `"1"` | 登记计数类状态 |
| REGISTRATION\stat | `"2"` | 授权状态字段；不能仅凭数字认定激活成功 |
| Installation\Email | 空字符串 | 安装过程记录，与上方 email 为不同键 |
| Installation\Email_Sent | `"1"` | 安装过程状态；导入本身不会发送邮件 |

DLL 有 `dfxp_CheckCorrectPassword`、注册状态等诊断字符串，并导入注册表 API。这支持其使用授权数据的判断，但不证明文件中的固定资料对任意用户、机器和版本有效。README 的修改版说明也不能代替实际验证。

## 4. x86／x64 文件应如何理解

这套 DSP DLL **全部为 32 位**，`_x64.reg` 表示面向 64 位 Windows 的注册表路径安排，不表示存在 64 位 DFX DLL。

在 64 位 Windows 上，`HKLM\SOFTWARE` 对 32 位应用有重定向；这里的 `HKCU\Software\DFX` 和 `HKCU\Software\iZotope` 则属于共享的普通用户分支。因此两个 DFX 文件都使用相同 HKCU 路径是合理的，无需另建 HKCU 的 WOW6432Node。参见 [Microsoft：受 WOW64 影响的注册表项](https://learn.microsoft.com/en-us/windows/win32/winprog64/shared-registry-keys)。

| 环境 | 当前文件设计意图 | 限制 |
| --- | --- | --- |
| XP／Win7／Win10 的 32 位系统 | x86 文件中的 HKLM\SOFTWARE\DFX | C 盘硬编码目录仍必须匹配真实安装位置 |
| Win7／Win10／Win11 的常见 x64 系统，运行 32 位宿主 | x64 文件明确写入 WOW6432Node | 导入工具的进程位数／视图必须核对，不能只看文件名 |
| 任意系统上的 64 位宿主 | 无法直接加载这批 x86 DSP | 导入任何 reg 都不能解决 DLL 位数不匹配 |

更适合后续安装器的做法是用逻辑路径 `SOFTWARE\DFX`，显式选择 32 位视图并填写真实路径，避免依赖手写 WOW6432Node。现代 `reg import` 的 `/reg:32`、`/reg:64` 可以选择视图；若使用这些参数，模板也应采用相应逻辑路径，不能盲目叠加到现有硬编码文件。XP 的导入工具参数需单独核实，不把现代命令直接当成 XP 通用方案。参见 [Microsoft：reg import](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/reg-import)。

HKCU 作用于执行导入的用户；用另一个管理员账户导入，可能把用户配置写给管理员而非实际播放用户。HKLM 写入需要相应权限，不能用“把整个播放器永久设为管理员运行”替代正确的安装步骤。

## 5. DFX 清除文件

原文件明确执行三项递归删除：

```reg
[-HKEY_CURRENT_USER\Software\DFX]
[-HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\DFX]
[-HKEY_LOCAL_MACHINE\SOFTWARE\DFX]
```

影响包括：

- 当前用户的 DFX 全部分支，不局限于 `9\11`。
- 安装路径、窗口状态、歌曲记录、处理状态和授权资料。
- 两种机器级路径下的 DFX 配置；具体实际视图仍受导入环境影响。

因此文件名“清除 DFX 注册信息”容易被理解得过窄。它实际更接近“清空 DFX 产品配置”，不是添加文件的精确逆操作：添加只覆盖列出的键值，删除会清除此前已有的其它数据。它也不会删除磁盘上的 DLL、皮肤、预设和 EXE。

后续若提供“仅删除授权资料”，范围应限定到已确认的授权字段／分支，并保留用户路径与界面配置；若提供完全重置，应单独命名并列出范围。

## 6. Ozone 添加文件

文件写入：

```text
HKEY_CURRENT_USER\Software\iZotope\Ozone\Winamp2
    EmailAddress  REG_SZ
    RegName       REG_SZ
    RegCode       REG_SZ
    Minimized     REG_DWORD = 1
```

| 值 | 意义及确认程度 |
| --- | --- |
| EmailAddress | 注册邮箱；DLL 存在同名字符串和 RegQueryValueExA 调用引用 |
| RegName | 登记名称；DLL 存在同名字符串和读取引用 |
| RegCode | 登记码；DLL 存在同名字符串和读取引用 |
| Minimized | 文件确实写入 DWORD 1；名称提示与最小化有关，但本次在 DLL 可见字符串中未找到对应字面量，不能断言当前修改版一定读取或因此最小化 |

**二进制验证：** `dsp_izOzone.dll` 中 `0x59473634` 保存 `SOFTWARE\iZotope\Ozone\Winamp2`，`0x594199B0` 附近用 `HKEY_CURRENT_USER` 调用 `RegOpenKeyExA`；`0x59481970`、`0x59481960`、`0x59481968` 分别是上述三个授权字段名，并有读取／写入路径引用。

它没有 DFX 那样的硬编码安装目录。资源配置还涉及 DLL 中的 `Ozone\iZOzone.cfg` 字符串，实际目录也有该文件；导入 reg 不会安装或替代它。

`Winamp2` 是这个插件保存设置使用的产品子键名，TTPlayer 作为 Winamp DSP 宿主也需要让插件按自身设计读取它。不能因为宿主叫 TTPlayer 就把子键随意改名。

## 7. Ozone 删除文件

实际内容只有：

```reg
[-HKEY_CURRENT_USER\Software\iZotope]
```

它删除的是 **整个 iZotope 厂商分支**，并不只删 `Ozone\Winamp2`。如果该分支下存在 Vinyl 或其它 iZotope 产品的设置，也会一并删除。是否已经存在这些数据取决于用户环境，本次没有读取本机注册表来作判断。

适当的范围应按目的区分：

- 只移除 Ozone 登记资料：只删除 `Ozone\Winamp2` 内已确认的 `EmailAddress`、`RegName`、`RegCode` 值。
- 重置该 Ozone 插件的全部用户配置：最多删除 `Ozone\Winamp2` 子键。
- 删除整个 `iZotope` 分支不应作为 Ozone 单插件的默认卸载步骤。

本次记录问题和修正方向，未改动／执行原删除文件。

## 8. 与重建版问题的对应关系

| 现象 | 本次证据支持的原因 | reg 能否解决 |
| --- | --- | --- |
| DFX 配置打不开 | top_folder 指向旧位置，Config 据此寻找辅助 EXE | 正确迁移路径可能解决这条分支 |
| DFX 在不同系统／导入文件下窗口大小不同 | reg 里本来写入了不同矩形，另有皮肤布局因素 | 应统一意图明确的界面默认值，不能当作架构必然差异 |
| Ozone／Dolby／VAPXP 配置无反应 | 重建版配置 worker 没有 Init，未使用活动实例 | 导入 reg 不能修复宿主生命周期 |
| 非模态插件窗口一闪而过 | Config 返回后 worker 卸载 DLL 并退出 | reg 无法补上消息循环和实例存活 |
| 已能调参但声音不变 | 配置与声音处理可能处于不同实例 | reg 只在插件主动重读时才可能同步，不能代替同实例设计 |
| 删除 Ozone 后其它产品设置消失 | 原删除文件删除了整个 iZotope 分支 | 应先缩小删除范围 |
| 加载 DLL 失败／位数不兼容 | 插件文件、依赖、架构或其它 loader 问题 | 不能用导入授权资料替代排查 |

## 9. 建议处理顺序

1. 先修复重建版 DSP 同实例配置与生命周期；否则即使路径、授权资料完整，配置行为仍不等价。
2. 将 DFX 的运行路径配置、界面默认配置、用户授权资料分成独立内容；按实际宿主及插件目录生成路径。
3. 统一 x86／x64 文件中与架构无关的默认值，或干脆不在迁移路径时覆盖用户窗口位置和处理开关。
4. 缩小两个删除文件的范围，并区分“移除授权资料”和“完整重置该插件”。
5. 在隔离测试环境验证路径、视图、配置窗口和真实音频；不要自动导入整套固定授权资料作为所有用户首次启动流程。

结论：这些 reg 文件是特定安装环境的配置快照，混合了用户状态和产品登记信息。它们不是可直接用于任意 XP／Win7／Win10／Win11 安装位置的通用插件部署方案。
