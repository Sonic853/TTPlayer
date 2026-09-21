# 在线歌词搜索恢复（5.7.9）

> 2026-09-21 补充：相关功能已在 XP SP3 / Win7 SP1 虚拟机中执行回归。覆盖项、修复和未覆盖边界见 [虚拟机验证记录](XP_WIN7_VM_VALIDATION.md)。下文保留原日期的历史结论。

2026-09-13。本次依据根目录 `TTPlayer.exe` 的伪代码及
`AddIn/ttp_lrcsh.dll` 的反汇编/反编译；没有用 6.1.2.0 的非皮肤代码作参照。

## 原版边界与接入点

| 原版函数/资源 | 原理 | 重建实现 |
| --- | --- | --- |
| `004CABC0`、`004CD3F9` | 枚举 AddIn 的歌词 creator，取得服务名称 | `PluginManager::LyricSearchProviders()` |
| `00497AE5`、`0043BA1E` | 设置页/搜索框从注册表填充服务器列表 | 删除固定四个名称，仅显示实际加载的 provider |
| `004CD44E`、`0043B9CE` | 创建搜索对象、初始化，失败时回退 creator 0 | `CreateLyricSearch`，后台调用及索引回退 |
| `0043ADCB`、`0043C2FF` | 提交歌手、歌名 | search slot 4 |
| `0043B332`、`0043B044` | 接收候选、按索引下载 | callback slot 3、search slot 5 |
| `0043B6D4`、`0043E3CF` | 收到文本，保存并应用歌词 | callback slot 4、ACP/UTF-8 保存、`LoadLyricsFrom` |
| `0044BD57`、`0044BEED` | 本地未找到后按设置联网；单结果直接下载 | 自动搜索、完整信息门槛、自动选择/结果窗口 |
| `0043AB9B` | 自动/手动分别用对话框 208/209 | 直接加载 `ttpres.dll` 的两个模板 |
| `0043BDCC`、`0043C1ED` | 多结果默认选择及 15 秒倒计时 | 默认行、倒计时、点击/键盘操作取消倒计时 |

搜索结果列名来自字符串 `0x8157`（以 `|` 分隔），提示使用 `0x8178..0x8186`，
不是在重建程序里重新写死中文。原代理设置入口进入选项页 9；2026-09-14
按用户要求将此处改为“编辑列表”，新增窗口的文案来自 EXE 自身资源。
`0044497F` 的在线匹配分支保留歌名优先、歌手加分、最高 9 分提前停止；
按 `00444450/004444BA` 做双向词边界匹配，并使用简繁/小写转换、
`0044434E` 的数字前缀/括号清理。系统 Unicode 字符分类与旧 CRT 的极端
字符分类仍可能不同，不能据此声称任意输入都二进制等价。

## 真实 DLL ABI

歌词 creator 类别：`{9D5AE963-7DF6-4323-B4CF-FBDA159BFF15}`。

搜索对象在 IUnknown 后：

- slot 3：`Initialize(callback, network_config)`。
- slot 4：`Search(artist, title)`，`ttp_lrcsh!60352282`，x86 stdcall。
- slot 5：`Download(result_index)`，`60352301`。
- slot 6：`GetExtra(title**, url**)`，返回内存由宿主 `CoTaskMemFree`。

初始化第二个参数此前误注释为 SoundLibrary host，实际是 24 字节代理结构：
`size/type/server/port/username/password`；见 `603520CD/60353428`。
type 0 直连、1 系统配置，其它带有效地址的值使用指定代理。

宿主 callback 支持 IUnknown 以及：

- `{CA540428-2528-4BBC-87B0-6605F21810C9}`：歌词回调。
- `{6F1F38A8-3021-4E2E-BBFB-7F616A5C3D62}`：初始化时查询的宿主标识。

callback slots 3..6 分别接收候选数组、下载文本、错误文本、服务器名称。
DLL 会在回调返回后释放候选字符串/下载文本，因此宿主必须立即复制。

## 原 DLL 的服务器列表来源

1. 加载 `AddIn/ttp_*.dll`。
2. DLL 的 `60351249` 读取**与 DLL 同目录、同文件名的 `.ini`**。
3. 文件实际是 UTF-8 XML；不存在或不合法时退回 DLL 字符串资源
   32000..32003（两个名称、两个 URL）。
4. 首次搜索调用 `60351420`，从首个服务地址的 `?svrlst&...` 获取更新列表。
5. `60351BE8` 更新列表并写回同名 `.ini`；最多接受两个 `server`。
   原插件刷新机制一次模块生命周期最多尝试一次；空响应还可能删除缓存。

示意配置（不自动写入用户目录）：

