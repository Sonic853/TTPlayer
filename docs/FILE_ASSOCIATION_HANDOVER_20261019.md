# 双份安装的文件关联交接与回滚修复

日期：2026-10-19。范围：用户反馈“两个不同安装目录之间切换关联，提示注册表项由其他程序管理、另一个程序正在使用此文件”。

## 1. 结论

本次已确认的主要原因是重建版的关联归属冲突，不是音频文件被占用：关联后端把 `TTPlayer.Rebuild.Owner` 中的旧 EXE 路径与当前 EXE 路径比较，遇到不同安装目录就自行返回 `ERROR_SHARING_VIOLATION`（32），再经 `FormatMessageW` 显示成系统的“进程无法访问文件”文字。单凭这一弹窗不能判断为安装文件或音频文件锁定。

重建版继续使用共享的 `TTPlayerRebuild.Audio.<扩展名>`、`TTPlayerRebuild\Capabilities` 和 `RegisteredApplications` 名称。本次加入明确、一次性的安装目录交接，不把同一播放器复制成多个默认程序条目，也不让启动时的自动检查在 A、B 之间互相抢回关联。

## 2. 5.7.9 原版伪代码依据

源文件：`reverse/decompiled/TTPlayer.exe.pseudo.c`；地址对应 5.7.9 原始 `TTPlayer.exe`。本节是静态证据，不冒充本轮运行原版并修改真实关联的结果。

| 函数 / 伪代码行 | 已确认行为 | 对重建版的约束 |
|---|---|---|
| `0049B3AA`，147944–147963 | 用当前 EXE 的完整路径组成带引号的播放命令；区分普通打开与 `/a` 添加 | 交接后必须把 open/add 命令指向明确启动的安装目录 |
| `0049B420`，147973 起；148127–148150、148270–148296 | 查询关联并比较当前 EXE 命令 | 不能只看扩展名默认值是否等于共享 ProgID 来判断“属于当前安装” |
| `0049BB64`，148319 起；148465–148505 | 清理旧 Explorer 覆盖，处理 Vista/Win7 UserChoice，使用共享 `Audio.<扩展名>`，备份并设置扩展名默认值 | 原版是共享注册、当前目录接管；没有现在这种因另一个重建版 Owner 而伪造文件占用错误的机制 |
| `0049BB64`，148404–148433、148524–148534 | 发布播放/添加菜单和默认图标 | 交接范围需要包含菜单命令与图标，不能仅改顶层 Owner |
| `0049C3D4`，148614 起 | 恢复备份或清除原关联 | 保留最早第三方备份，A→B 时不能把 A 再存成新的“原默认程序” |
| `0049CA63`，148934 起；`0049D8C0`，149700–149739 | 分发及批量执行关联变更，并通知 Shell | 选项页批次与注册表通知应有明确边界 |
| `0049CD41`，149097–149165 | `/unreg` 先调用当前 EXE 的归属检查，再清理扩展名与菜单 | 旧安装卸载应保护新安装，不是无条件删除共享类 |
| `0049DFB4`，150009 起 | 使用 `ttpsvr.exe` 辅助执行关联流程 | 不能把历史辅助程序假设成两个安装目录各有独立默认程序身份 |

