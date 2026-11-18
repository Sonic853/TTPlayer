# Winamp Modern (2).skn：播放时间数字局部刷新错误

日期：2026-11-18。范围：TTPlayer 5.7.9 原版、根目录重建 EXE、当前 Release 重建 EXE、当前重建源码及指定皮肤。

**后续修复已完成，见第 11 节。第 1–10 节保留修复前的取证结论，源码行号与二进制身份均对应当时版本。**

## 1. 结论

**根因不是数字素材损坏、播放时钟不准、切错字形或 BMP alpha，而是 LED 的实际绘制矩形与 Windows 失效矩形不一致。**

目标皮肤的 XML 是：

```xml
<led position="26, 39, 91, 42" image="number.bmp" align="left" />
```

XML 仅给出 `65×3` 的矩形；`number.bmp` 实际为 `156×17`，横向 12 格，每格 `13×17`。

- 原版加载素材后会重新设置 LED 子窗口的真实尺寸；本例实际为 `[26,39,91,56)`，即 `65×17`。
- 重建版绘制、图层与命中已经使用正确的 `65×17`。
- 但重建版播放更新仍用 XML 的 `[26,39,91,42)` 请求重画。
- `BeginPaint` 的目标 DC 只允许写入已失效的 3 行；后续 `RGN_AND` 不会扩大这个区域。
- 结果：数字顶部 3 行更新，下方 14 行保持旧内容。

这不是仅凭截图的推测。本轮同时取得了：

1. REA/Ghidra 对原版和根目录重建 EXE 的静态反编译证据；
2. 当前重建源码的完整失效、合成、输出链路；
3. 三个程序的隔离运行截图与逐像素比较。

**本轮只增加分析报告和取证附件，没有修改生产源码、皮肤包或用户配置，也没有实施修复。**

## 2. 分析对象及身份

| 对象 | 路径 | SHA-256 |
|---|---|---|
| 原版 | `TTPlayer.exe` | `c7999ea5823469c1684148cac1bd196009824348ccac5cdd45978f8fb574e28c` |
| 根目录重建版 | `TTPlayerRebuild.exe` | `b64491f75642b21b474e489ec2afc2786c4b8a446616d7d6d73dea3191211cef` |
| 当前 Release 重建版 | `rebuild/build/Release/TTPlayerRebuild.exe` | `98686b46f9984f48ea753f050782a9201aaa59cddb1e83b8bc66af52c8deab05` |
| 指定皮肤 | `Skin/Winamp Modern (2).skn` | `68783bde3ab71068f2d77f678da011f3b069cb7e087aef16b309abc6e3aed9b5` |
| 数字素材 | 包内 `number.bmp` | `b90a10606164615bc7224229487b9d108e374d51fe1d307d4bb79c340fee71ff` |
| 布局 | 包内 `Skin.xml` | `811e32b25c698785f1741c72fb498cf553b6e50399cc51c34f019bffd20fe1a3` |

原版及根目录重建版的静态地址分别属于各自二进制，不能交叉套用。当前源码基线 HEAD 为 `46503402f1bbba677fb01045ceaec750f849b2a5`。源码对应关系不等同于逐字节构建身份；当前 Release 的实际故障由独立运行证据确认。

REA 使用 Ghidra 12.1.4，未修改原始 EXE。最终完整证据：

[`../artifact/winamp_modern_led_rea_20261118.evidence.json`](../artifact/winamp_modern_led_rea_20261118.evidence.json)

截图、资源、运行元数据、测量结果及复现脚本：

[`../artifact/winamp_modern_led_20261118/`](../artifact/winamp_modern_led_20261118/)

## 3. 素材语义：单行 12 字形，而不是播放状态帧

`number.bmp` 是 24bpp BMP，实际尺寸 `156×17`：

| 字符 | 索引 | 源 X | 源 Y | 尺寸 |
|---|---:|---:|---:|---|
| `0`…`9` | 0…9 | `index×13` | 0 | `13×17` |
| `:` | 10 | 130 | 0 | `13×17` |
| `-` | 11 | 143 | 0 | `13×17` |

