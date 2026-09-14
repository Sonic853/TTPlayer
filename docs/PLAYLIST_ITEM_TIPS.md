# 播放列表曲目悬停提示恢复

## 原版证据与故障原因

- `00482BAF` 创建 Files 曲目控件（ID `0x2802`），扩展样式 `0x4420`
  包含 `LVS_EX_INFOTIP`。曲目提示属于这个控件，不是播放器按钮的公共提示。
- `004887AA` 响应 `LVN_GETINFOTIPW`：`DAT_00547C94`（XML `ItemTips`）
  为零时返回空文本；否则按悬停曲目索引取信息。`ReadInfoMode=2` 不触发读取。
  本地与网络模板分别来自 `ttpres.dll` 的 `0x81C9`、`0x8296`，网络回退文本
  使用 `0x8297`，不存在的普通本地文件附加 `0x828E` 警告。
- `004AEB8F` 格式化 `Filename`、`Duration`、`Format` 三个内建字段，其他
  占位符查元数据；字段为空时省略相应模板行。列表缓存的显示名不等于 Title 标签。
- 宿主机运行原版确认：Files 提示最大文本宽度 400 像素，当前系统初始延时
  500 ms，显示时 style/ex-style 为 `94000102` / `000800A8`。
  延时采用公共控件系统默认值，不在程序中硬编码 500 ms。

修复前重建版将每首歌的提示矩形注册在 `TTPlayer_PlayListWnd` 上，但实际
排队的鼠标消息来自 `Files` 子 HWND，并使用子控件客户区坐标。
`PreTranslateMessage` 正确保留了原始 MSG，却无法命中父窗口上的提示区域。
此外，Files 子控件没有向父窗口转交提示 `WM_NOTIFY`。

## 修复

1. 创建 Files 专属的 `playlist_item_tooltip_`，使用原版运行时对应的
   `TTS_NOPREFIX | TTS_USEVISUALSTYLE`、400 像素最大宽度。
2. 提示工具注册在 Files HWND 上，将行矩形转换为 Files 客户区坐标。
   原始 MSG 分别转发给按钮提示与 Files 提示，各自只命中自己的工具；
   不重复转发合成的子控件到父窗口的鼠标消息。
3. Files 转发 `WM_NOTIFY`；通知按来源 tooltip 和工具 ID 解析回曲目。
   保留异步、有界的单曲目信息读取，不在悬停回调里阻塞调用旧插件。
4. 滚动、改变分隔条、列表刷新和窗口大小变化时更新命中区域；销毁或切换皮肤
   时注销旧工具并销毁专属提示。`ItemTips=0` 不注册曲目工具。
5. 仍从 `ttpres.dll` 读取提示模板。删除 Title/Artist/Album 的显示字段回退，
   避免给无标签歌曲虚构信息；内建 PCM WAV 的短格式名改为原版的 `PCM`。

这是在重建版自绘 ListCtrl 上恢复可观察行为，不表示已将该控件替换为原版
私有类的逐字二进制实现，也不涉及本次范围以外的行高、列表布局恢复。

## 宿主机验证

`tools/probe_playlist_item_tips.ps1` 在独立运行目录生成静音 WAV 与 TTBL，
可复制一首真实 FLAC 作元数据验证。它使用真实鼠标移动、读取可见提示文字，
而非直接调用字符串函数或注入提示回调。行位置通过目标进程的
`LVM_GETITEMRECT` 取得，不假设原版与重建版具有相同行高。

覆盖默认皮肤、LX-iPlay、TT2012 的首行/换行悬停、带标签 FLAC、滚动、
移出、`ItemTips=0` 和正常退出。报告保留在 `build/host-item-tips-*` 中。
测试关闭了播放、Discord 通知和歌词下载，原配置、播放列表和音频样本不被覆盖。

示例（在项目根目录运行，PowerShell）：

```powershell
.\rebuild\tools\probe_playlist_item_tips.ps1 `
  -Runtime .\rebuild\build\Release `
  -OutputDirectory .\rebuild\build\host-item-tips-rebuild-final `
  -Skins @('<Default_Skin>', 'LX-iPlay.skn', 'TT2012.skn') `
  -TaggedSample 'C:\Users\Sonic853\Music\test\陈慧娴 - 千千阙歌.flac' `
  -ExpectedTags @('千千阙歌', '陈慧娴')
```

原版对照使用 `-Runtime . -ExecutableName TTPlayer.exe`，并指定独立输出目录。
无标签 WAV 应只包含格式、长度、文件名；上述 FLAC 应包含标题、艺术家、专辑、
格式、长度、文件名六行。测试目录路径不同导致的文字换行不作为样式差异。

2026-09-07 验证结果：Release 编译成功，CTest 21/21 通过。
`host-item-tips-before/report.json` 记录修复前 30 行已加载却无提示的失败；
`host-item-tips-original-final/report.json` 与
`host-item-tips-rebuild-final/report.json` 的三皮肤测试均通过且正常退出。
首行、第二行和 FLAC 共 9 组文本在仅替换测试目录前缀后逐字一致，窗口
style/ex-style、400 像素最大宽度与系统默认延时也一致。
`host-item-tips-disabled/report.json` 的三皮肤关闭提示检查通过；
`host-item-tips-blank-original/report.json` 与
`host-item-tips-blank-rebuild/report.json` 的三曲目短列表空白区检查均无残留提示。
