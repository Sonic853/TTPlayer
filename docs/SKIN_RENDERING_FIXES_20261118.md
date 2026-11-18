# 皮肤主要绘制差异修正：BMP / PNG

依据：[原版 5.7.9 全链路对照](SKIN_RENDERING_FULL_CHAIN_COMPARISON_20261118.md)
及其 REA 伪代码、汇编和原版运行证据。

## 修复范围

| 对照项 | 修正 |
|---|---|
| R01 | LED 使用可增长分钟栏 `MMM:SS`，不在一小时后自动引入小时栏；保留真字形高度、左右锚点、旧/新范围清理和统一显示快照。 |
| R02、S01 | 主/迷你 progress、volume 的 bar 从客户区原点复制，限制源边界，不拉伸/居中；BMP 不应用 fill 的色键。预览共用同一 helper。 |
| R03 | disabled progress 使用第四帧，禁用状态不进入 PNG hover/pulse 绘制。 |
| R04 | BMP 按钮状态局部绘制先保留目标 DC 旧像素；新热帧的色键孔洞不再被统一背景重建抹掉。完整重画仍恢复背景。 |
| R05 | 没有有效保存矩形时，可伸缩列表初始尺寸取背景原尺寸与 XML 初始尺寸的逐轴最大值。 |
| R06、R07 | 两栏 selected 和图片 splitter bar 改为固定 1px 边缘、拉伸内部；保留极小/异常尺寸的防御边界。 |
| R08 | splitter 箭头单帧居中、限制复制宽度；不将完整帧压缩到窄分隔器。 |
| R09 | 歌词、EQ、列表 title 的 BMP 使用不透明复制，PNG 继续 source-over；歌词按钮底图重建也一致。 |
| R10 | 固定原尺寸 8 位背景按首个匹配调色板索引生成 region；可伸缩背景仍先合成真彩色缓冲再按 RGB。 |
| R11 | 合法 BMP NULL-region 是矩形回退，可清除旧异形轮廓；资源失败不被当作合法 NULL。主/mini/歌词/EQ/列表统一接入。 |
| R12、R13 | 结束标签仅在匹配时忽略 ASCII 大小写；无引号属性保留实体字面值，带引号值按既有规则解码。 |
| S02、S03 | scrollbar 上、下 page 各自从源 `(0,0)` 平铺；splitter 的 bar/arrow 成组有效，否则回退默认宽度和绘制。 |

## 对 PNG 的判断

**共享控件语义，不共享旧 BMP 像素副作用。**

可以直接用于 PNG 的是：

- LED 时间格式与实际布局范围；
- 四态帧选择、禁用状态和动画分支；
- bar 的原点/裁剪、selected/splitter 的九宫格、箭头居中/限宽；
- 列表初始尺寸、配套素材回退、scrollbar page 的纹理起点。

不能照搬给 PNG 的是：

- `#ff00ff` 透明色键：PNG 的不透明洋红必须仍然可见；
- BMP 热帧新透明洞的旧屏幕像素留存：PNG 必须重建干净底图，否则半透明像素会越画越深；
- BMP NULL-region 矩形回退：alpha-empty PNG 保持非 NULL 空覆盖；顶层窗原有空形状预检策略保留。

继续使用内容识别和 `IsGdiPlus()`，不根据 `.bmp`/`.png` 后缀猜绘制格式。
PNG 保留 NearestNeighbor/Half、source-over、逐像素及整体 alpha、hover/pulse/flash。
与 PNG 相交的 BMP 局部更新仍提升为底图重组，不用旧屏幕像素给上层 PNG 充当 backing。

scrollbar 的 alpha 底图另作保留：先排除两个 page，仅画按钮/thumb 下的原 rail；
两个 page 再各绘制一次。这样更正了纹理相位，但没有让 PNG 控件的透明洞突然露出父底图，
也没有在同一区域重复 source-over。

## Region 完成状态

`SkinImage::CreateRegion(..., bool* completed)` 与
`LegacySkin::CreateWindowRegion(..., bool* completed)` 区分：

- `completed=true, region=NULL`：合法 BMP 矩形回退；
- `completed=false, region=NULL`：读取、分配、GDI 等失败，保留原窗口形状；
- `completed=true, region!=NULL, NULLREGION`：真正的 PNG 空覆盖，不是矩形。