根节点透明色为 `#ff00ff`。该素材没有上下两行的播放/暂停数字，也不使用按钮的四状态切图规则。不能把 XML 高度 3 当作字形高度，更不能将素材高度除以状态数。

本轮干净绘制的原版及两份重建版 LED，均能与素材直接拼接的 `00:06` **逐像素完全匹配**；这直接排除了本例的字形选取、缩放和透明色解码错误。

## 4. 原版绘制链路：先建立真实控件，再按控件失效

以下语义名称是分析用名称，不代表恢复了原始源码符号。

### 4.1 加载和布局

```text
Skin.xml
  → 004A8536：分派 player_window 子节点
  → 004A988C：读取 led 的 position / image / align
  → 004710FD：将皮肤设置应用到主窗口控件
  → 0045120C：读取数字位图尺寸
       glyph_width  = bitmap_width / 12
       glyph_height = bitmap_height
  → 0045127C：计算当前显示格式需要的控件宽高
  → 004512E8 / 00451385：按实际宽高调整、放置 LED HWND
  → 004052F1：SetWindowPos
```

`0045120C` 的关键结果是：

```text
this + 0x34 = 156 / 12 = 13
this + 0x38 = 17
```

`004512E8` 会把矩形底边设置为 `top + glyph_height`；`00451385` 的汇编进一步确认：

- 左对齐：保留输入 left，重算 right；
- 其他对应模式：保留输入 right，重算 left；
- 不采用输入 bottom 作为字形裁剪高度；
- 调用 `SetWindowPos` 后使真实子窗口失效。

因此，这个皮肤的短 XML 矩形在原版中可以正常使用。XML 是布局输入，不是最终 `65×3` 的 LED 客户区。

本轮运行时枚举得到原版：

```text
class      = LEDTimerCtrl
control_id = 32221
parent     = TTPlayer_PlayerWnd
parent rect = [100,100,390,245)
LED rect    = [126,139,191,156)
LED client  = [0,0,65,17)
```

换算到父窗口客户区正是 `[26,39,91,56)`。

### 4.2 播放时钟、失效与最终绘制

```text
0046010E：主窗口 timer 10 分支
  → 播放器状态检查、读取 position / duration
  → 0045CE05：已用/剩余时间选择
  → 004C1F1C：毫秒转整数秒，再拆时/分/秒
  → 00451416：保存新值，并使变化数字区域失效
  → LED 控件擦除背景 / WM_PAINT
  → 004515BC → 00450257：按需恢复父窗口背景
  → 004515FF：组织数字、冒号及负号
  → 0045176A / 0045183D：逐字形输出
  → 00450A1C → 004507BD：TransparentBlt 或兼容掩码路径
```

`00451416` 使用的是 `GetClientRect(LED HWND)`，不是再次取 XML 的短矩形。它还能只失效变化的秒个位等后缀，但区域高度仍是控件真实的 17。

`0045183D` 的源、目标尺寸均采用真实字形宽高；透明色为无效哨兵时走普通 `BitBlt`，否则走透明复制包装。

原版存在格式控制位，包括对齐、带负号以及小时格式。这里仅确认本例 `MM:SS` 的实际结果；不把当前重建版所有长时格式策略直接宣称为原版语义。

## 5. 重建版链路：实际绘制正确，失效仍用 XML

### 5.1 当前源码路径

```text
LegacySkin::LoadPackage
  → src/skin/skin.cpp：保存 XML bounds，独立记录 image_size
  → UI timer：每 250ms 进入 RefreshPlaybackUi
  → CurrentLedText：从音频位置生成字符串
  → changed(L"led", ...)
  → InvalidateSkinElement(L"led")
  → MainSkinButtonBounds(L"led")             ← 返回 XML bounds
  → RedrawWindow([26,39,91,42))
  → WM_PAINT / BeginPaint                  ← 目标 DC 仅能写 3 行
  → PaintSkin
       新离屏画布、恢复父背景
       SkinLedBounds / DrawSkinLed          ← 正确画 65×17
       SkinLayers::ClipRepaint(RGN_AND)
  → BitBlt                                 ← 实际仅传回顶部 3 行
```

