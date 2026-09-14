# 全屏歌词文字边缘平滑（2026-09-15）

## 原版证据与问题来源

本次只分析根目录 **5.7.9** 的 `reverse/decompiled/TTPlayer.exe.pseudo.c`，
不引用 6.1.20 的歌词实现。原版不透明歌词与透明歌词的字体质量不同：

- `0044B4A1` 根据 `this+0x204` 的全屏矩形选择普通/全屏配置。
  透明开关分别来自 `005479FC` / `00547A84`。开关非零时，明确执行
  `local_64.lfQuality = 3`，即 `NONANTIALIASED_QUALITY`。
- `004499AD` 在 AutoFontFS 调整后同样把 `00547AA6`（全屏 LOGFONT 的
  `lfQuality` 字节）设为 3，创建字体，再恢复保存的 LOGFONT。
  因此只改变实际安装的字体，不改变用户保存的 `FontFS`。
- `0043F6FA` 将窗口设为 `WS_EX_LAYERED`，以背景色作颜色键；其最后
  参数包含 `LWA_COLORKEY`。重建版的 `ApplyFullScreenLyricTransparency`
  原来同样调用 `SetLayeredWindowAttributes(..., LWA_COLORKEY)`。
- `00441C7E` 创建离屏位图，填背景，再调用 `0043FC10` 画文字，按需执行
  `004416E0` 的边缘渐隐和 `00441330` 的拖拽辅助线，最后 BitBlt。
  字体灰阶覆盖率在创建字体阶段就被关闭，不是歌词经过视觉效果低分辨率
  缓冲区缩放造成的。重建版原来的歌词 DIB 也一直使用歌词控件的实际尺寸。

颜色键只有“全透明/不透明”两种结果。直接把字体质量改为抗锯齿会留下
混入背景色的边缘，叠在专辑图或动态效果上形成毛边。**原版在此透明路径中
也明确禁用抗锯齿，不能把平滑透明合成声称为原版已有的逐像素行为。**

## 本次修复及边界

按用户的平滑显示要求，只替换透明全屏的文字合成路径：

1. 实际字体使用 `ANTIALIASED_QUALITY`，不使用有彩色子像素的 ClearType；
   保持原字体、字号、字重、字符集、自动字号和临时 LOGFONT 语义。
2. 新的 `lyric_alpha_mask.h` 在黑底上画白字得到灰阶覆盖率，随后按真实文字
   颜色生成预乘 BGRA。透明度不从文字颜色或颜色键反推，所以黑字、文字与
   背景同色也不会丢失。覆盖率缓冲在同一帧的可见行之间复用；只处理命中行
   的可见区域。读取 DIB 前调用 `GdiFlush`。
3. `UpdateLayeredWindow(ULW_ALPHA)` 更新现有 `LyricCtrl`，系统把边缘与
   实时视觉效果、专辑图或桌面合成，不截取/缓存底下的屏幕。背景是零 alpha，
   不增加黑矩形，也不重建 HWND 或改变屏幕位置、owner、Z 序。
4. 卡拉 OK 的普通/高亮覆盖区域分离，避免同一个灰阶像素叠加两遍后变粗；
   横向、纵向滚动及高亮时间边界不变。拖拽线和进度文字也写入 alpha 通道。
5. 从旧颜色键状态迁移时先清除并重设 `WS_EX_LAYERED`。这是 Windows 的
   [API 约束](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setlayeredwindowattributes)，
   否则后续 `UpdateLayeredWindow` 会失败。重复布局时不重置已有的 alpha
   表面；退出透明/全屏仍恢复原子窗口。

普通/迷你歌词保留原颜色键字体策略；不透明全屏保留原 FontFS 和背景/边缘
渐隐绘制。专辑图透明度、歌词时间轴、50/20 ms 动画节拍、全屏菜单和拖拽开关
不变。零 alpha 的空白区仍由现有全屏输入层接收鼠标，见
[FULLSCREEN_LYRIC_DRAG.md](FULLSCREEN_LYRIC_DRAG.md)。

## 宿主机验证

`fullscreen_lyric_render_tests` 直接加载 5.7.9 内置皮肤，调用实际字体和
歌词绘制函数，使用独立 HWND、内存播放时钟，不打开音频设备、不写用户配置。

- 中文/英文/数字混排行：纵向有 **4322** 个半透明边缘像素，横向 **1989** 个；
  检查 RGB 不超过 alpha、空白完全透明、黑字和同背景色文字保持覆盖率。
- 同色卡拉 OK 开/关逐像素一致；不同色高亮不改变覆盖率；改背景色不产生
  颜色键毛边。拖拽辅助线保持不透明；不透明全屏仍存在灰阶边缘。
- 在宿主机 DWM 的两个实际背景上各抽样 **150** 个文字边缘像素，与预乘
  alpha 计算结果比对通过（每通道容差 2）。不是仅凭字体质量标志判断。
- 重复颜色键迁移、透明/不透明切换、返回普通子窗口通过； HWND 不变。
  连续 30 帧不增加 GDI 对象；本机 1920×1200 离屏绘制约 **3.2 ms/帧**，
  不含 DWM 提交，不能推论所有设备/分辨率的帧率。
- 原全屏文字/空白拖拽与专辑背景测试通过。界面截图为
  `build/Testing/fullscreen-lyric-render/smooth.png`。

扩大回归时，鼠标实测曾有一次普通/迷你模式的位移断言失败，独立复测通过；
未修改该输入路径或放宽断言，不能将这次偶发失败隐去。置顶及任务栏控制测试
通过。另 `options_drawing_tests` 仍断言关联提示为“前往查看系统关联”，而用户
在提交 `160cbd5` 已主动改成“Win7 以上系统需要手动关联”，所以此既有测试
失败；本次未改动用户的新文案，也未把它计作歌词渲染回归通过。

运行：

```powershell
cmake --build build --config Release --target fullscreen_lyric_render_tests --parallel 4
ctest --test-dir build -C Release -R "^fullscreen_lyric_render_tests$" --output-on-failure
```

可给测试 EXE 第二个参数指定 PNG 路径，保存实际离屏结果的合成图；第一个参数
为包含原 `ttpres.dll` 的目录。本轮没有在其它 Windows 版本、HDR 或混合 DPI
环境实测，也没有声称透明渲染与原版关闭抗锯齿的像素一致。