只缓存完成的扫描结果；调用者得到独立 region 副本。另保存 owned DIB 创建时的方向：
Windows `GetObject(DIBSECTION)` 会把负高度报告为正数，不能据此反转实际 top-down
内存。8 位索引、PNG alpha、主/mini 和四态缓存均加入此项回归。
未移除图片尺寸、XML 深度、节点数量、资源路径等加固。

## 验证入口

新增三个合成回归目标：

- `skin_render_compat_tests`：分钟 LED、BMP/PNG bar 像素、disabled thumb、
  BMP 留存/PNG 干净底图、三类 title、NULL/空 region 的 UI 应用。
- `playlist_render_compat_tests`：初始/保存尺寸、两栏选中图片、splitter、
  混合格式、箭头裁宽、两栏 scrollbar 分页相位及 alpha、title、旧 region 清理。
- `skin_region_compat_tests`：8 位重复索引/无匹配/全键色、四态按钮原规则、
  PNG alpha 0/1/128/255、缓存副本、读取失败恢复、XML 词法和真实资源名字选择。

原有 LED live-WM_PAINT 用例改为在 `99:59 → 100:00` 等真实位数边界测试；
旧 PNG 综合测试中误把 BMP bar 居中当作预期的断言作定点修正，其他既有修改保留。

```powershell
cmake -S . -B out/png-6120 -DBUILD_TESTING=ON -DTTPLAYER_STAGE_RUNTIME=OFF
cmake --build out/png-6120 --config Release --parallel 8 --target `
  ttplayer_rebuild skin_render_compat_tests playlist_render_compat_tests `
  skin_region_compat_tests led_repaint_tests skin_png_tests skin_alignment_tests `
  skin_rebind_tests progress_seek_tests skin_profile_tests
# STAGE_RUNTIME=OFF 时，先在测试 EXE 同目录放入匹配的重建版 ttpcomm.dll/ttpres.dll。
# 本次使用 build/Release 中的配套 DLL；不要放入根目录的原版 DLL。
ctest --test-dir out/png-6120 -C Release --output-on-failure `
  -R '^(skin_render_compat_tests|playlist_render_compat_tests|skin_region_compat_tests|led_repaint_tests|skin_png_tests|skin_alignment_tests|skin_rebind_tests|progress_seek_tests|skin_profile_tests)$'
```

原版/修正版真实窗口回放脚本：
[`artifact/skin_render_fixes_20261118/run_replay.py`](../artifact/skin_render_fixes_20261118/run_replay.py)。
它串行复用上一轮隔离探针，不修改历史证据或用户配置；以源位图字形解码 LED，
并核验实际 RGB、窗口矩形、region 和禁用帧，而不是只调用 PrintWindow 判断 BMP 局部留存。

## 本轮不做的推测性改动

没有重写 visual 输出排除、多控件合批、辅助子 HWND 独立打印等尚需消息序列补证的架构。
也没有回退用户要求的连续轨道拖动、松开 seek、PNG 扩展或窗口稳定性策略。
桌面栏播放状态重绑定、tooltip、splitter 折叠状态及键盘步长不在本轮主要绘图修正中。

## 执行结果

### 构建及交付

- 正式产物：`rebuild/build/Release/TTPlayerRebuild.exe`。
- SHA-256：`ee128ec00f0df8c358375b7d577d84b2de3af20e645c976a01adc86a62024782`。
- Win32 Release 构建成功；静态导入检查通过：24 个 DLL、763 个导入，
  subsystem 5.01，分别与 XP/Win7 清单校验。这不是在 XP/Win7 上的本轮运行测试。
- 配套 `ttpcomm.dll` SHA-256：
  `725437dd48490502a6df5f5f25761d6329f2750fa788b80fa2c5d02356e11e64`。
- 配套 `ttpres.dll` SHA-256：
  `8baa5835aafdaf28f5399aba629ad45e77ae7424b4c87cc145a3a25b1fb97bd7`。
- 没有覆盖根目录原版/旧重建版 EXE，没有提交或重置既有脏文件。

### 针对性回归：9 / 9 通过

上述九个 CTest 目标最终均通过，日志为
[`ctest-skin.log`](../artifact/skin_render_fixes_20261118/ctest-skin.log) 和
[`ctest-skin.xml`](../artifact/skin_render_fixes_20261118/ctest-skin.xml)。