关键代码位置均相对于 `rebuild/`：

| 环节 | 文件和行号 | 结果 |
|---|---|---|
| XML 与素材尺寸分别保存 | `src/skin/skin.cpp:598–636` | LED 不是四态按钮，不会在此重设 bounds 高度 |
| 计算真实数字范围 | `src/ui/player_window.cpp:1290–1300` | 宽为字数乘 `image_size.cx/12`；高为 `image_size.cy` |
| 逐字形取图 | `src/ui/player_window.cpp:1303–1315` | 12 列、源 Y=0、完整素材高 |
| 图层使用真实范围 | `src/ui/player_window.cpp:1123–1158` | LED 分支调用 `SkinLedBounds` |
| 错误的范围查询 | `src/ui/player_window.cpp:4342–4344` | 非 play/pause 直接返回 `element->bounds` |
| 请求失效 | `src/ui/player_window.cpp:4372–4413` | LED 走矩形 `RedrawWindow` |
| 判断是否仅局部更新 | `src/ui/player_window.cpp:3156–3182` | pending 同样由错误的范围查询生成 |
| 完整离屏背景 | `src/ui/player_window.cpp:4545–4557` | 新画布与父背景并没有漏画 |
| 实际画 LED | `src/ui/player_window.cpp:4752–4755` | 真实 17 高 |
| 最终输出 | `src/ui/player_window.cpp:4778–4792` | 裁剪后才 `BitBlt` |
| 输出裁剪 | `src/skin/skin_layers.cpp:82–87` | `ExtSelectClipRgn(..., RGN_AND)` |
| 文本改变触发失效 | `src/ui/player_window.cpp:7203–7223` | 缓存值更新后调用 `InvalidateSkinElement` |

### 5.2 根目录 EXE 的 REA 交叉核对

不是仅用当前源码替根目录 EXE 下结论。REA 在根目录 EXE 中取得：

| 语义对应 | 地址 | 原生代码事实 |
|---|---|---|
| `RefreshPlaybackUi` | `004501CE` | LED 值进入改变检测 |
| `changed` 辅助函数 | `00450DA0` | 值改变后调用 `00445FE9` |
| `InvalidateSkinElement` | `00445FE9` | 调用 `00445DF5`，再 `RedrawWindow(...,0x41)` |
| `MainSkinButtonBounds` | `00445DF5` | 非 play/pause 分支复制元素 `+0x18…+0x24` 的原始矩形 |
| `SkinLedBounds` | `004382BF` | 素材宽除以 12、乘字数；底边等于 top 加素材高 |
| `DrawSkinLed` | `0043833D` | 0–9、冒号 10、负号 11，完整素材高取图 |
| `PaintSkin` | `00446BAD` | 画 LED，随后 `ExtSelectClipRgn(...,1)`，最终 `BitBlt` |
| 窗口消息处理 | `00440804` | `BeginPaint → 局部/完整绘制 → EndPaint` |

这里的 `0x41` 为 `RDW_INVALIDATE | RDW_NOCHILDREN`，裁剪参数 `1` 为 `RGN_AND`。重建版只有可视化为真实子窗口，数字等主要由父窗口模拟绘制；因此必须自行维护与原版真实子窗口等价的完整损伤范围。

### 5.3 为什么“离屏缓冲已经画完整”仍会错

本例最终能写出的区域：

```text
BeginPaint 更新区 ∩ 图层输出区
= [26,39,91,42) ∩ [26,39,91,56)
= [26,39,91,42)
```

重新绘制完整离屏背景不会解除目标 DC 的更新区限制。即便只把 `PaintSkin` 改成全量合成，若 Windows 失效区没有扩大，底下 14 行仍不能输出。

## 6. 隔离运行和像素实证

### 6.1 方法与边界

