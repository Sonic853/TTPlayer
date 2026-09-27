# 主配置 XML 无法写入程序目录时的行为与处理方案

分析日期：2026-09-28。

## 1. 结论与范围

原版 `TTPlayer.exe` 没有在主配置写入失败后，自行改存 AppData、注册表或临时目录的兜底分支。它根据 EXE 路径确定 XML 路径，直接覆盖写入；底层返回的失败没有传递到选项窗口或退出流程。

因此，设置可以在本次运行中生效，但退出后没有保存。某些 Windows 环境中，原版对受保护目录的写入可能被系统重定向到用户的 VirtualStore；这是 Windows 的旧程序兼容机制，不是原版自己的目录回退实现。

本次完成原版伪代码、关键调用点反汇编、EXE 清单与重建版源码分析，并在当前 Windows 主机运行原版的可写、文件只读、文件共享冲突三组对照测试。本文提出的改造尚未实施；没有修改产品源码或构建发行包。

## 2. 原版调用链

分析对象：`reverse/decompiled/TTPlayer.exe.pseudo.c`，对应原版 EXE SHA-256：

```text
c7999ea5823469c1684148cac1bd196009824348ccac5cdd45978f8fb574e28c
```

| 函数地址 | 作用 | 与保存失败有关的结论 |
|---|---|---|
| `00401829` | `GetModuleFileNameW` 取得程序路径，替换扩展名 | `TTPlayer.exe` 对应同目录 `TTPlayer.xml`，不是按当前工作目录查找 |
| `00402D9E` | 初始化默认配置，建立 XML 路径，加载配置 | 调用路径助手时容量为 `0x104`，即 260 个字符；主配置链没有第二保存目录 |
| `00401974` → `004C62AD` | 读取、解析 XML | 读取失败时保留此前初始化的默认设置 |
| `0040374B` | 主配置保存入口 | 整理设置后调用 `00401A9D` |
| `00401A9D` | 建立 XML 文档，调用设置序列化器和文件写入器 | 忽略 `004C6368` 返回值 |
| `004C6368` | 打开文件、序列化输出、写入和关闭文件 | 独占打开，`CREATE_ALWAYS` 覆盖写；无备份或切换目录分支 |
| `0049FE39` | 选项窗口命令处理 | “全部保存” `0x4D2` 调用主配置保存，没有失败提示或重试 |
| `004616BD` | 程序关闭处理 | 调用主配置保存后继续退出，没有保存失败补救 |

### 2.1 文件写入方式

`004C6368` 的核心语义如下，省略字符串与内存管理细节：

```cpp
HANDLE file = CreateFileA(path,
    GENERIC_READ | GENERIC_WRITE,
    0,                         // 不共享
    nullptr,
    CREATE_ALWAYS,             // 直接覆盖原文件
    FILE_ATTRIBUTE_NORMAL,
    nullptr);
if (file == INVALID_HANDLE_VALUE)
    return false;

SerializeXmlToBuffer();
BOOL written = WriteFile(file, buffer, length, &bytesWritten, nullptr);
CloseHandle(file);
return written != FALSE;
```

原版使用可增长的序列化缓冲区；初始分配的 `0x2000` 不是配置文件的固定大小上限。

这里有两类不同后果：

1. **打开文件就失败**：例如只读属性、无访问权限、共享冲突。通常旧文件尚未被截断，旧设置保留，本次修改丢失。
2. **打开成功，后续写入失败**：`CREATE_ALWAYS` 已经截断旧内容；磁盘写满、I/O 错误或过程中异常退出可能留下空文件或不完整 XML。原版没有先写临时文件再替换的保护。

该函数只使用 `WriteFile` 的布尔结果，没有核对写入字节数，也没有显式调用 `FlushFileBuffers`。而上层连这个布尔结果也没有处理：反汇编中 `00401BAF` 调用 `004C6368` 后，`00401BB4` 直接进入清理流程并覆盖 EAX。

`004C6368` 前面的 `00403D13` 是字符串处理调用，不是创建目录或选择备用目录。

微软对 `CREATE_ALWAYS` 的定义也明确说明：已有可写文件会被截断。[CreateFileA 文档](https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-createfilea)

### 2.2 不应混淆的其他实现

- 原版代码中确实有 AppData 路径，已定位的相关用途包括网络缓存；不能据此推断主配置会保存到该目录。
- 播放列表保存代码存在临时文件和移动文件流程，不能把它套用到 `TTPlayer.xml` 的写入方式上。
- XML 声明采用 UTF-8，但**文件路径**经过宽字符到当前代码页的转换后交给 `CreateFileA`。包含当前 ANSI 代码页无法表示的路径字符时，可能出现路径问题；这不等于“所有中文目录都不能用”。
- 配置名来自 EXE 文件名。重命名原版 EXE，也会改变它对应的 XML 名称；修改快捷方式的“起始位置”不能改变上述配置路径。

## 3. 为什么原版在有些受保护目录下仍然能保存

