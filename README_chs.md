# Thousand Tunes Player

<img width="713" height="569" alt="image" src="https://github.com/user-attachments/assets/a14ee585-7f05-4000-bac7-3a3bf81d7d79" />

该播放器基于 千千静听 5.7.9 的版本复刻，并在此基础上进行了多项优化和改进：

1. 扩展皮肤兼容性：支持 6.x 以上版本的 BMP/PNG 混合皮肤、逐像素透明和按钮动画，同时保留经典 BMP 皮肤的显示方式。
2. 新增任务栏播放控制：可直接通过 Windows 任务栏缩略图切换上一曲、播放／暂停和下一曲，无需还原播放器窗口。
3. 支持 Discord 音乐状态同步：显示正在播放的歌曲、歌手、专辑、播放进度和暂停状态，并可选择同步当前歌词。
4. 增强在线歌词服务：支持添加、编辑和排序多个自定义歌词服务器，增加 HTTPS、证书验证及代理支持。 [TTPlayerHttps](https://github.com/Sonic853/TTPlayerHttps)
5. 改善多显示器全屏体验：支持选择全屏显示的屏幕、在不同显示器之间切换，切换到其他应用时也能保持全屏显示。
6. 增强全屏歌词交互：新增独立的“允许拖拽歌词”选项，可在歌词文字或显示区域的空白处拖动定位播放进度，支持水平和垂直滚动模式。
7. 新增桌面歌词字号快捷调整：在提供对应按钮的皮肤中，可直接通过工具栏放大或缩小歌词字号，并保存调整结果。
8. 采用现代 Windows 文件选择窗口：改善添加音乐、选择文件夹和浏览歌词文件的操作体验，保留多选和文件类型筛选等功能。
9. 增强插件故障隔离：将文件信息与标签处理、DSP 扫描／配置、输出设备探测放入独立工作进程，加入超时和取消处理，降低这些操作中的插件异常对主界面的影响。
10. 独立保存重建版配置：使用 TTPlayerRebuild.xml 保存设置，首次运行时可导入原版配置而不覆盖原文件；歌词关联记录也独立保存，方便与原版并存。
11. 新增全屏下的 专辑封面 显示：在全屏模式下可选择显示专辑封面，该功能曾经在 8.x 版本中存在。
12. 普通版新增 [SMTC 系统媒体控件](docs/SMTC.md)：在 Windows 10／11 的系统媒体面板显示曲目信息、专辑封面和进度，支持播放、暂停、停止、上一首、下一首及进度跳转。
13. 增加对逐字歌词的支持 [#1](https://github.com/Sonic853/TTPlayer/issues/1)
14. 增加 Winamp 经典皮肤的支持 [TTPlayerWaskin](https://github.com/Sonic853/TTPlayerWaskin)
15. 增加 AAC 编码的额外拓展 [TTPlayerAAC](https://github.com/Sonic853/TTPlayerAAC)
16. 增加多语言支持 [TTPlayerI18n](https://github.com/Sonic853/TTPlayerI18n)
17. 新增 [GitHub / Gitee 软件更新](docs/RELEASE_UPDATE_IMPLEMENTATION.md)：默认使用 Gitee，自动检查默认关闭；支持独立更新器下载、校验、备份及替换程序。

## 构建

**当前 Release 为统一 x86 构建，支持 Windows XP SP3、Windows 7 至 Windows 11。**
Actions 输出 `TTPlayerRebuild-版本号.zip`，不再拆分现代版与旧系统版。兼容构建与功能范围见
[XP / Win7 兼容说明](docs/LEGACY_WINDOWS.md)。

Actions 会从 [TTPlayerHttps 最新正式版](https://github.com/Sonic853/TTPlayerHttps/releases/latest) 下载 HTTPS 组件，校验 ZIP、DLL 的 SHA-256 和 XP / Win7 导入兼容性，随包提供 `AddIn/ttp_https.dll`。手动安装时合并 `AddIn` 文件夹；自动更新器保留本机已有 HTTPS 组件。

需要安装 Visual Studio 2026 的“使用 C++ 的桌面开发”组件和 CMake 4.2+。当前项目必须使用 Win32/x86，不能选择 x64。

在项目中打开终端，执行以下命令进行构建：

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A Win32 -DBUILD_TESTING=OFF -DTTPLAYER_STAGE_RUNTIME=OFF
cmake --build build --config Release --target ttplayer_rebuild --parallel 4
```

生成文件位于：

```
build/Release/TTPlayerRebuild.exe
build/Release/TTPUpdater.exe
```

这组命令构建播放器与更新器，不构建测试、不复制原版运行资源。运行时，将两个 EXE 放到包含 ```ttpcomm.dll```、```ttpres.dll```、```AddIn```、```Skin``` 的原播放器目录。

更多详细信息请参考 [README](README.md)

## TODO List:

- [x] 鼠标在控件上的样式
- [ ] 音乐窗（非优先计划，目前需要先完善本地功能）
- [ ] 高 DPI 支持（目前受限于图片皮肤）