- 仅启动复制到会话临时目录的程序；不使用用户播放列表和配置。
- 输入为新生成的 30 秒、8kHz、16bit、双声道静音 WAV。
- 各程序使用同一个指定皮肤、已用时间模式，窗口为原生 `290×145`。
- 播放约 6 秒后暂停，先抓取屏幕当前内容；随后才调用 `PrintWindow`。
- 保持暂停，显式使整个窗口及子窗口重画，再抓屏。
- 比较仅限 LED `[26,39,91,56)`，不声称整个窗口像素一致。
- 运行取证使用 Win32/Python；REA 证据为静态分析，不伪称 REA 观察到了这些运行调用。

根目录原有 `ttpcomm.dll` 与重建 EXE 不兼容，原有 `ttpres.dll` 也不能满足重建资源要求。隔离重建运行使用 `rebuild/build/Release` 的配套文件，**没有替换工作区根目录 DLL**：

```text
ttpcomm.dll SHA-256
725437dd48490502a6df5f5f25761d6329f2750fa788b80fa2c5d02356e11e64

ttpres.dll SHA-256
8baa5835aafdaf28f5399aba629ad45e77ae7424b4c87cc145a3a25b1fb97bd7
```

相应元数据记录在各 `*-runtime.json`。两个重建进程退出码为 0；原版处理关闭消息后返回 1，取证本身成功。所有本轮启动的进程均已结束。

### 6.2 结果

`summary.json` 中的像素统计：

| LED 比较指标 | 原版 | 根目录重建 | 当前 Release 重建 |
|---|---:|---:|---:|
| 暂停屏幕 vs 暂停 PrintWindow，差异像素 | 0 | 62 | 62 |
| 仅顶部 3 行差异 | 0 | 0 | 0 |
| 仅下方 14 行差异 | 0 | 62 | 62 |
| 屏幕下方 14 行 vs 初始 `00:00` 下方 14 行 | 62 | 0 | 0 |
| 整窗重画后 vs PrintWindow，LED 差异像素 | 0 | 0 | 0 |
| 干净暂停结果 | `00:06` | `00:06` | `00:06` |

干净暂停图像的五个字形，与 `number.bmp` 对应五格的差异均为 0。

所以重建版的实时数字是**新 `00:06` 的顶部 3 行，加旧 `00:00` 的下方 14 行**，不是合法的完整 `00:00` 字形；只是视觉上很接近旧数字。两份重建 EXE 的故障模式相同。

原版正常暂停屏幕：

![原版](../artifact/winamp_modern_led_20261118/original-paused-live.png)

当前重建版暂停屏幕，尚未强制重画：

![重建版局部刷新错误](../artifact/winamp_modern_led_20261118/current-paused-live.png)

同一暂停状态，完整重画后：

![重建版完整重画正确](../artifact/winamp_modern_led_20261118/current-paused-redraw.png)

### 6.3 为什么历史截图与测试会漏掉

`tools/compare_playback_skins.ps1:91–109` 使用 `PrintWindow`；当前主窗口的 `WM_PRINTCLIENT` 在 `src/ui/player_window.cpp:3153–3155` 直接执行完整 `Paint`。

因此“PrintWindow 截图正确”不能证明播放中屏幕增量更新正确。本轮保留先抓屏、后打印的顺序，就是为了区分两种结果。

现有相关覆盖也不完整：

- `tests/progress_seek_tests.cpp:153–159` 已测试短 XML 矩形的 `SkinLedBounds`，但不检查真实失效区。
- `tests/skin/skin_png_tests.cpp:413–462` 的局部 LED 夹具中 XML 高与素材高相等，且数字条缺少可区分字形。
- `tests/skin/png_repaint_cases.inc` 已提供读取实际 WM_PAINT 后窗口 DC 的方法，应复用，而不是把 PrintWindow 作为残影验证入口。

没有重跑或改动这些既有测试；测试子仓库中的已有修改全部保留。

## 7. 修复方案

### P0：让实际范围贯穿全部失效链路

最小修改点为 `src/ui/player_window.cpp:4342`。示意代码，**未应用**：

```cpp
RECT PlayerWindow::MainSkinButtonBounds(std::wstring_view name) const {
    const auto* element = FindActiveSkinElement(name);
    if (name == L"led")
        return element ? SkinLedBounds(*element, CurrentLedText()) : RECT{};
    if (name != L"play" && name != L"pause")
        return element ? element->bounds : RECT{};
    // 保留现有 play/pause 几何逻辑……
}
```