```xml
<ttp_lrcsvr>
  <server name="自建服务一" url="http://your-server/lyrics"/>
  <server name="自建服务二" url="http://your-other-server/lyrics"/>
</ttp_lrcsvr>
```

服务器必须实现原插件的协议，不能仅把普通歌词网站 URL 填进去：
`?sh?Artist=...&Title=...&Flags=...` 搜索，`?dl?Id=...&Code=...` 下载。
上述是原 DLL 的处理方式。2026-09-14 按新需求增加了同名 INI 编辑器及宿主
HTTP/HTTPS 协议客户端，解除 HTTP-only/两个服务器的限制；已知资源地址的服务
改走宿主客户端，不发送 `ci`、不允许 `svrlst` 覆盖用户编辑列表。
其它协议插件仍保留 DLL ABI。详见 [LYRIC_SERVICE_EDITOR.md](LYRIC_SERVICE_EDITOR.md)。

重建主配置仍为 `TTPlayerRebuild.xml`：`Lyric/AddInIndex` 和新增 `Lyric/ServerKey` 仅保存选择，
`Network/ServerList` 是 FreeDB/CDDB 列表，**不是歌词服务器列表**。
本次不覆盖现有 DLL、不替用户修改服务地址、不把服务器写进主配置。

## 线程、取消及保存

- `OnlineSearch` 保留独立 PluginManager/DLL 引用；Initialize、Search、Download、
  GetExtra 和 Release 都不在 UI 线程执行。回调不保留 PlayerWindow 或 HWND。
- 销毁/切歌只取消共享状态。晚到回调被丢弃，旧歌曲结果不会更新下一首歌词；
  新的本地歌词/编辑器优先于自动搜索。
- 初始化自身可能等待 5 秒，原 DLL 网络等待可达 60 秒，其析构还包含
  500 毫秒等待及内部终止逻辑。本次不把这些等待转移到 WM_CLOSE；后台增加
  搜索/下载的 65 秒状态超时，不在宿主新增 TerminateThread。
- 同一曲目的自动请求去重；手动搜索可重新发起。无插件时两个服务下拉框为空，
  手动搜索按钮禁用，不显示假服务器，也不伪造成功结果。
- 保存目录消费 `SaveToSoundFolder/DownLoadFolder`，相对目录以 EXE 路径解析。
  无下载目录且是本地歌曲时依原逻辑使用歌曲目录；URL 没有本地歌曲目录。
- 文件名消费 `SameFileTitle`；下载后关联消费 `AutoAssociate`，接入 `TTPlayerRebuild.rll` 持久关联表。
  防止服务器/输入文件名穿越路径、NTFS ADS、DOS 设备名；下载文本上限 2M 字符。
- 依 `0043E3CF` 优先无损 ACP，否则 UTF-8 BOM。先写同目录临时文件，再移动到
  目标；未授权覆盖不会截断原文件。手动覆盖询问复用 `0x814D`。

## 验证与边界

`lyric_search_tests` 在宿主机测试真实根目录 `ttp_lrcsh.dll`，复制到唯一临时目录，
预先配置两个 `127.0.0.1` 地址，**确认 DLL 确实读到测试名称后才发起搜索**。
测试不访问历史服务器，不改生产配置。

覆盖：外部 XML 名称、一次性列表刷新及缓存写回、Unicode 候选/文本、索引映射、连续下载不同候选、
无效索引、下载保存/防覆盖、代理结构实际调用、坏索引回退 0、错误回调、
慢响应下立即取消、主 PluginManager 先释放的生命周期、原版 208/209 控件、
设置服务列表、自动下载、单候选、多候选倒计时、缺失元数据门槛、切歌取消、无 DLL。

```powershell
cmake --build out/png-6120 --config Release --target lyric_search_tests
ctest --test-dir out/png-6120 -C Release -R '^lyric_search_tests$' --output-on-failure
```

这些结果验证**客户端调用链可运行**，不代表历史外网服务器现在仍可用。
为避免网络等待及可重入的模态循环，重建的 208/209 使用可关闭的 modeless 外壳，
不是逐字复制原版 `DialogBoxParamW` 的内部对象/消息循环。
2026-09-14 已补上手动关联窗口和跨会话关联表，详见
[LYRIC_ASSOCIATION_RECOVERY.md](LYRIC_ASSOCIATION_RECOVERY.md)。
2026-09-15 已补上上传浏览器表单，并按用户要求使用选中的搜索服务器 origin + `/dll/lrcup.php`；
详见 [LYRIC_MENU_RECOVERY.md](LYRIC_MENU_RECOVERY.md)。未验证服务器上传后台；原版广告/提示链接业务仍不在已恢复范围。
不能将本次结果描述为整个歌词网络系统已实现逐字或二进制等价。