现代 Windows 不照搬原版删除受保护 UserChoice 的做法。注册候选程序与系统最终选定默认程序是两步；Windows 8 以后旧设置默认程序 API 有弃用限制，Windows 10/11 保持系统选择流程。依据：[Microsoft Default Programs](https://learn.microsoft.com/en-us/windows/win32/shell/default-programs)、[IApplicationAssociationRegistration](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-iapplicationassociationregistration)、[Windows app defaults platform](https://learn.microsoft.com/en-us/windows/apps/develop/windows-integration/default-apps-platform)。

## 3. 修复的具体问题

### 3.1 归属冲突使用业务错误

- 增加 `FileAssociationError::ownership_conflict`；不再用原生错误码 32 表达安装目录不同。
- 返回旧安装列表、当前 EXE 路径和冲突注册表路径；真正的注册表拒绝访问等错误仍保留原生错误码。
- 选项页明确显示已有安装和当前安装，默认按钮为“否”；用户选择“是”才执行一次交接。取消会重新读取实际状态，不继续打开默认程序设置，也不显示伪造的失败弹窗。
- 启动时的自动关联检查不会自动交接到当前安装。

### 3.2 整批预检查与失败回滚

- `AssociationUpdate` 把候选格式、扩展名勾选、图标、文件夹与音频 CD 菜单变更组织成一个批次。
- 写入前检查批次中的归属、元数据类型和可写权限，拒绝时不先留下一个新 Capabilities Owner。
- 使用命名互斥量串行执行本版程序的关联更新；注册表修改记录旧值、类型、是否存在、新建键和删除键。
- 中途失败逆序恢复成功写过的部分；删除过的键恢复原值及 DACL。Win7 UserChoice 可拒绝 `KEY_SET_VALUE`，预检查不会误把“不能改值”解释为“不能删除整个旧键”；恢复时先写回值再恢复原权限。
- 回滚前对比当前位置是否仍等于本次最后写入的值。若外部程序已改写，不覆盖该外部修改，而返回 `rollback_failed` 和恢复失败说明。
- 批次成功后只按实际变化统一通知 Shell；不逐个扩展名通知。
- `/unreg` 的格式登记、目录菜单、音频 CD 菜单也由 `UnregisterAllAssociations` 放在同一事务中；不再出现前一段成功、后一段失败却无法整体恢复的情况。

### 3.3 A→B→A 交接与历史半套状态

- 交接前重新枚举重建版已管理的共享登记，不仅处理这次点击的一个扩展名。确认提示会列出此次会涉及的已知旧安装路径。
- 转移 Owner；只有当前值仍等于旧安装记录的 Installed 值时，才替换精确匹配旧 EXE 的播放命令、添加命令和默认 EXE 图标。
- 保留自定义图标、描述，以及原有 `Backup`、`BackupPresent`、`KeyExisted`。它们由新安装继承，最终取消关联仍可恢复原第三方程序。
- Win7 的旧 UserChoice 备份跟随 Owner；Windows 8+ 的实际 UserChoice/Hash 不参与改写。
- 对历史“Capabilities 已属于 B，但类及扩展名仍属于 A”的部分写入状态，普通登记先拒绝且不再继续写入；用户确认交接后在同一批次修复。
- 对历史 `Audio.*`，只有存在重建版自己的 Owner 标记才迁移；不会把原版播放器自己的类当作可任意清理的重建版类。
- 旧 A 在 B 接管后取消关联/卸载是成功的无操作，不删除 B 的 OpenWith 项和共享登记；第三方后来选定的默认值也不会被旧备份强行覆盖。

### 3.4 `/reg`、`/unreg` 的单实例路由

普通打开文件继续保持原有全局单实例行为。显式启动某目录 EXE 的 `/reg` 或 `/unreg` 使用单独的维护 IPC 名称，不把该命令转发给另一个目录已运行的播放器。关联操作仍由后端互斥量串行执行。`/unreg` 失败现在返回非零退出码。

这一路由已做源码核查并参与主程序构建；本轮没有对真实用户关联执行 `/unreg`，因此不把它记录为真实 Shell 关联的端到端卸载验证。

## 4. 修改位置

| 文件 | 主要内容 |
|---|---|
| `include/ttplayer/settings/file_association.h` | 归属冲突结果、批次数据结构、整批取消关联入口 |
| `src/settings/file_association.cpp` | `OwnershipConflict`、`RegistryJournal`、全批次预检查、Owner 交接、备份继承、卸载保护 |
| `src/ui/player_window_options.cpp` | 一次性交接提示、选项页整批提交、取消刷新、启动不自动接管、整批 `/unreg` |
| `src/app/main.cpp` | 维护命令与普通播放 IPC 分开 |
| `src/app/application.cpp` | 传递 `/unreg` 的实际成功或失败状态 |

本次未修改安装器自身的文件复制、删除逻辑。安装器文件确实被占用、杀毒软件拦截、真实注册表 ACL 拒写仍需依据实际路径和原生错误另行判断。

## 5. 测试与结果

测试代码与产物仅保存在 `rebuild/tests/settings/association_20261019`，不提交到播放器仓库，Action 不运行它们。沿用的私有 `tests/settings/default_programs_tests.cpp` 断言已更新为“另一安装登记返回明确冲突、另一安装卸载安全无操作”。

隔离方案：所有测试均使用独立测试树；新交接回归为每次测试创建 `HKCU\Software\TTPlayerRebuild\AssociationHandoverTests\<进程及轮次>`；后端 Classes、Capabilities、RegisteredApplications、Explorer 覆盖均映射到测试树，并关闭 Shell 通知。测试结束清理自己的树。未修改主机或虚拟机的真实默认文件关联。

| 测试程序 | Windows 11 本机 | XP 虚拟机 | Win7 虚拟机 |
|---|---|---|---|
| `file_association_tests.exe` | 通过，退出码 0 | 通过，退出码 0 | 通过，退出码 0 |
| `default_programs_tests.exe` | 通过，退出码 0 | 通过，退出码 0 | 通过，退出码 0 |
| `association_handover_tests.exe` | 通过，退出码 0 | 通过，退出码 0 | 通过，退出码 0 |

测试覆盖：

1. XP、Vista/Win7、现代 UserChoice 三种后端分支；中文/空格安装路径。
2. A 首次登记后 B 未授权被拒绝，整个测试注册表树保持不变；错误不是 32。
3. A→B、B→A；MP3/FLAC、open/add、目录与 CD 菜单、默认图标、自定义图标和原第三方备份。
4. 旧安装卸载不损坏新安装；最终当前安装卸载恢复原默认值和目录菜单。
5. 旧版缺少 Capabilities、Owner 分裂、外部后来改变默认值的场景。
6. 现代 UserChoice Hash 保持不变；Win7 原 UserChoice 值及拒写权限恢复。
7. 逐次注入 `ERROR_ACCESS_DENIED`：首次安装 **242** 个 API 修改位置、交接 **70** 个位置、整批卸载 **205** 个位置，共 **517** 个失败位置。每次比较整棵测试树的键存在性和全部值的类型、字节，要求完全恢复；Win7 拒写场景另检查 `KEY_SET_VALUE` 仍被拒绝。
8. 回滚期间第三方并发改值：保留第三方值并明确返回恢复冲突。

本地结果：`win11-results.log`、`xp-results.log`、`win7-results.log`；虚拟机测试目录为 `C:\TTPAssoc20261019`。项目按 VC-LTL/YY-Thunks 的 XP 兼容配置编译；测试 EXE 仅依赖系统 DLL，无需复制播放器 DLL。

## 6. 验证边界与后续

- 本轮是隔离注册表与后端行为验证；没有声称 Windows 默认应用设置界面的实际选择、图标缓存刷新和 Explorer 双击启动均已完成端到端测试。下一轮可在可回滚的干净 VM 中人工确认这些 Shell 行为。
- Windows 10 没有独立测试环境；Windows 11 已执行现代分支测试，不能把它写成在 Windows 10 上运行过。
- 事务日志位于内存，处理本次覆盖的 API 失败，不提供断电、强制终止进程后的持久恢复，也未验证内存耗尽时的恢复。若以后增加安装器迁移工具，可另设计持久恢复日志；本次不声称具备它。
- 互斥量只协调遵守同一名称的新实现。其它应用、旧版程序仍可能并发写入；本次提供预检查、提交时归属复核及回滚时的值对比保护，不把注册表操作宣称为跨所有程序的隔离事务。
- 文件系统复制/卸载错误不由该注册表修复掩盖；保留原生错误便于定位真实问题。
