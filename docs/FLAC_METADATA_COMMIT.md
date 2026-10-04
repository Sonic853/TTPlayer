# FLAC 可选提交接口与文件属性保存

日期：2026-10-04。播放器与 ttp_flac 独立构建，当前版本均为 `2026.10.04`。

## 问题与修复

原 FLAC 的 Set 只修改内存，真正写盘发生在最后一次释放 Reader。旧接口无法返回保存的 HRESULT。重建版此前会重新读取核验，但不能准确区分磁盘已满、权限问题等提交错误。

现在新增独立 QI 接口 `{3AB643C1-D8A4-4D49-9CBB-EF196B04B6E7}`，IUnknown 后槽 3 为 `HRESULT __stdcall Commit()`。原 Reader、Metadata、Thumbnail 和工厂布局保持兼容；原版继续最终 Release 保存。

| 位置 | 改动 |
| --- | --- |
| `include/ttplayer/plugins/plugin_manager.h`、`src/plugins/plugin_manager.cpp` | CommitMetadata 查询可选接口，保护跨 DLL 调用，缓存第一次结果，结束支持该接口的会话 |
| `src/app/file_info_worker.cpp` | 设置标签／封面后提交，再释放并重读，保留真实提交错误 |
| `src/ui/file_info_write_verification.h` | 格式化系统错误描述及十六进制 HRESULT |
| `src/ui/player_window_playlist_properties.cpp` | 显示真实错误，保留失败编辑，阻止失败后自动切换曲目 |
| `src/audio/replay_gain_scanner.cpp` | 增益写入失败时返回提交错误，保留已算出的分析数据 |
| `src/ui/player_window_playlist.cpp` | 删除增益字段的结果包含提交结果 |
| `src/ui/player_window_lyrics.cpp` | 独立 Reader 写入歌词后检查提交结果 |

不支持新接口的插件返回 S_FALSE，继续释放后重新读取验证；TTA 仍由宿主通用标签处理。

Commit 是终结操作。成功或失败后均不能继续用此 Reader 播放、定位或修改；重复 Commit 返回原结果，Release 不会再次尝试。重试需要新 Reader 和保留的编辑草稿。

正在播放的共用 Reader 的内嵌歌词写入继续旧生命周期。新接口只用于可以关闭的独立写入会话。完整协议位于相邻 ttp_flac 项目的 `docs/COMMIT_INTERFACE.md`；播放器不引入插件源码或构建依赖。

## 验证与交付

- 最终插件 208 条本机检查通过，XP／Win7 回传文件和发行包的 64 项证据核对通过。
- XP SP3、Win7 SP1、Windows 11：原版与重建版实际加载新 DLL 并播放 FLAC／TTA；原版文件属性修改标题成功落盘。
- 三个系统的真实文件属性页、写入子进程和 PluginManager：新接口成功、旧插件回退、模拟磁盘已满三组用例通过。磁盘已满返回 `0x80070070`，未保存编辑保留。
- Windows 11 按用户要求代测 Windows 10，不计为独立 Win10 实机验证。
- 播放器和更新器 Release 构建、XP／Win7 导入检查通过。测试及注入器仅在 rebuild/tests，未上传，Action 不运行测试。

插件包 `ttp_flac-2026.10.04.zip` 仅含 AddIn/ttp_flac.dll 和 SHA256SUMS.txt；播放器包独立输出为 `build/Release/TTPlayerRebuild-2026.10.04.zip`。
