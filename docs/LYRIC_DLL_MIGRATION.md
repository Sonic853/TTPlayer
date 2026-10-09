# 在线歌词迁移到 TTPlayerLrcsh

2026-10-09。

重建版全部在线歌词请求迁入独立 `ttp_lrcsh.dll`：查询规范化、协议 XML、Code、HTTP/HTTPS 会话、Cookie、重定向和下载。宿主保留 UI、设置、结果匹配、文件保存和插件回调状态管理；没有宿主网络回退路径。

旧网络实现从生产目标移出，保存在私有 `tests/lrcsh_rebuild/host_reference`，仅供历史协议对照。测试代码不上传、不进入 Action。

`PluginManager::CreateLyricSearch` 支持通过新 GUID 在 Initialize 之前配置服务器；不改变原版 24 字节网络结构和搜索 vtable。取消在后台线程请求插件退出，不等待 UI。缺失新 DLL 时显示更新组件提示。

Action 默认下载 [TTPlayerLrcsh 最新稳定版](https://github.com/TTPlayerRebuild/TTPlayerLrcsh/releases/latest)，校验 ZIP 与 DLL 摘要、导出、XP/Win7 导入，在主程序 ZIP 内加入 `AddIn/ttp_lrcsh.dll`，并记录 build-info。暂未发布稳定版时流程明确失败，需先发布插件。

本地 staging 使用 CMake `TTPLAYER_LRCSH_DLL`，默认 `../ttp_lrcsh/build/Release/ttp_lrcsh.dll`。首次配置前须独立构建插件；Action 使用 `TTPLAYER_STAGE_RUNTIME=OFF` 和发行下载。

详细兼容边界和验证记录在兄弟项目 `ttp_lrcsh/docs/RECONSTRUCTION.md`、`ttp_lrcsh/docs/VALIDATION.md`。NCAB 修正已迁移，实际 DLL 和捕获响应对比通过。XP 运行测试因 Guest Control 超时仍待完成。