- 新列表测试完成 **47,074 次条件检查、91,923 次像素比较**。
- 原有 PNG 综合测试保留 460 个图片引用、PNG/BMP 混用、动画、真实 WM_PAINT、
  局部重绘和辅助窗口覆盖；LED 的 52 个真实重画用例通过。
- 初轮新增测试的活动列表初始化、动画固定状态、splitter 父底图 oracle 已纠正；
  旧 Classic title 像素 oracle 改为不透明 BMP 复制。未用放宽容差掩盖差异。

### 原版 / 修正版窗口回放

启动前曾检测到安装版播放器，未关闭或操作它；稍后的安全检查不再检测到运行实例后，
执行了五组隔离探针。每个被测进程只使用自己的复制目录和配置，并由探针关闭。
原始审计目录和旧证据哈希清单保持不变，新取证在
[`artifact/skin_render_fixes_20261118/`](../artifact/skin_render_fixes_20261118/)。

[`runtime-results.json`](../artifact/skin_render_fixes_20261118/runtime-results.json)
中的修正断言通过：

| 检查 | 修正版实测 |
|---|---|
| 长时间 LED | 原版与修正版均解码为 `116:06` |
| bar 原点 | 两版采样均为 `(124,62,173)` |
| disabled progress | frame 3 的 110 个非色键像素，0 个不符 |
| BMP 新热帧孔洞 | 保留 `(212,212,212)` 旧像素 |
| 列表冷启动 | `373 × 160` |
| selected / splitter | 1px 边缘 stretch；splitter 关键采样与原版一致 |
| 箭头 | `[65,85,68,90)`，不再向右偏 2px |
| EQ BMP title | `(255,0,255)`，不再被错误色键抹掉 |
| 重复调色板索引 | 首匹配索引形成洞，第二个同 RGB 索引仍在 region 内 |
| BMP NULL / XML | 矩形回退、结束标签大小写、无引号实体资源选择均通过 |

**并非整个列表逐像素一致：**省略显式字体的原探针中，原版第一行实际高度 21px，
修正版 17px。因而 `(160,50)` 的绿色分量分别是 110、140，对应同一九宫格规则在
不同目的行高上的结果。回放验证器对两版各自的实际行高使用独立 nearest 九宫格
oracle，不把此差异算作 R06 的 tile/stretch 失败，也不声称字体/行高已修复。
此行高差异保留为后续项；BMP/PNG 同目的尺寸的几何一致性由合成回归检查。

### 扩展回归的未通过项

扩展 CTest 为 **13 / 16 通过**（包含上述九项）；完整日志保留为
[`ctest-final.log`](../artifact/skin_render_fixes_20261118/ctest-final.log)。
没有为了通过而修改下列测试或其非绘制功能：

- `ttplayer_tests`：在 `startup-settings` 断言停止。测试仍要求
  `check_update_days==7`，当前默认值为 `-1`，尚未进入皮肤测试。
- `options_drawing_tests`：选项页和注册页检查通过后，夹具清理遇到
  `registration-test-only.xml.bak`，`remove(directory)` 报目录非空。
- `desktop_lyrics_menu_tests`：`zoom tooltip mismatch`；其可选 zoom 提示读取
  当前 EXE 的字符串资源。直接检查测试 EXE 和正式 EXE 的 4104/4105 字符串，
  两者当前均为空，见 `zoom-resource-diagnostic.json`；未据此修改 PNG 绘制。
- 另尝试构建 `window_topmost_tests`，旧测试引用已不存在的
  `fullscreen_lyric_window_was_visible_` / `fullscreen_desktop_lyric_was_visible_`，
  因而没有运行该目标。错误保留在 `build-tests.log`。

补充按 `--filter skin` 运行核心测试：原目录的混用运行库先报归档版本错误；
切换到配套 `build/Release` 目录后，catalog、profile、TT2012、Vista、Vista EQ、
Media Player 10、Claymore、icon/mini 共八节通过，随后 `skin-resource-default`
因找不到其私有 `resources/root/ttpres.dll` / 旧 reverse 夹具而停止。
见 `core-skin.log` 和 `core-skin-runtime.log`；不能据此声称核心全套通过。
