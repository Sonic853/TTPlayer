# 英文翻译、gettext 与 ttpres 修正（2026-10-28）

## 输入与结论

读取用户提供的 `en_US.zip` 中的 `英文翻译说明.txt` 和 `en_US/ttplayer.po`。说明署名为 [jerry25-hash](https://github.com/jerry25-hash)，提供了 1,365 条非空译文，参考千千静听英文版 v1.0.1。gettext 主 README 及发行包内 i18n README 已加入翻译鸣谢。“尽听精彩”保留提供者注明的原英文版译文 `set you on fire!`。

Issue 的编号判断正确：

| 资源 ID | 用途 | 修正 |
| --- | --- | --- |
| 32904 / 0x8088 | 示波显示命令的工具提示 | 原误用“将选中歌曲作为下一个播放对象”，改为“显示示波器效果” |
| 32587 / 0x7F4B | 将选中曲目设为下一首播放 | 保留原义 |

POT、简体、繁体和英文 PO 使用相同的正确上下文；英文为 `Display oscilloscope effect`。菜单命令 ID、顺序及执行逻辑不变。

## 播放列表底部三行不翻译的原因

原文说明中含有 `%(字段名)`，英文说明用 `%(field name)`。宿主将其当作实际标题格式字段，因名称不同拒绝整条译文，显示中文资源原文。

现在仅对静态说明 `ttpres/dialog/254/control/4294967295/4` 允许示例名称翻译。这条文字不传入格式化器。真实 `%(Title)` 等字段及 printf 参数仍须匹配；过滤模式、分隔符、换行及快捷键制表符继续检查。提取工具也不再把此说明误标为 `c-format`。路径说明的 `%USERPROFILE%` 作为完整环境变量校验，其后的标点可翻译。

## 文本覆盖与资源

- 本次扫描模板为 **1,885 条**，简体、繁体、英文均为 **1,885/1,885 非空译文**。补齐之前未纳入模板的相邻字符串、头文件常量和动态新增控件文本。
- 覆盖压缩包密码、分卷与错误提示、osu! 导入／导出及视觉选项、MusicBrainz、WASAPI 与音频处理、文件关联、后缀纠正、歌词编辑、更新器等界面。
- 提取支持 `i18n::Text`、`Literal`、`Plural`、`Mark`，扫描 `src` 和 `include`。`Mark` 只标记提取，必须在显示时翻译。
- 更新器初始化同一语言目录，增加 `updater/` 资源上下文；版本号、路径、曲名、用户文字及设备名称作为数据保留。
- **468 条代码／更新器消息**写入 ttpres 的扩展字符串区 48000–59999，附带 `TTPLAYER_RES_MESSAGES` 索引。维护工具保留已分配编号，独立构建不依赖相邻源码。
- ttpres 原版 JSON 基准不修改。构建在验证基准后实施 32904 的精确修正；审计反向还原该修正和既有 CompanyName 修正，其余 **190/192 项资源逐字节一致**。
- 审计同时核对新消息索引、ID 范围、唯一性和字符串表内容；原版菜单／对话框结构、DOS 校验和无代码资源 DLL 契约保留。

覆盖统计指当前模板和播放器维护的界面文本。用户文件内容、系统返回的错误／设备名称、外部插件自行创建的界面不是 PO 中的固定词条。资源中的原版功能说明保留其历史含义；没有借翻译修改功能行为。

## 英文显示修正

英文对话框采用 Tahoma，横向对话框单位放宽至 1.5 倍；动态创建控件使用相同映射。中文模板和动态控件继续采用原尺寸。

另外修正：

1. 选项导航栏加宽，完整显示 Media Library、Lyrics Display、Audio Device 等名称。
2. 英文标题栏使用 Tahoma；中文保留此前与 XP 对齐的 SimSun 14 像素绘制。
3. 音频处理模式标签及下拉框避开缓冲区控件，设备属性列按译文长度调整。
4. 播放列表的 When／下拉框／read file info 留出相应空间。
5. 桌面歌词的 Transparent 端点加宽、相应滑轨缩短；不改变数值和事件映射。
6. 校正部分可见术语及长度，如 Dither、Opaque、Unplayed、Font smoothing、fps。

## 验证

本机 Windows 11：

- gettext C++ PO／MO、资源上下文、复数及语言别名测试通过。
- 三种语言的模板覆盖、占位符、结构分隔符、过滤模式、重复同步、提取及 32904／32587 独立性检查通过。
- 宿主 i18n UI 测试通过，包含对话框、菜单、重复静态控件 ID、用户编辑内容保留和不兼容格式拒绝。
- 实际打开选项的 15 个主页面、4 个常规子标签以及视觉、媒体库、歌词、全屏子标签，共保存 33 张界面截图；对可见静态文字测量边界并检查截断。
- 更新器在隔离目录中验证英文标题、版本字段、来源、链接及按钮，随后关闭；未安装更新。
- 中文常规页、15 页标题字体、歌词／网络子页切换、鼠标和键盘操作回归通过。旧网络测试的固定宽度断言已更新为现有嵌套标签填充宽度，资源高度与输入行为仍检查。
- ttpres 基准与扩展审计通过；故意修改 32587 或消息字符串表均被审计拒绝。
- EXE、更新器及 gettext DLL 的 XP／Win7 静态导入审计通过。本次没有重新执行 XP／Win7 虚拟机运行测试。

测试代码、截图和日志保存在私有 `rebuild/tests/i18n_20261028` 与 `rebuild/tests/ui`。测试使用独立设置目录，不更改用户保存的语言或播放列表。

## 维护与发布

```powershell
python gettext/tools/extract_catalog.py --ttpres ttpres/build/Release/ttpres.dll --updater rebuild/build/Release/TTPUpdater.exe
python gettext/tools/sync_catalogs.py
# 补译新增条目后检查
python gettext/tools/sync_catalogs.py --check
python gettext/tools/export_resource_messages.py
python ttpres/tools/export_sdk.py rebuild/third_party/ttpres-sdk
```

随后分别构建 gettext、ttpres 和播放器。部署时同步 EXE、TTPUpdater.exe、ttpres.dll、AddIn/ttp_i18n.dll 及 i18n 目录。若另有旧 MO，应重新生成或移除，避免其优先级覆盖新 PO。两种播放器文件名 TTPlayerRebuild.exe／TTPlayer.exe 均保留。