这能同时修正 `InvalidateSkinElement` 和 `WM_PAINT` 的 pending 区域计算。更清晰的长期接口是 `RuntimeSkinElementBounds`，让绘制、命中、失效和损伤分类共享同一套语义，而不是继续用“ButtonBounds”处理 LED。

**只修 PaintSkin 的字形高无效——它现在已经正确。只更改末端图层裁剪，也不能突破 BeginPaint 的更新区域。**

### P1：清除旧范围，并正确处理位数变化

仅修“当前实际范围”仍不能完整处理文本变窄：

```text
左对齐：
00:00     → [26,39,91,56)
-00:00    → [26,39,104,56)

按当前重建版格式策略：
01:00:00  → [26,39,130,56)
```

因此剩余模式、长时格式以及长时反向 seek，都可能超出 XML 原宽。格式缩短时，还必须擦除旧范围中已不属于新文本的部分。

建议：

1. 保存上一已呈现 LED 的实际范围，并维护待绘制损伤。
2. `damage = old_actual_bounds ∪ new_actual_bounds`。
3. 范围未变时，使用修正后的 LED 元素失效路径。
4. 范围改变时，可用现有 `InvalidateSkinBacking(damage)` 触发区域内完整重合成；它会设置 `skin_backing_dirty_`，避免最终输出再次缩到“仅新 LED 图层”。
5. 多次文本更新合并到一次 WM_PAINT 时，累积 damage，不得只保留最后一个新矩形。
6. 换肤、普通/迷你切换、销毁时清空旧布局的呈现范围及待绘制状态。

不建议每秒整窗失效作为最终修复。正确方式是完整覆盖 LED 及其暴露区，同时保留可视化子窗口和兄弟控件原有绘制顺序。

### P2：一次绘制使用同一份时间文本与几何快照

目前图层构建与实际 `DrawSkinLed` 可分别调用 `CurrentLedText()`。音频线程可能在两次调用之间推进到位数变化边界。

建议一次取得 `text + actual_bounds`，供图层、取图和范围管理共同使用。根据需要使用音频的统一时钟快照，避免位置、时长分开读取产生边界不一致。

### 不应作为正式修复的做法

- 将数字素材改成 3 像素高，或猜测上下状态行；
- 强制 BMP 改走 alpha、改透明色或填黑清屏；
- 只调整 UI 刷新周期；
- 只在停止/暂停时整窗重画；
- 改目标 XML 为 `26,39,91,56` 后就宣称宿主兼容性恢复。

最后一项可以在隔离复制的皮肤上作诊断实验，但原版无需修改该包，而且负号、长时格式及旧范围清除问题仍然存在。

## 8. 应补充的验收

1. **精确目标夹具**：XML 高 3，素材高 17，12 个字形均可区分；验证运行时范围为 `65×17`。
2. **真实更新区**：只改变 LED 文本后，`GetUpdateRgn` 必须覆盖 y=55，不能仅检查辅助函数返回值。
3. **实际屏幕/窗口 DC**：`00:01 → 00:02 → 00:08` 后，全部 17 行与干净参考一致；取证入口不使用 PrintWindow。
4. **时间模式**：已用/剩余切换、负号、右对齐、长时格式、位数增长和缩短、反向 seek。
5. **损伤合并**：一次 WM_PAINT 前多次更新，及 LED 与 progress/info 同时失效。
6. **背景与遮挡**：纹理父背景、透明孔洞、重叠 PNG、独立 VisualCtrl；旧字形不能变成黑块或覆盖错误的兄弟控件。
7. **状态**：播放、暂停、暂停定位、恢复、停止、换肤。停止淡出时钟策略须与绘制范围问题分开验证。
8. **格式忠实度**：原版小时格式受控制位影响；超过一小时的具体格式应独立与原版对照，不能仅以重建版 `FormatLedTime` 为原版参考。

