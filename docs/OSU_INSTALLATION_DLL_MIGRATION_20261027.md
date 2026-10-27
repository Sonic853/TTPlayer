# osu! 安装信息迁入 ttp_osu.dll

已将 exe 身份识别、默认安装位置发现、stable 歌曲目录以及 lazer 便携/storage.ini 解析集中到可选 DLL。播放器保留设置 UI、XML 保存、菜单接入和识别结果缓存。

## 源码变化

- 删除宿主 `src/settings/osu_executable_identity.h`、`src/settings/osu_storage_path.h`。
- 引入 `third_party/ttp-osu-sdk/include/ttp_osu/installation.h`：独立 ABI 1、导出序号 4。
- `src/audio/osu_plugin_client.cpp` 通过同一可选插件加载器查询接口、复制同步回调结果；不增加静态 DLL 依赖或宿主解析回退。
- `settings.cpp` 只读取和保存配置；播放器初始化/选项刷新时才通过 DLL 补齐 XML 未保存的路径，已保存的值保持不变。更新器和后台工作进程读取配置不会加载或占用可选 DLL。
- `OsuLibrarySettings::explicit_paths` 区分尚无可用默认值和用户明确清空。无插件保存时不写空默认值；已有显式空值仍可保存、重开。
- 可用条件继续为“任一配置 exe 识别成功＋DLL 对应视觉能力”，只有全屏可用；缺新接口时设置页提示更新 DLL。
- 下载脚本及 ExtraAddIn 包校验支持新增的固定序号 4，旧三个导出与各自 ABI 保持不变。

## 兼容范围

| 组合 | 结果 |
| --- | --- |
| 新播放器＋新 DLL | 完整识别、路径发现、两种星环、OSZ/歌曲库 |
| 旧播放器＋新 DLL | 原有接口继续工作 |
| 新播放器＋旧 DLL | 保留 OSZ/歌曲库；缺少识别接口时隐藏星环并提示更新 |
| 没有 DLL | 可选功能隐藏，保留用户已有路径与配置 |

两套 lazer 解析已经合并：UTF-8/BOM、UTF-16 LE/BE、最后一个 `FullPath`、64 KiB 上限、离线绝对路径规则统一；便携模式缺失/损坏配置时回到程序目录。默认用户目录由 Windows 提供，兼容 XP 的目录布局。

## 验证

- 36 项 DLL exe 识别检查通过。
- Windows 11：59 项 DLL ABI、路径和存储检查；stable 和 Realm 样本读取通过。
- XP / Win7：各 58 项 DLL ABI 和路径检查；识别 x64 lazer 成功。
- 三个系统的媒体库三页、动态视觉入口、XML 和便携目录 UI 回归通过。
- 新宿主＋旧 DLL、无 DLL、新 DLL＋旧宿主的隔离测试通过。
- 配置构造/读取/保存不加载 `ttp_osu.dll` 的隔离测试通过，避免更新器占用待替换插件。
- Release 构建与 XP/Win7 静态导入检查通过。

私有证据保存在 `tests/osu_installation_20261027`、`tests/osu_identity_20261027`，不打包。测试所启动的 XP / Win7 虚拟机在结束后恢复关闭状态。

## 使用输出

`build/Release/TTPlayerRebuild-2026.10.27.zip` 是播放器包；`build/Release/ttp_osu-2026.10.27.zip` 是独立可选插件包。关闭播放器后更新两者。主程序包仍不强制携带插件，Action 的插件包需发布新的 TTPlayerOSU Release 才能获取新接口。
