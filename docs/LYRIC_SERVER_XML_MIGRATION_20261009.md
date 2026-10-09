# 歌词服务器配置迁移到 XML

日期：2026-10-09；本地构建 `2026.10.09p3`。

后续 `2026.10.09p4` 已解除 XML 模式下原版播放器的两项限制，支持 1～128 项，INI 的两项及刷新逻辑保留。重建宿主同时修正单工厂 XML 的资源默认项索引，详见 [多服务器实现与测试](../../ttp_lrcsh/docs/MULTI_SERVER_ORIGINAL_HOST_20261009.md)。下文保留 p3 迁移记录。

## 用户可见行为

- 重建播放器的服务器编辑器读取、保存 `AddIn/ttp_lrcsh.xml`。
- 只有 XML 不存在且 INI 存在时，才完整复制 `ttp_lrcsh.ini` 到 XML；原 INI 保留。
- XML 已存在时始终优先使用 XML，不会用 INI 覆盖它。XML 无效则报告错误并保护文件。
- 后续增删、修改、排序写入 XML，覆盖保存时保留 `.xml.bak`；已有服务器选择不因文件名改变而重置。
- 原版播放器搭配本次重建歌词 DLL 时同样优先读 XML，并保持 XML 列表、不请求服务器目录刷新。只有无 XML 时才继续走 INI 的旧刷新路径。未经替换的原 DLL 不能读取 XML。

原版的两个服务器工厂限制保持不变；XML 的第三个及以后条目完整保留，供重建播放器使用。原版播放器修改配置后需重启，以重新加载 DLL 缓存。

## 改动位置

| 文件 | 改动 |
|---|---|
| `include/ttplayer/lyrics/service_catalog.h` | 配置路径字段及接口改为 XML |
| `src/lyrics/service_catalog.cpp` | INI 首次复制、XML 读写、冲突及备份保护；保留旧服务器选择标识 |
| `src/ui/player_window_lyric_services.cpp` | 新增服务器条目的保存位置改为 XML |
| `../ttp_lrcsh/src/catalog.cpp` | 原宿主 XML 优先，XML 模式不自动刷新目录 |

迁移在同一目录用临时副本完成，再以不覆盖已有目标的方式重命名；复制失败不会截断 XML 或修改 INI。INI 的 BOM、换行、注释、未知属性及全部服务器条目在复制时保持原样。

## 验证与发行

Windows 11 两套重建宿主测试通过，覆盖迁移、保存及真实 DLL 搜索下载；插件本机 8 组 XML/INI 场景、22 场景协议回归和 8 项发行校验通过。XP、Win7 各 5 组旧 ABI 目录测试通过。Win10 按约定由 Win11 代验；本轮未重跑原版 GUI 全流程。

测试源码、样本和日志仅位于 `rebuild/tests`。Actions 不增加测试步骤，也不向发行包放入用户服务器配置。

产物在 `build/Release`：`TTPlayerRebuild.exe`、`AddIn/ttp_lrcsh.dll`、`TTPlayerRebuild-2026.10.09p3.zip`、`ttp_lrcsh-2026.10.09p3.zip`。摘要见同目录 `XML-Catalog-2026.10.09p3-SHA256SUMS.txt`。本次为本地构建，未远程发布。

原版伪代码入口、边界规则和测试明细见 [插件 XML 目录兼容说明](../../ttp_lrcsh/docs/SERVER_CATALOG_XML_20261009.md)。
