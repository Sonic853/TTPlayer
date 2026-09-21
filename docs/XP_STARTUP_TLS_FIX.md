# XP 启动崩溃：ttpcomm 静态 TLS 加载修复

> 2026-09-21 补充：相关功能已在 XP SP3 / Win7 SP1 虚拟机中执行回归。覆盖项、修复和未覆盖边界见 [虚拟机验证记录](XP_WIN7_VM_VALIDATION.md)。下文保留原日期的历史结论。

## 结论

2026-09-18 在用户提供的 VirtualBox `XP` 虚拟机中复现。系统为 Windows XP Professional
SP3（5.1.2600），运行位置为 `C:\Documents and Settings\853\My Documents\TTPlayer`。
Z 盘映射宿主机 `D:\Program Files (x86)`。

原版 `ttpcomm.dll` 包含静态线程局部存储（TLS），需要由 XP 加载器在进程启动时分配。
重建版只有运行时 `LoadLibraryW` 调用，导致 DLL 的 TLS 索引仍为 0，与 EXE 的槽位重合。
启动过程调用 `ttpcomm!srand48` 后，DLL 将随机数种子写进 EXE 的 TLS，覆盖 C++ 运行库状态。
WTL 的局部静态初始化随后被错误跳过，消息循环表为空，注册消息循环时崩溃。

这次错误与文件复制、共享盘驱动、缺少 `CreateFile2` 入口无关，也不是 WTL 10.01 的绘制问题。

## 原版与重建版证据

1. C 盘崩溃 EXE、共享盘 EXE 和此前兼容构建的 SHA-256 相同：
   `6e57e4f9ff6b1b9e187a7e00888f5bc67a6bb3d56e0b23fad9b417c45bf579e5`。
2. XP 的 Dr Watson 日志与转储指向 `0x0040338D`，异常 `0xC0000005`。
   指令 `mov eax,[esi+8]` 的 `esi=0`，位于 WTL 消息循环映射的 `CSimpleMap::Add`。
   调用者从 `_Module.m_pMsgLoopMap` 取到空指针。
3. 原版 `TTPlayer.exe` 的 PE 导入表包含 `ttpcomm.dll!#3`，由系统启动加载。
   原版入口 `004C0E8F` 在加载资源后调用 `srand48(GetTickCount())`。
4. 原版伪代码 `ttpcomm.dll!60002980` 与实际 DLL 反汇编一致：
   先读取 `ThreadLocalStoragePointer[_tls_index]`，随后写偏移 `+8`、`+10`、`+12`、
   `+14`、`+16`、`+20`。`+12` 的种子高字和 `+14` 的 `0x000B` 覆盖了 EXE 的
   `_Init_thread_epoch`，使其从负数变为正数。
5. 崩溃 EXE 中 `EnsureWtlRuntime()` 比较静态初始化 guard 与上述 epoch。
   guard 初值为 0，被改成正数的 epoch 使构造路径被跳过；零初始化的 `HRESULT` 却等于 `S_OK`。

微软说明：Vista 之前，包含 `__declspec(thread)` 的 DLL 通过 `LoadLibrary` 加载时，
系统不能为它补充分配静态 TLS；访问这些数据可能导致保护错误。
参见 [MSVC Thread Local Storage](https://learn.microsoft.com/en-us/cpp/parallel/thread-local-storage-tls?view=msvc-170)。

## 修复

- `cmake/legacy_ttpcomm.def` 为原版 DLL 的序号 3 生成最小导入库，不需要原版二进制参与构建。
- 仅兼容版通过 `cmake/legacy_ttpcomm.cmake` 保留该启动导入。
  `/INCLUDE:__imp__srand48` 保证 `/OPT:REF` 和 LTCG 不会移除它。
  已有 API 适配器继续按序号解析函数，不新增 DLL 版本检查。
- 主程序和内嵌工作进程使用同一个 EXE，均获得系统分配的 TLS；线程创建和退出也由系统管理。
- 保留 C++ 线程安全局部静态初始化。没有修改原版 DLL，也没有修补线程内部数据或绕过 WTL。
- 静态导入审核强制要求 `ttpcomm.dll!#3`，其它应用 DLL 导入不会自动获准；打包再次验证该项和哈希。
  普通版加载策略不变。

## XP 验证

| 对照 | 结果 |
| --- | --- |
| 只用 `LoadLibrary` 的探针 | EXE TLS=0，DLL TLS=0；种子 `0x12345678` 将 epoch 从 `0x80000000` 改为 `0x000B1234`，返回 1 |
| 使用启动导入的探针 | EXE TLS=0，DLL TLS=1；epoch 保持 `0x80000000`，返回 0 |
| 三个并发线程 | 300 次设种子、3000 个随机值与预期一致；局部静态值正确，失败数 0 |
| 修复版复制到指定 C 盘目录 | 回读 EXE 哈希与构建产物一致 |
| 连续三次启动／自动关闭 | `--smoke-test` 均返回 0 |
| 普通启动 | 显示主窗体，启动文件关联提示可正常关闭，进程继续运行 |
| 界面与正常退出 | 主窗口响应消息，选项窗口正常打开；关闭后进程返回 0 |
| 构建审核 | 19 个 DLL、642 项导入通过 XP 与 Win7 清单及应用依赖检查 |

修复 EXE 的 SHA-256：`b3902ae770366b644dc57ba19f57bcac6138e5e90da324ac042b66cfcd276dfb`。
宿主机共享目录和来宾 C 盘目录均已更新，原 EXE 已分别备份。

本轮 XP 运行验证针对上述启动与 TLS 问题；不代表已覆盖全部音频格式、设备和选项。
Win7 尚未进行本轮虚拟机运行验证。

本地诊断和回归源码位于 `tests/platform/`；不提交测试子模块内容，也不在 Actions 中运行测试。
转储、截图、构建日志保存在本地 `out/xp-diagnosis-20260918/`，不随 ZIP 分发。
