# WTL 10.01 接入与现有实现替换

## 范围

使用官方 WTL 10.01（2026-03-01）发布包。ATL 提供窗口和对话框基类，WTL 提供消息循环、控件、菜单、属性页及绘图资源封装。保持原版窗口类名、资源 ID、皮肤绘制和交互规则。

官方发布地址：<https://sourceforge.net/projects/wtl/files/WTL%2010/WTL%2010.01%20Release/>

## 可以替换哪些现有实现

| 现有实现 | ATL／WTL 组件 | 可替换内容 | 继续保留 |
| --- | --- | --- | --- |
| 应用消息循环 | CAppModule、CMessageLoop、CMessageFilter、CIdleHandler | 消息分发、过滤器与空闲处理 | 原版消息优先级及 WM_TIMER 空闲排除规则 |
| 窗口回调和对象绑定 | ATL CWindowImpl、消息映射 | HWND 绑定、窗口过程和销毁管理 | 每个窗口的业务处理、类名及样式 |
| 播放列表 | CListViewCtrl、ATL 窗口超类 | 原生控件操作、通知与默认消息处理 | 虚拟数据、选择同步、皮肤滚动条、滚轮路由 |
| 媒体库树 | CTreeViewCtrl | 节点操作、选择、展开和数据访问 | 扫描、分类、监控、缓存及查询 |
| 弹出菜单 | CCommandBarCtrlImpl、CMenuHandle | 弹出跟踪、访问键、焦点及消息分发 | 原版绘制、动态菜单及经典菜单边框 |
| 命令状态 | CUpdateUI、CDynamicUpdateUI | 启用、勾选与状态更新 | 播放业务规则及皮肤控件适配 |
| 设置和属性页 | ATL CDialogImpl，WTL CPropertySheetImpl、CPropertyPageImpl | 对话框和属性页生命周期、消息分发 | 资源模块、原版导航及提交时机 |
| 设置控件数据 | CWinDataExchange | 数据交换、范围验证 | 原版容错与提示语义 |
| 歌词编辑 | CRichEditCtrl、CToolBarCtrl、CFindReplaceDialog | 编辑器、工具栏及查找窗口的接口封装 | 时间标签、歌词搜索语义及快捷键 |
| 绘图资源 | CPaintDC、CClientDC、CBitmap、CFont、CBrush | 资源自动释放 | 所有权边界、选入 DC 的对象恢复 |
| 普通双缓冲 | CMemoryDC、CDoubleBufferImpl | 内存 DC、临时位图与回写 | 桌面歌词透明合成、长期绘图缓存 |
| 通用对话框 | CFileDialog、CFolderDialog、CFontDialog、CColorDialog | 文件、目录、字体、颜色选择 | 路径记忆、多选、筛选和 XP 回退 |
| 辅助控件与布局 | CHyperLink、CBitmapButton、CDialogResize | 链接、普通图像按钮、对话框布局 | 皮肤按钮、吸附和皮肤布局算法 |
| 上传页面容器 | ATL CAxWindow | ActiveX 宿主、控件创建和销毁 | 浏览器事件、导航目标校验和歌词表单填充 |

## 实施顺序

1. 固定依赖版本及校验值；接入消息循环、控件接口、GDI 资源封装。
2. 迁移对话框、属性页及独立窗口生命周期，保持原有消息处理函数的业务语义。
3. 接入命令状态和定制菜单，验证访问键、动态菜单、焦点及原版绘制。
4. 迁移 ActiveX 宿主和适合使用库的辅助布局。

每一步均先检查普通版，再检查 XP／Win7 版。完成情况及验证结果记录在本文末尾。

## 必须保持的行为

- 原版 004B54F2 排除 WM_TIMER；WTL 10.01 默认不排除，须覆盖 IsIdleMessage。
- 保持 PreTranslateMessage 的滚轮、对话框、编辑器快捷键及 tooltip 路由顺序。
- 多个播放器窗口各自绑定窗口对象，共享 PlayerWindow 状态。
- 菜单项数据只能有一个所有者，避免库与皮肤绘制同时替换 dwItemData。
- 属性页继续使用各自的 hInstance，不能把所有资源统一重定向到 EXE 或 ttpres.dll。
- CMemoryDC 的普通 BitBlt 不能代替桌面歌词的透明合成。
- CDwmThumbnail 不是任务栏专辑封面接口；保留现有 TaskbarPreview。

## 兼容及验证

WTL 基础头文件允许 Windows XP 目标，但最终兼容性仍取决于 ATL、CRT 和实际 API 导入。继续使用 VC-LTL、YY-Thunks、XP 子系统和导入检查。新系统对话框及 DWM 保留能力检测和回退。

测试代码只保存在本地 rebuild/tests，不发布；Actions 的 BUILD_TESTING 保持 OFF。

音频解码、播放模式、随机索引、媒体库数据层、元信息工作线程、歌词解析与皮肤引擎不属于本次基础库替换范围。

## 本轮实施记录

### 第一阶段：基础依赖、消息循环、控件和 GDI

- `cmake/wtl.cmake` 固定官方包及 SHA-256；普通版与兼容版共享头文件依赖。兼容版额外启用 ATL 的 XP 目标分支。
- `wtl_runtime` 管理 CAppModule，应用使用 CMessageLoop/CMessageFilter，覆盖 WM_TIMER 的 idle 规则。原有消息预处理顺序保持在单个过滤器中。
- 媒体库树使用 CTreeViewCtrl；设置标题的离屏 DC/位图使用 CDC/CBitmap 管理生命周期。

