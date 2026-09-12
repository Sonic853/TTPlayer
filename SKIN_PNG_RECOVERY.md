# 6.1.2 BMP / PNG 皮肤兼容

依据本地 `TTPlayer6120/reverse` 中的 EXE 伪代码、汇编及从 `ttpres.dll`
提取的 `<DEFAULT_SKIN>` ZIP。这是向现有重建版补充 6.1.2 皮肤图片及控件能力，
不是将整套 6.1.2 主程序或云服务原样移植。旧版默认皮肤不被替换。

## 加载与绘制

| 原版地址（6.1.2） | 重建实现 |
| --- | --- |
| `0051E1A5`、`0046CD40` | 沿用资源 ZIP / 外部 `.skn` 的解压、Skin.xml 和附属 XML 加载流程 |
| `00522A01`、`0041751D` | 检查 `BM` 及 `bfSize`，匹配时保留 BMP DIB / 调色板分支 |
| `0046A76A`、`0046A653` | 其他编码图片从内存流交给 GDI+，不依赖扩展名 |
| `00417430`、`0041760A` | `SkinImage` 统一有效性及共享资源生命周期 |
| `0046A938`、`0046A9F6` | GDI+ NearestNeighbor / Half，保留逐像素 Alpha 和整体透明度 |
| `00521E6F`、`00521FA4`、`00522428` | 解析按钮、滑块、工具栏的 flash 属性及图片 |

新增 `skin_image.h/.cpp`。PNG 不经过 `TransparentBlt`，不将 `#ff00ff`
作为透明色，也不将半透明边缘压成实色。BMP 的旧色键和不透明绘制分别保留。
内存流保留到 GDI+ 图片销毁之后，避免延迟解码引用已释放流；共享图片所有权支持
皮肤切换、布局复制及预览。图片缓存使用完整标准化路径，避免同名子目录图片冲突。

为兼容现有 `GetObject`、窗口区域和背景读取接口，PNG 另有 32 位 DIB 视图；
PNG 控件实际绘制仍使用原 GDI+ 图片，不以这个 DIB 替代 Alpha 绘制。
这属于重建版架构适配，不是复制原版 0x20 字节对象的二进制布局。

图片分流已接入主/迷你窗口、歌词按钮、均衡器按钮和滑块、播放列表标题/关闭按钮/
工具栏、共享平铺/九宫格及选项中的皮肤预览。父背景先恢复到双缓冲表面，再合成 PNG。

## 状态与动画

- 图片横向固定四态：普通、悬停、按下/选中、禁用。
- `frame_count` 是过渡计数，不是四态图片的分割数量。
- `00418FA5` / `0041917E`：普通与悬停之间开始、反转、结束动画；按下/禁用立即显示静态帧。
- 按钮 mode 1 使用正常图加悬停图透明度过渡；mode 2 使用 `flash_image` 帧条。
- `004AD31C` / `004AD423`：播放列表七个工具栏单元分别维护 normal/hot 过渡。
- `004A9ED7` / `0042FFDB` / `0042FA6B`：进度滑块在播放时循环脉冲，暂停时收尾；
  拖动时画静态按下帧。解析 mode 1 的 flash 透明度、mode 2 帧条、mode 3 滑块透明度。
- UI 使用一个调度定时器维护独立控件计数；没有动画时关闭定时器，换肤/迷你切换时清空状态。
  它不是原版每个子类各自的定时器实现，不能据此宣称调度时刻逐指令一致。
- `set` 连接现有选项窗口；五个 `mode_*` 只显示当前模式，点击循环切换，并更新提示信息。
- `Playlist.xml/Color_SelText` 补充为可选选中文本颜色；缺省继续使用旧版规则。

## 验证

在宿主机使用 VS 2026 / Win32 / Release 独立构建，`TTPLAYER_STAGE_RUNTIME=OFF`：

```powershell
cmake -S rebuild -B rebuild/out/png-6120 -G "Visual Studio 18 2026" -A Win32 `
  -DBUILD_TESTING=ON -DTTPLAYER_STAGE_RUNTIME=OFF
cmake --build rebuild/out/png-6120 --config Release `
  --target ttplayer_rebuild skin_png_tests skin_rebind_tests
ctest --test-dir rebuild/out/png-6120 -C Release `
  -R '^(skin_png_tests|skin_rebind_tests)$' --output-on-failure
```

`src/skin/skin_png_tests.cpp` 验证：

- 合成透明/半透明/不透明 PNG、PNG 内容使用 BMP 扩展名、坏图片和图片/流复制生命周期。
- 真实默认皮肤的全部 **36 张 PNG**（整体透明度 255/128/0）与直接 GDI+ 参考绘制逐像素比较。
- 默认包全部 **75 张 BMP** 与原 `TransparentBlt` 参考逐像素比较。
- 真实布局/四态尺寸/flash 属性、主窗体/迷你/均衡器/列表绘制、五模式循环。
- 从 6.1.2 `ttpres.dll` 加载、右键菜单外部 PNG 皮肤切换、切回资源皮肤及 HWND 保留。
- 外层 Skin 及 TTPlayer6120/Skin 共 **179 个 `.skn`** 解压及布局加载。

`skin_rebind_tests` 另验证 LX-iPlay、TT2012、Let's Vista、旧默认皮肤往返后保留 40 个 HWND、
播放列表选择、歌词编辑文档及窗口隐藏状态。测试生成截图/解压文件保留在专用临时目录。
无本地逆向素材时，新增测试运行合成用例并明确跳过真实素材测试。

## 使用与边界

6.1.2 的资源 ZIP 可直接作为 `.skn` 使用，无须替换正在使用的旧 `ttpres.dll`。
把提取的 `ZIP__DEFAULT_SKIN__2052.zip` 复制为运行 EXE 同目录的
`Skin/TTPlayer_60.skn`，然后从皮肤菜单选择 `TTPlayer_60`。

按用户要求，登录（`login`/`login_name`）和音乐窗（`browser`）控件保留解析数据，
但在主/迷你窗口及皮肤预览中隐藏，不注册悬停提示，也不响应控件点击。
本轮不恢复云登录、手机管理、浏览器/上传业务窗口。
`fill_image2` 已解析和保留，但网络缓冲进度的上游数据通路
不在本轮实现范围。没有把全部 6.1.2 行为或所有皮肤所有交互声明为与原版完全一致。
未运行原版 EXE 逐帧屏幕对拍；像素参考来自原版已确认的 GDI+ 调用方式和真实图片。
