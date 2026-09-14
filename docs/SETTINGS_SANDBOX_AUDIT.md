# 设置窗口宿主机与 Windows Sandbox 对照审计

## 反编译入口

`TTPlayer.exe.pseudo.c` 的主窗口 `WM_COMMAND` 分派（约
`004582D0`）将下列命令送入同一个 `FUN_0045D531` 设置外壳：

| 命令 | 原始入口 | 原始菜单资源 | 首次显示页（沙箱实测） |
|---|---|---|---|
| `0xE140` | 主窗口“千千选项...” | 138 | 关于 |
| `0x7918` (`31000`) | 皮肤子菜单“选项...” | 155 | 皮肤 |
| `0x8084` | 视觉效果“选项...” | 145 | 视觉效果 |
| `0x7EFD` | 播放列表曲目菜单“选项...” | 152、153 | 播放列表 |
| `0x7FEF` | 媒体库菜单“选项...” | 159 | 媒体库 |
| `0x8023` | 歌词秀“选项...” | 143 | 歌词秀 |

歌词搜索对话框还有一条非菜单入口：控件 `0x889` 由
`FUN_0043C28E` 向主窗口投递 `WM_COMMAND 0xE140`，并将 `lParam`
设为 9；沙箱实测它选择“网络连接”页。选项窗口自身的“歌词搜索”页也有
同 ID 链接，但该路径由 `FUN_00497F63` 向现有属性表发送
`PSM_SETCURSEL(9)`，不得销毁并重建属性表。主窗口真实右键菜单的首项也是
`0xE140`；探针不仅
发送命令，还实际发送 `WM_CONTEXTMENU`，等待属于播放器线程的
`#32768` 菜单，再点击首项。

`FUN_004658A3` 在打开完整选项前先退出全屏，然后调用
`FUN_0045D531`。后者会销毁旧设置窗口、保存目标页到
`DAT_00547E70`、构造 `0xFD4` 字节的设置对象，并在窗口已经存在时
将新创建的外壳带到前面。`FUN_0049F4AD` 用 `CreatePropertySheetPageW` 创建完整
的 15 页集合；`FUN_004345B4` 是运行时选页路径。

## 原版动态基线

原版 Discovery 矩阵在 Windows Sandbox 用户
`WDAGUtilityAccount` 下完成：

- 报告：`rebuild/build/sandbox-settings/20260905-013813/settings-sandbox-report.json`
- 每个入口的控件树：同目录的 `settings-original-*.json`
- 每页截图：同目录的 `settings-original-*-page-00..14.png`
- 所有运行均正常关闭且没有强制终止；新版探针还会在打开前后用有界
  `WM_NULL` 记录 UI 线程响应性。

属性页外壳为 `#32770`，标题为“千千静听 - 选项”。通过外壳自己的
`PSM_SETCURSEL` 逐页选择得到以下固定顺序：

1. 关于
2. 常规
3. 播放
4. 快捷键
5. 视觉效果
6. 播放列表
7. 媒体库
8. 歌词秀
9. 歌词搜索
10. 网络连接
11. 音效插件
12. 音频设备
13. 皮肤
14. 全屏显示
15. 系统关联

外壳尺寸实测为约 `558x458` 像素。左侧页面导航是原版自绘
`SheetCtrl`，不暴露 UI Automation `TreeItem`；探针因此使用原程序
本身支持的属性页选择消息，而不是按猜测坐标点击。各页仍会同时保存
原生 HWND/UIA 控件名称、控件 ID、样式、可见性和矩形。

## 关闭与持久化语义

原版可见的底部按钮是“全部保存”`0x4D2`、“全部重置”`0x4D3` 和
“关闭”`IDOK(1)`，并没有传统属性表的可见“应用”按钮。
`FUN_0049FE39` 对 `IDOK(1)` 与 `IDCANCEL(2)` 走同一关闭分支：先保存
当前页索引、通知主窗口，再销毁窗口；它不恢复页面已经写入的设置。

沙箱 Persistence 报告
`rebuild/build/sandbox-settings/20260905-013331/settings-sandbox-report.json`
进一步验证了这点。探针在常规页切换“启动播放器后最小化”
（控件 ID `2088`）后分别发送 `1`、`2` 和属性表 Apply ID `0x3021`：

- `IDOK(1)` 与 `IDCANCEL(2)` 都结束当前设置外壳；
- `ID_APPLY_NOW(0x3021)` 应用后仍保持设置窗口打开；
- 设置在控件操作时立即写入进程内设置对象；
- 三条路径操作当下都不立即改写 `TTPlayer.xml`，播放器退出时才统一保存
  `General/@StartupMinimize=1`；
- Cancel 不提供事务回滚语义。

因此重建版若显示标准 OK/Cancel/Apply，可以把它们做成视觉兼容层，
但 Cancel 回滚会偏离原版。可观察行为应以“页面即时生效，关闭播放器时
统一序列化”为准。

## 历史重建版基线与自动化边界

实现前的 Release 基线位于
`rebuild/build/sandbox-settings/20260905-012806/settings-sandbox-report.json`：

- `0x8084` 只能打开现有单页视觉参数属性表；
- `0xE140`、`0x7918`、`0x7EFD`、`0x7FEF`、`0x8023` 和真实主窗口
  右键入口尚不能打开完整设置外壳；
- 现有视觉属性表只有索引 0 的 `PSM_SETCURSEL` 成功，其余索引返回 0，
  因而不能误判成 15 页外壳。

需要故意采集尚未完成实现的失败基线时，可给宿主 runner 加
`-AllowFailures`；默认情况下任一入口、页数、初始页、响应性或持久化断言
失败都会令命令返回失败，同时保留全部 JSON/PNG。