### 第二阶段：窗口及资源对话框

- 主窗口、播放列表、歌词、均衡器、可视化窗口分别绑定独立的 CWindowImpl 对象；原窗口类名及 GWLP_USERDATA 对外协议保留。
- ListCtrl 使用以 CListViewCtrl 为基础的 ATL 绑定，保留 SysListView32 原生默认过程和皮肤模型投影。
- 设置、注册、曲目属性页通过 CPropertySheetImpl/CPropertyPageImpl 管理；原始页面回调继续负责业务，适配层转换 SDK 回调的 DWLP_MSGRESULT 语义。
- 歌词搜索、歌词服务编辑、设置子页和转换进度窗口通过 CDialogImpl 管理。显式保留资源模块，避免改变 EXE/ttpres.dll 的资源来源。
- 模态属性页在调用返回后释放；非模态窗口在最终销毁后释放，处理嵌套 DestroyWindow。

### 第三阶段：命令状态及菜单

- `wtl_menu` 使用定制 CCommandBarCtrlImpl 管理实际弹出跟踪、键盘/CBT 钩子及焦点恢复。
- 绘制、尺寸、菜单提示、动态菜单和访问键消息交回原所有者窗口；关闭 WTL 默认图标数据转换，确保 MenuVisualItem 只有一个所有者。
- 统一启用/勾选接口使用 CDynamicUpdateUI，保留当前勾选、默认项等状态；大于 WORD 范围的动态命令保留 Win32 路径。
- 临时命令栏始终先取得返回命令，再转交原 HWND，避免命令栏销毁导致排队的 WM_COMMAND 丢失。嵌套弹出保留原生回退。

### 第四阶段：ActiveX、通用对话框及布局

- 上传窗口改用 ATL CAxWindow 承担 OLE 容器；应用继续处理事件、导航目标校验和歌词表单填充。
- XP 的文件/目录选择改用 CFileDialog/CFolderDialog；保留多选缓冲区、路径记忆及原有选项提示。新系统 IFileDialog 路径继续使用已有实现。
- 上传窗口继续按状态栏实际高度调整浏览器宿主。皮肤布局、固定设置页布局以及透明合成不套用通用布局/绘图默认行为。

### 后续可逐处使用的接口

上表是替换能力清单。尚未逐条改写的普通 RichEdit/Toolbar 消息、字体/颜色对话框、DDX、链接和普通按钮，可在修改对应功能时继续采用 WTL；本轮没有改变这些现有交互以适应库的默认值。命令状态仍按原有菜单更新时机计算，没有新增后台轮询或播放线程工作。

## 验证结果（2026-09-16）

- 普通 Release：7 项 UI 回归通过，覆盖原版菜单/列表行为、真实弹出菜单、消息过滤、属性页返回值、实际设置页逐页开关、换肤、桌面歌词、列表框选及启动。
- XP／Win7 Release：宿主机上的迁移、原版行为和启动检查共 3 项通过。
- 最终兼容 EXE：x86、子系统 5.01；18 个 DLL、637 项静态导入通过 XP 和 Windows 7 导出清单检查。
- 新测试位于本地 `tests/ui/wtl_migration_tests.cpp`。测试目录通过本地 Git exclude 排除；不覆盖用户已有的 `.gitignore` 编辑，Actions 继续显式使用 `BUILD_TESTING=OFF`。
- 原桌面歌词鼠标用例会把旧位置的排队 WM_MOUSEMOVE 当成进入按钮完成。已在本地用例中等待按钮客户区内的实际移动消息，再采集绘制基线；最终整组回归通过。
- 以上旧系统验证包含静态导入审计和当前 Windows 宿主运行，尚未替代 XP／Win7 实机检查。

## 播放列表右键期间白底黑字回归修复

原版 `00425DEC` 以 `SysListView32` 为列表基类，`00482BAF` 创建目录、歌曲两栏；
歌曲右键入口 `00488FEF` 按命中行及选择数量加载菜单，空白区域使用 `0x8B` 菜单。
重建版继续使用原生列表处理默认输入，由父窗口统一绘制皮肤。

回归原因是 `DefaultPlaylistListMessage` 调用原生右键处理时保持 `synchronizing` 标志。
原生处理尚未返回就进入弹出菜单的嵌套消息循环，此时 `PlaylistListWindowProc` 把重入的
`WM_PAINT`、`WM_ERASEBKGND` 也交给了系统列表，出现白底黑字。菜单自己的绘制消息和配色正常。

现将这两类皮肤绘制消息放在同步标志判断之前处理，避免菜单期间回到系统列表绘制。
同步保护仍用于原生状态投影，防止递归同步和错误的业务通知。

本地 `tests/ui/skin_rebind_tests.cpp` 新增真实右键按下/松开回归，在菜单未关闭时强制重绘并检查擦除结果。
覆盖两栏的空列表、已有曲目、多选、空白区域及键盘菜单，验证皮肤没有被原生控件覆盖、菜单配色正常和多选保留。
仅直接发送 `WM_CONTEXTMENU` 不会进入出错的原生右键调用栈，因此不足以检测本次问题。

本次修复验证：普通 Release 的 6 项相关回归通过；兼容 Release 在当前宿主上的 4 项回归通过，
包括上述真实右键用例。兼容 EXE 仍通过 18 个 DLL、637 项 XP／Win7 静态导入审计。
测试仅保存在本地 `tests/`，Actions 继续关闭测试。