直接读取原版 EXE 内嵌清单，发现 `requestedExecutionLevel level="requireAdministrator"` 位于 XML 注释内，没有生效的执行级别声明。

在支持 UAC 的 Windows 上，符合条件的旧程序对受保护位置的写入可能被系统重定向到每用户位置。文件副本通常位于 `%LOCALAPPDATA%\VirtualStore\...`，所以用户查看程序目录时，可能看不到真正更新的那份配置。

需要区分以下边界：

- 这是 Windows 的兼容行为，受进程运行方式、UAC 策略和目标路径等条件影响，不能认为任何写入失败都会被重定向。
- XP 没有 UAC 文件虚拟化；原版在 XP 下也需要实际的目标写权限。
- 重建版 `src/app/ttplayer.manifest` 明确声明 `asInvoker`。微软文档说明，显式声明 `requestedExecutionLevel` 会禁用文件与注册表虚拟化。
- 这不是 VC-LTL 或 YY-Thunks 引起的保存 API 兼容问题。依赖这些构建组件不会自动提供配置目录回退。

依据：[应用程序清单](https://learn.microsoft.com/en-us/windows/win32/sbscs/application-manifests)、[UAC 设置与文件/注册表写入虚拟化策略](https://learn.microsoft.com/en-us/windows/security/application-security/application-control/user-account-control/settings-and-configuration)。

不建议删除重建版清单、关闭 UAC 或全局修改系统策略来恢复这种隐式行为，应显式管理用户数据位置。

## 4. 原版本地实测

本地探针：`tests/ui/settings_save_original.py`。

结果：`tests/artifacts/settings-save-20260928/observations.json`。

三组测试均使用隔离的程序副本和生成的配置/静音 WAV，不使用个人播放列表或配置。启动后发送原版静音命令，将 `Player/Mute` 从 1 改为 0，再通过正常 `WM_CLOSE` 流程退出。

| 场景 | XML 是否变化 | 保存后的 Mute | 退出情况 |
|---|---|---|---|
| 正常可写 | 是 | 0 | 正常退出 |
| XML 设置只读属性 | 否，原文件哈希不变 | 1 | 正常退出，本次修改未保存 |
| 启动后由测试句柄保持只读打开，仅允许读共享 | 否，原文件哈希不变 | 1 | 正常退出，本次修改未保存 |

三组原版退出码均为 1，与正常对照一致，不能把该程序的退出码 1 直接理解为崩溃。测试结束后关闭测试句柄并恢复测试 XML 的属性。

**验证边界**：本轮没有在 XP/Win7 虚拟机重跑，没有模拟磁盘写满、NTFS ACL 拒绝或实际 Program Files / VirtualStore 重定向。无备用路径的结论来自保存调用链分析；测试只验证上述三种条件下的实际保存结果。

测试代码与结果留在本地 `rebuild/tests`，不加入产品发行包或 Actions。

## 5. 当前重建版的状态

重建版主配置已经改为 `TTPlayerRebuild.xml`；同目录 `TTPlayer.xml` 是首次迁移来源。

1. `src/settings/runtime_settings.cpp::LoadRuntimeSettings`：优先读取新文件；新文件不存在时尝试复制旧文件。只读目录导致复制失败时，可以在内存中读取旧文件，但 `source_path` 仍指向同目录的新文件。这是读取兼容，**不是可持久化的备用保存目录**。
2. `src/settings/settings.cpp::SaveWindowState`：返回类型为 `void`，末尾调用 MSXML 的 `document->save(...)`，没有处理保存 HRESULT。
3. `src/ui/player_window_options.cpp`：“全部保存”无法据此判断是否真的写入成功。
4. `src/ui/player_window.cpp::PersistWindowState`：保存前就设置 `window_state_saved_ = true`，失败也没有可靠的成功状态反馈。
5. 皮肤配置保存已有布尔结果，但 `SaveCurrentSkinProfile` 的调用路径忽略该结果。

重建版采用 MSXML 保存，不能直接认定它内部使用了和原版完全一样的 `CreateFileA` 参数。不过，忽略保存错误、没有用户目录回退的问题同样存在。

## 6. 不改程序时的处理方法

| 原因 | 可行处理 | 注意事项 |
|---|---|---|
| 程序安装目录不允许当前用户修改 | 把完整播放器目录放到当前用户拥有写权限的位置，或者给专用数据目录配置合理的当前用户权限 | 换到 D 盘不必然可写；不要只移动 EXE 而遗漏配置、列表、皮肤等 |
| XML 文件带只读属性 | 退出播放器，备份配置，确认允许更新后清除该文件的只读属性 | 文件夹属性页的“只读”框不等于 NTFS 写入权限 |
| XML 被其他进程以不兼容共享方式打开 | 释放占用句柄，然后重新保存 | 不是所有编辑器都会持续占用文件，应先确认原因 |
| 磁盘已满或介质只读 | 腾出空间，或迁移到可写介质 | 提权不能解决介质只读或空间不足 |
| 原版 ANSI 路径转换或路径长度问题 | 使用较短、当前系统代码页可表示的目录路径 | 属于原版路径限制，与 XML 正文的 UTF-8 编码不同 |
| 原版出现两份配置、编辑程序目录 XML 没有效果 | 检查当前用户的 VirtualStore 中是否有对应副本，并备份、核对实际生效文件 | 不要直接删除未知副本或假定所有机器都会生成它 |

以管理员身份运行可能解决某些目录权限问题，但不适合作为默认方案，也不能解决所有上述原因。虚拟化视图还可能随运行权限不同而改变，导致读取不同配置。

## 7. 建议重建版采用的修复顺序

以下为建议设计，不是当前已实现行为。

### 第一阶段：可靠反馈保存结果

- 用结构化保存结果返回成功/失败、目标路径、失败阶段及 HRESULT/Win32 错误。
- “全部保存”失败应显示实际原因；设置仍保留在内存，允许用户重试或导出。
- 只有确认完成持久化后，才设置保存成功标记；全局配置与皮肤配置的结果需要分别记录。
- 退出失败时提供重试、另存或放弃本次修改的明确选择，避免看似保存成功。

### 第二阶段：保护已有 XML

- 先在内存完成序列化，再在目标目录建立唯一临时文件。
- 使用 Unicode 文件 API，核对全部字节写入结果，刷新并关闭临时文件。
- 已有目标采用带备份的 `ReplaceFileW`；首次创建使用合适的移动操作。逐步检查结果，失败后保留可恢复文件。
- 不要先删除旧文件再移动新文件；同时处理替换过程中可能出现的特殊失败状态。

`ReplaceFileW` 支持 XP。临时文件、目标和备份应在同一卷；不能把替换操作描述为对所有断电和文件系统错误都绝对安全。`REPLACEFILE_WRITE_THROUGH` 标志不受支持，`REPLACEFILE_IGNORE_ACL_ERRORS` 在 XP 不可用，不应误用。[ReplaceFileW 文档](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-replacefilew)

### 第三阶段：明确的便携目录 / 用户目录策略

- 程序目录可写时，保留现有便携使用方式。
- 安装目录不可写时，使用当前用户的持久数据目录，例如：

```text
%APPDATA%\TTPlayerRebuild\Profiles\<安装目录标识>\TTPlayerRebuild.xml
```

- 该路径是建议方案；安装目录标识用于避免多份便携播放器互相覆盖。XP 与新系统均可通过 `SHGetFolderPathW(CSIDL_APPDATA | CSIDL_FLAG_CREATE)` 获取用户目录，不需要硬编码各系统的物理目录。[SHGetFolderPathW 文档](https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shgetfolderpathw)
- 回退时写入当前内存中的最新设置，不能只复制程序目录里的旧 XML。
- 选定用户目录后，要在可写的用户位置持久记录选择，保证下次启动仍读取同一份设置。否则权限变化后可能重新加载旧的程序目录 XML，表现为设置回退。
- 临时文件可创建不代表已有目标可替换，需要处理目标自身只读属性及替换权限。
- 文件主动设为只读可能是用户希望固定配置；应提供“不保存”或“保存到用户目录”的选择，不擅自清除属性。
- 共享冲突可做有限次数重试；磁盘写满和临时占用不应无条件触发永久切换配置目录。

### 第四阶段：统一管理其他可变数据

仅更改主 XML 的位置还不够。播放列表目录、`Music.library`、歌词关联、皮肤配置、插件状态和预设等可变数据都应审查并纳入用户数据目录策略。

例如当前播放列表目录仍由 `player_window_playlist.cpp::LoadStoredPlaylist` 定位到程序目录；媒体库也使用运行目录。而 DSP 的 `PluginState` 已相对 `settings_.source_path.parent_path()` 定位。只改一个路径会使这些数据分散，出现“设置能记住，列表或窗口位置却记不住”的问题。

插件 DLL、皮肤包和程序资源等静态资产继续从程序位置加载。迁移时保留原文件、未知 XML 字段和明确的优先顺序，避免覆盖用户已有数据。

如果用户目录也无法写入，最后只能保留本次会话设置并明确告知，或让用户选择可写的导出位置；不应把临时目录当作可靠的长期配置存储。

## 8. 后续实施时的验证矩阵

- XP、Win7、Windows 10/11：程序目录可写与不可写，新旧配置迁移，连续两次启动保持一致。
- 文件只读、共享冲突、替换权限不足、写入中失败：正确报告原因，旧配置或备份可恢复。
- 首次不存在配置、已有配置、损坏配置、多份程序目录及未知 XML 字段的保留。
- 用户目录回退后，“全部保存”、正常退出、皮肤窗口位置、播放列表和插件状态均从同一数据策略恢复。
- 配置只读、不保存选择、重试与导出，以及没有可写持久目录时的明确反馈。

推荐总体方案：**保留便携使用方式，增加显式用户目录回退、可靠的保存错误反馈和替换保护，而不是复制原版的静默失败。**