早期双版本矩阵中出现过“未响应”，根因是探针向另一个进程发送
`CB_GETLBTEXT` 时误用了探针进程自己的字符串缓冲区地址；目标进程无法访问
该指针，因而会阻塞在跨进程同步消息中。这不是播放器消息循环的行为。
当前探针对组合框只读取无需调用者缓冲区的有界计数/选择；确实需要结构体的
日期时间测试则先用 `VirtualAllocEx` 在目标进程分配 16 字节
`SYSTEMTIME`。每个场景同时写原子 checkpoint，并由无进展 watchdog 做精确
进程清理，失败诊断也限制长度，避免错误路径再次拖住整轮测试。

文件选择器、文件关联写注册表、网络代理探测和插件专属配置会产生额外系统
副作用。本探针只记录这些页面的结构、初始值及公共设置对象的持久化，不会
确认外部文件对话框或网络请求。预置颜色弹层已有独立专项；文件关联另由使用
随机 nonce HKCU 根的原生测试覆盖。网络在 Sandbox 配置中明确禁用。

## 宿主机最终验证

按当前要求，最后一轮没有启动 Windows Sandbox，而是直接在宿主机使用探针
创建的独立临时产品副本运行。报告内部的 `Probe` 和文件名仍保留 `Sandbox`
字样以兼容既有读取器，不表示实际执行环境。

- `rebuild/build/host-settings/20260905-170304-color-final-current/`：原版/重建版各 1 个
  `ColorPopup` 场景。两者均为 `ColorSelectCtrl`、样式 `0x94400100`、
  `156x171`、8 列 6 行；“自定义...”再进入系统颜色对话框。执行、行为和
  对比均通过，无强制结束。该专项验证弹层结构、几何、色格数量及自定义路由，
  不做调色板逐像素比较，也不比较系统颜色对话框内部；预置色弹层无模态且不自建
  嵌套消息循环，“自定义...”进入的 `ChooseColor` 仍为系统模态对话框。
- `rebuild/build/host-settings/20260905-165607-all-final-current/`：`Both / All` 最终
  矩阵，原版和重建版各 20 个场景、各 195 次页面访问。`Success`、
  `ExecutionSucceeded`、`BehaviorSucceeded`、`ParitySucceeded` 均为 true；
  failures、assertions、mismatches、errors、forced terminations 均为 0。报告未记录
  可执行文件哈希，不能把这一结果自动外推到之后重新生成的二进制。
- 390 次页面访问覆盖 15 页和 13 条探测入口：真实主窗口右键入口，以及 12 条
  命令/私有消息路由。该次探针对媒体库、独立桌面歌词等入口直接注入消息，
  所以这份历史矩阵验证的是路由接纳；现已恢复的媒体库源模式和桌面歌词窗口
  由后续实现与专项测试证明，不在这份矩阵的覆盖范围内。
- 音频设备页在双方各 13 次访问中均为 8 项、选择索引 4，且每次
  `Responsive=true`。这是应用 KS render alias、独占 overlapped 打开及
  sink/input/interface/medium pin 过滤修正后的可观察结果一致。重建版把 waveOut、
  DirectSound、KS、ASIO 的目录发现和当前已恢复的能力字段放在同位数 helper 中；
  页面首次创建仍同步等待，父进程等待期限为 4000 ms，故障终止后再给最多
  1000 ms 收口，并以 kill-on-close Job 保证不会无限等待。这不是异步刷新。
- ASIO helper 已恢复原版四行能力调用和格式：输出通道、固定 13 档
  `canSampleRate`、首输出通道有效位和 preferred buffer。为隔离旧驱动挂死，helper
  在快照发布前物化原本选择后懒计算的字段；实际播放由选定的 ASIO CLSID 打开，
  不会兼容回退到 waveOut，callback 生命周期、固定周期队列和 PCM 转换由独立
  mock 覆盖。waveOut
  `dwSupport == 0` 的三个支持详情已按原版保持空白，但最终矩阵没有直接捕获这些
  文本。最终报告走正常 helper 路径；本宿主机没有被接纳的 KS/ASIO 项，也没有
  覆盖 helper 缺失、超时、崩溃或损坏快照的故障注入分支。

## 运行命令

宿主机直接对比使用底层探针；它把原版和重建版复制到不同临时目录，并对消息、
窗口等待和退出设置期限：

```powershell
$out = Join-Path (Resolve-Path .) `
  ('rebuild\build\host-settings\' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-all')
New-Item -ItemType Directory -Path $out -Force | Out-Null
& .\rebuild\tools\windows_sandbox\probe_settings_window.ps1 `
  -SourceRoot (Resolve-Path .) -OutputDirectory $out `
  -Runtime Both -Suite All -WindowTimeoutSeconds 12
```

若需要重新执行历史 Sandbox 流程，可使用包装器：

```powershell
# 原版入口、15 页和真实右键菜单
.\rebuild\tools\windows_sandbox\run_settings_sandbox.ps1 `
  -Runtime Original -Suite Discovery -TimeoutSeconds 600

# 原版关闭/应用/持久化语义
.\rebuild\tools\windows_sandbox\run_settings_sandbox.ps1 `
  -Runtime Original -Suite Persistence -TimeoutSeconds 600

# Sandbox 原版/重建版双矩阵
.\rebuild\tools\windows_sandbox\run_settings_sandbox.ps1 `
  -Runtime Both -Suite All -TimeoutSeconds 900
```

两种路径的报告文件均为 `settings-sandbox-report.json`；Sandbox 包装器另写
`sandbox-settings-complete.json`。每个产品副本都有明确的窗口等待期限，正常
先发送 `WM_CLOSE`，4 秒未退出才强制终止，并在复制证据后删除精确匹配
`%TEMP%\TTPlayerSettings-<runtime>-<guid>` 的临时目录。