本轮运行确认的是目标包短高度导致的残影；长时格式、位数缩短和停止淡出的边界尚未完成独立运行验收。生产修复尚未实施，因此也不存在“修复后测试通过”的结论。

## 9. REA 证据索引

以下为最终 bundle 中的完整 Evidence ID；反编译均是静态伪代码/汇编，不是原始源码或运行观测。

| 证据 | Evidence ID |
|---|---|
| 原版数字位图装载 `0045120C` | `ev_348c3ccc95545010add645e2366757254fdd6259f845fc48f379c148f310a349` |
| 原版实际尺寸计算 `0045127C` | `ev_c26e4eda2900ddccc2a673b5fe10a364169bdf5c6d609d506e632f84fa2ae8e5` |
| 原版控件重设 `004512E8` | `ev_addcb24427d60180bcba881ccb8fc7dc90c11e03015e0dc0207179ecca18e1be` |
| 原版放置汇编 `00451385` | `ev_eec6b89ff8c504a658388af5c93320814e9ac6e2b70b3c02a9f2c66fc336d332` |
| 原版数值更新/失效 `00451416` | `ev_4373a99829b4b8c993fe69ef8f5def5603eac572efb42f3120c79bb6757e2bde` |
| 原版播放时间更新 `0045CE05` | `ev_d22b52c13f716fcb7a0c54cd2dd32cfec10200f0cf6f3e1e825bb8a47713eddc` |
| 原版 timer 分支 `0046010E` | `ev_3cc68be1d3956f067ce8e0c53567d5060f8398bcdcccd944c06d0501a5e62b7e` |
| 原版逐字形绘制 `0045183D` | `ev_f5ea076303e2c85f55d63109cb068b85be50419fc95893931cbd7cdf6a529eae` |
| 重建版实际 LED 范围 `004382BF` | `ev_bf945eef6903ae23204341eccc08e26e661c933ba0070aa3639b21512f06ea87` |
| 重建版逐字形绘制 `0043833D` | `ev_ee343664bf2de742bde8b06dd55bdda90f055a27a525d2e41b5cb43fc36bb839` |
| 重建版错误范围查询 `00445DF5` | `ev_0ae17f978f3e654d418e3a850e50bf5b9dfe4ad8b6d1db1cdad5cd675128a62e` |
| 重建版元素失效 `00445FE9` | `ev_1f315bc35dd078975184e5add4450fff95c14641234fd7acd16cb11ec9ca8d47` |
| 重建版 PaintSkin `00446BAD` | `ev_2409330ac2b853558fd5c8ba194e8c0b65e171f019dd1e36d08b19cb6c83f83f` |
| 重建版窗口消息处理 `00440804` | `ev_6848fbfef4657e8b1485ef7fc6421d0b123fb63414a6c6543a45039b83a296ec` |
| 重建版改变检测 `00450DA0` | `ev_41f277099a1db7e07e3d6f0c1aea7738a8df41880cedbba02dfc4ad54a6e12b4` |
| 指定皮肤 ZIP 静态清单（父记录） | `ev_298129b6d49bc4d2aba88c77c186fab34a90f14866a6f1303bf72a791cadc192` |

## 10. 完成度

| 问题 | 状态 |
|---|---|
| 指定皮肤的素材布局、原始矩形 | 已确认 |
| 原版加载、真实几何、失效、绘制链路 | REA 静态证据及运行几何确认 |
| 根目录重建版对应原生实现 | REA 静态确认 |
| 当前重建源码中的根因 | 已确认 |
| 两份重建 EXE 的实际故障 | 隔离运行及像素确认 |
| 修改范围与修复设计 | 已提供，未实施 |
| 负号、小时、缩短位数等完整动态矩阵 | 尚未完成运行验收 |
| 修复后回归 | 未实施修复，未进行 |

REA 会话已关闭；51 条 Evidence 已导出，随后清空本次内存调查。原版 EXE、重建 EXE、皮肤和原有用户改动均未被替换。

## 11. 后续修复与验收

### 11.1 实际修改

按后续“完成修正”的要求，已修改重建实现：

