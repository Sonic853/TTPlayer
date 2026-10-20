# TTPlayerRes 接入记录

资源工程位于相邻的 `ttpres`，采用原版 5.7.9 的 192 项资源基准与独立扩展区。完整说明见 [资源工程实施记录](../../ttpres/docs/RECONSTRUCTION_20261013.md)。

## 本次变更

1. 使用独立构建的资源 DLL，停止在播放器 CMake 中重链旧 `.res`。主资源采用原版应用程序目录基准，不采用 AddIn 下的修改版。
2. 引入 `third_party/ttpres-sdk`，集中管理 9 个重建版对话框、48 条已分配扩展字符串、原版资源清单和加载契约。按用户要求移除 EXE 备用资源：重建版必须使用完整扩展 DLL，旧 DLL 不再满足启动条件。
3. 资源选择接入 i18n/WTL，维持原 gettext context 与模板内存生命周期；原版和插件资源的加载方式不变。
4. “切换列表”和均衡器重设备用文字纳入资源区；动态菜单仍按原版路径准备、更新和执行。
5. 播放器 Action 默认下载 `TTPlayerRebuild/TTPlayerRes` 最新正式版本并审计；播放器 ZIP 根目录新增 `ttpres.dll` 和对应 SHA-256。
6. 独立资源构建完成后写入原版 EXE `004566E2 → 004BEBFF` 要求的头部校验。最终资源依旧无可执行代码，不需要额外运行库。
7. 启动时验证资源品牌、扩展 ABI 和所需对话框/文字；缺失、无法加载或不兼容时显示明确提示。i18n 读取与 WTL 普通/模态/属性页创建均拒绝缺失扩展，避免 `HMODULE=nullptr` 被 Win32 隐式解释为 EXE。
8. SDK 停止导出备用 RC；打包前检查 EXE 不含扩展模板/字符串，并检查 DLL 的模板结构及必需文字非空。Action 沿用这些构建检查，不运行测试。

## 验证范围

Windows 11、XP、Win7 均通过：原版 192 项逐字节对照，25 个菜单，49 个对话框及 677 个控件，9 个扩展对话框，播放列表菜单回归；原版 EXE 搭配原版/重建资源、重建版搭配重建资源的独立进程启动也全部通过。

移除回退后，三个系统重新验证了：缺少 DLL、原版 DLL、删除扩展对话框、清空必需字符串、错误 ABI 五类完整启动拒绝；卸载 DLL 后普通/模态/属性页创建拒绝；正常 DLL 的 9 个 WTL 对话框和 gettext 字符串、标题、按钮翻译。EXE 资源枚举确认不存在备用模板或文字，静态审计拒绝不完整资源 DLL。

初次接入时完成的独立源码干净构建、本地资源暂存目标及发行包离线下载校验仍保留证据。此次移除回退没有改变 DLL 内的 207 项资源或二进制哈希；修改了宿主、SDK 和审计工具，并重新打包。私有新增记录为 `tests/ttpres_audit_20261013/no-fallback-*.log`。

测试代码及日志只留在私有 `tests` 目录，不上传测试代码，不进入发行包，Action 不运行测试。界面截图工具授权超时，完整逐页截图未完成；没有将其计入通过项。

本次完成了本地独立构建及播放器打包；未推送远程仓库或执行 GitHub 托管 Action。需先在 TTPlayerRes 发布首个正式 Release，播放器 Action 才能取得新组件。

## 文件

- 资源包：`build/Release/ttpres-2026.10.13.zip`。
- 播放器包：`build/Release/TTPlayerRebuild-2026.10.13p1.zip`。
- 资源 DLL：`build/Release/ttpres.dll`，309,248 字节。
- DLL SHA-256：`f4db74b1f4095fa1d25146ec33265ec4b3ade12dff9c5e75680c44a3e9138df6`。

原版资源 `VERSION/1` 保留 5.7.9.0；资源组件的日期版本保存于 `TTPLAYER_RES_BUILD` 和发行包名中。原版启动读取的产品名、显示版本、菜单占位编号、皮肤内容及字体均保留。

## 2026-10-21：Action 缺少资源审计模块

### 原因与修复

`download_ttpres_component.py` 从 `third_party/ttpres-sdk/tools` 导入
`audit.py`，后者还依赖同目录的 `resource_model.py`。根 `.gitignore` 中不带
路径限定的 `tools` 规则忽略了所有同名目录，这两个文件虽然存在于本机，却未进入
Git 源码。Action 只检出仓库文件，因此在开始下载前便抛出
`ModuleNotFoundError: No module named 'audit'`。它不是需要通过 pip 安装的包，
也不是 Python、VC 运行库或下载凭据的问题。

现已为 SDK 目录下这两个文件增加精确的忽略例外，使它们可以随播放器源码提交；
其它本地工具和 `__pycache__` 仍保持忽略。两个文件分别与 `sdk.json` 中记录的
SHA-256、相邻 TTPlayerRes 源码完全一致，无需更改资源基准或下载脚本的导入路径。
提交本次修复时必须同时包含两个新增 Python 文件，仅提交 `.gitignore` 不足以修复 CI。

另外，资源 Release 返回 404 时，按用户要求仅显示
`TTPlayerRes: no releases found.` 并以失败状态退出，不显示异常堆栈或发布说明。
下载、哈希、192 项原版资源、扩展 ABI 和版本检查全部保留，不使用备用 DLL，
不跳过审计。

### 验证与待完成的远程条件

- 在只含 Git 已跟踪构建文件的临时目录中复现了同一个 `audit` 导入异常。
- 将可提交的新模块加入独立临时目录后，资源、TTPCOMM、HTTPS、歌词搜索四个下载脚本均可导入。
- SDK 全部文件通过既有哈希清单校验；仅包含两个应提交的工具模块。
- 本地资源 DLL 通过原版 192 项资源和扩展校验；破坏旧校验字段的 DLL 被拒绝。
- 使用现有 `ttpres-2026.10.13.zip` 与离线 Release 元数据，完整执行下载器入口、
  ZIP/DLL 哈希校验、资源审计、版本匹配和暂存，生成 DLL 与组件报告。
- 缺失 Release 的回归检查通过：仅显示简短提示，不生成输出或使用回退。
- 本地测试位于 `tests/ttpres_action_20261021/verify.py`，不上传测试代码，
  不添加 Action 测试步骤；本次未重编译播放器或重复虚拟机运行测试。

2026-10-21 的实际联网检查中，匿名 GitHub API 已达速率限额；使用已有登录凭据后，
确认 `TTPlayerRebuild/TTPlayerRes` 仓库可访问，但 Release 列表为空，
`releases/latest` 返回 404。因此本次未完成实际远程资源下载，也未运行托管 Action。
用户正在补上资源 DLL 发行包；本次仅保留缺少 Release 的简短提示，未发布或推送
任何仓库。
