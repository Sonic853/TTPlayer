# OSZ 解析迁移至独立插件

日期：2026-10-25。独立仓库：<https://github.com/TTPlayerRebuild/TTPlayerOSU>。

## 1. 本次变更

`ttp_osu.dll` 的输入是 **`.osz` 压缩包路径**。插件发起包内枚举，读取包内元数据，返回所有被引用的独立音频及逐曲目背景。`.osu` 仅作为 OSZ 内部的引用信息使用，没有单独的 `.osu` 导入或解析 API。

- 删除播放器 `src/audio/osz_archive.cpp` 内置解析实现，改用 `osu_plugin_client.cpp` 调用独立 DLL。
- 将谱面解析、路径边界、首次引用排序、去重、多音频和逐音频背景规则迁入插件。
- 通用 ZIP 读取、CRC／密码校验、缓存与取消仍由现有归档层提供；音频复用已有解码器。
- 插件查询后按同步回调复制字符串／图片，无 STL 对象或内存释放权跨 DLL 边界。
- 保留封面顺序：内嵌封面 → 对应背景 → 无封面。OSZ 不套用普通 ZIP 的 `cover.*` 猜测。
- 包内标签、封面等保持只读；`.osz`／`.OSZ` 不建议改为 `.zip`。
- 普通 ZIP／RAR 和包内 CUE 保持原有入口与处理。
- 将 `ttp_osu.dll` 排除在旧音频插件枚举外，避免被列为“缺少 ttpGetSoundAddIn”的失败插件。

## 2. 部署与失败行为

播放器从 EXE 同目录的 `AddIn/ttp_osu.dll` 加载并调用 `ttp_osu_query`（序号 1），验证 ABI 1 和完整函数表。缺失、错误架构或不兼容的插件会让 OSZ 导入给出明确错误，不使用内置解析副本，也不把击打音效当成歌曲回退导入。

播放器仍保留后缀分派、打开文件筛选器、归档成员身份与后缀保护，因此插件缺失不会使 OSZ 被误提示改名为 ZIP。普通 ZIP／RAR 不依赖此插件。

原版 5.7.9 播放器没有 OSU ABI 1 调用入口。该 DLL 属于重建版的归档扩展，单独放入原版播放器无法增加 OSZ 功能，不声称兼容旧 SoundReader 接口。

## 3. 构建和 Action

独立项目位于 `../ttp_osu`，使用现代 MSVC Win32、C++20、VC-LTL 5.3.1、YY-Thunks 1.2.2，以体积优化构建通用 DLL。用户无需另装 VC 运行库；同一 DLL 用于 XP SP3、Win7 和现代系统。

独立手动 Action 生成日期版本，`Release a Version` 发布 `ttp_osu-版本号.zip`。二进制 ZIP 只有 `AddIn/ttp_osu.dll` 与 `SHA256SUMS.txt`，许可证留在独立仓库。

播放器 Action 调用 `cmake/download_osu_component.py`，从 **TTPlayerRebuild/TTPlayerOSU** 最新稳定 Release 获取组件，验证：

1. 资产 URL、大小和 GitHub SHA256 摘要。
2. ZIP 严格成员集合、CRC 和内部 SHA256。
3. x86 PE32、XP 子系统、XP／Win7 静态导入清单。
4. `ttp_osu_query` 导出序号／名称、无转发导出，以及 DLL 版本和 Release 标签相同。
5. 播放器运行时再次协商实际 ABI。

本地 `TTPLAYER_STAGE_RUNTIME=ON` 接受 `TTPLAYER_OSU_DLL` 指向独立构建结果，并执行相同 PE／版本审计。CI 使用 `STAGE_RUNTIME=OFF`，在播放器编译后下载组件。打包器要求有效的 `osu-component.json`，并将 DLL 哈希写入通用发行包的校验清单。

本轮访问 GitHub 最新 Release API 返回 HTTP 404，尚不能确认有可下载的稳定发行版。已完成本地独立构建、安装布局和打包链验证；线上 Action 首次打包前需先发布插件。本轮未上传源码、未创建线上 Release。

## 4. 验证范围

私有测试在 `tests/archive_support/osu_plugin_20261025/`，复用 `osz_20261016/` 的真实样本与合成包。不上传测试代码或用户样本，不在 Action 中运行测试。

覆盖：

- 输入为 OSZ 路径，插件自主枚举；单独 `.osu`／普通 ZIP 参数被此接口拒绝。
- 两个真实样本分别导入 1 首、3 首；每首音频解码、定位、独立文件属性、列表信息进程与背景匹配。
- 内嵌 APIC 优先，同包部分音频使用背景，Unicode／相对路径／别名、多难度、缺失文件、无背景和无效背景。
- 限制触发、CRC／短读／取消／内存不足回调错误，不提交成功的部分列表。
- 并发调用保持独立状态；缺失及不兼容 DLL、普通 ZIP 不受影响。
- 包内资源只读、用户原始样本哈希不变；ZIP／RAR CUE 回归。
- 已有后台封面读取按 `MetadataRevision` 完成后核对结果，避免将尚未完成的异步读取当作错误。

| 环境 | OSZ 主流程／ABI | DLL 缺失 | DLL 不兼容 |
| --- | --- | --- | --- |
| Windows 11 | 315 项通过 | 7 项通过 | 7 项通过 |
| XP SP3 虚拟机 | 315 项通过 | 7 项通过 | 7 项通过 |
| Win7 SP1 虚拟机 | 315 项通过 | 7 项通过 | 7 项通过 |

另有 11 项独立打包校验通过，覆盖损坏 ZIP、错误摘要、额外／重复成员、错误文件版本和导出。两份用户原始 OSZ 的 SHA256 未变。Windows 10 未单独实测，由 Windows 11 验证现代系统路径；未做独立听音或任务栏预览截图测试。

日志为私有目录下 `win11.log`／`xp.log`／`win7.log`、`missing-*.log`／`incompatible-*.log`；产物记录为 `artifacts.json`。播放器、更新器、OSZ DLL 的 XP／Win7 静态导入审计通过，资源审计为原版基准 192/192 一致、15 个扩展。

## 5. 本地输出

播放器及独立插件包统一存放于 `build/Release`；插件工程自己的构建产物保留在 `../ttp_osu/build/Release`。

| 文件 | 字节数 | SHA256 |
| --- | ---: | --- |
| `TTPlayerRebuild-2026.10.25.zip` | 3,412,744 | `09a80fdd42e229f246fb74cbdd2964719138eefa13cc018444efbaf4592c6f0b` |
| `ttp_osu-2026.10.25.zip` | 31,688 | `3bbc64b4ecfad4d2b46f661c27d166941bba0fbfa6083147d7102f67c2a3122e` |
| `AddIn/ttp_osu.dll` | 62,464 | `a5fbaed36d9fcc023ac9cc471baf7acbd49050619a367e3657e871d935fe1f92` |

通用包包含播放器、更新器、TTPCOMM、资源 DLL、HTTPS／歌词／OSZ 插件和校验清单。独立插件包只有 DLL 与校验清单。已逐文件核对内部 SHA256 与实际 Release 文件，并更新输出目录的校验清单；没有测试、样本或许可证文本进入二进制包。