- `MainSkinButtonBounds` 对 LED 使用 `SkinLedBounds`，不再把 XML 锚定矩形作为完整失效范围。
- LED 保存 UI 已请求显示的文本快照；图层、字形绘制、命中及可视化遮挡使用同一快照，不会在音频时钟跨位数边界时各自读取不同文本。
- 使 LED 失效时先保留旧实际范围，再更新显示快照。范围变化则将旧、新范围并集交给 `InvalidateSkinBacking`，完整重合成暴露区。
- 多次更新在 WM_PAINT 前合并时，Windows 更新区累积，并保留 backing-dirty 标志。
- 已用/剩余命令立即更新 LED 快照；换肤、普通/迷你重绑会清空旧布局的缓存。
- 没有改变数字条、色键、时间格式策略或皮肤 XML，也没有使用周期整窗失效兜底。

涉及实现文件：

```text
include/ttplayer/ui/player_window.h
src/ui/player_window.cpp
src/ui/player_window_skin_animation.cpp
src/ui/player_window_visual.cpp
```

### 11.2 专项回归测试

新增 `tests/skin/led_repaint_tests.cpp` 和对应 CMake/CTest 目标 `led_repaint_tests`。未改写原有已经被用户修改的皮肤测试文件。

测试使用与目标包相同的 `156×17` 数字条及 `65×3` XML，字形每行均可区别，并包含色键孔洞；预期图像直接由原始像素拼接，不调用生产数字绘制函数作为参考。

它验证真实更新区及 WM_PAINT 后窗口 DC，共 44 个转换用例：

- 全 17 行更新；
- 左/右对齐、普通/迷你布局；
- 秒数变化、负号、小时位数增减；
- 先增长后缩短且中间不绘制的损伤合并；
- 请求刷新后音频时钟跨越小时边界，绘制仍使用同一 UI 快照；
- 停止归零、旧字形透明孔洞及纹理背景恢复。

修复前测试以 `LED invalidation omitted native glyph rows or old/new width` 失败。修复后 44 个转换用例通过。

以下相关 CTest 最终全部通过（4/4）：

```text
skin_png_tests
led_repaint_tests
progress_seek_tests
skin_rebind_tests
```

日志：[winamp_modern_led_fix_20261118/ctest.log](../artifact/winamp_modern_led_fix_20261118/ctest.log)。

### 11.3 原始皮肤实机复核

重新构建的程序在隔离目录中使用未修改的 `Winamp Modern (2).skn` 播放相同静音 WAV。播放约 6 秒后，先抓屏、再 PrintWindow：

| LED 比较 | 修复前 | 修复后 |
|---|---:|---:|
| 屏幕 vs PrintWindow 差异像素 | 62 | **0** |
| 下方 14 行差异像素 | 62 | **0** |
| 屏幕 vs 原始素材拼接 `00:06` | 混合旧/新字形 | **0** |
| 停止屏幕 vs 原始素材拼接 `00:00` | — | **0** |

截图、运行身份及完整测量：
[winamp_modern_led_fix_20261118](../artifact/winamp_modern_led_fix_20261118/)。

![修复后未经强制重画的屏幕](../artifact/winamp_modern_led_fix_20261118/current-paused-live.png)

### 11.4 构建产物

修正版：

```text
rebuild/build/Release/TTPlayerRebuild.exe
SHA-256: c51622a70c1fcb586d58e67e8109130f11c796eceb0add03b53cc547cf1e5512
```

该目录也生成更新后的 `TTPlayer.exe` 别名。Release 构建的 XP/Windows 7 静态导入检查通过（主程序 24 DLL、763 imports）；这不是 XP/Windows 7 实机运行验证。

工作区根目录的旧重建 EXE 没有覆盖：该目录还承担原版运行环境，不能仅替换 EXE 或混换两版 DLL。使用上述 Release 目录及其配套资源运行修正版。

测试时临时启用了构建目录的 `BUILD_TESTING`，结束后恢复原值 `OFF`。需要重新通过 CMake 构建测试时，先以 `-DBUILD_TESTING=ON` 配置。
