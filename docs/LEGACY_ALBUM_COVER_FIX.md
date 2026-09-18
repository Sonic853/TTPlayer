# XP / Win7 专辑封面兼容修复

日期：2026-09-19。

## 检查结论

旧实现存在程序兼容问题，PNG 封面并非在所有路径上正常：

1. 原版 `FUN_004AD0AD` 的 MIME 过滤只接受 JPEG / JPG / BMP / GIF。
   重建版在插件封面适配层和 MP3 / FLAC 兼容解析器中都恢复了这一限制，
   因而 PNG 数据可能在进入解码器之前就被丢弃。文件属性页此前却已允许导入 PNG，
   形成了“可以写入、播放时读不出来”的不一致。
2. 主窗口、全屏背景和任务栏封面以 WIC 解码为主、OLE 为后备。
   文件属性预览则只有 WIC 路径，缺少旧系统解码后备。
3. 文件属性预览将未预乘透明度的 BGRA 像素直接复制到位图，未与背景合成，
   透明 PNG 可能显示错误背景或边缘。

实际 XP SP3 虚拟机中，创建 WIC 工厂返回 `0x80040154`（类未注册）；
Win7 SP1 中返回 `S_OK`。不能把开发机上 WIC 可用当作 XP 一定可用。

## 修复

- 插件 `CopyThumbnail` 和兼容解析器接受 `image/png`，通配 MIME 使用完整的
  8 字节 PNG 签名识别；ID3v2.2 的 `PIC/PNG` 也纳入支持。
- PNG 是按本次要求增加的重建版能力。其它原版规则继续保留：读取首个封面，
  不搜索其它封面替换插件返回的空结果，不使用 `Folder.jpg` 冒充内嵌封面。
- 新增共用 `DecodeCoverImage`：先尝试 WIC，失败后尝试系统 GDI+ 1.0。
  它同时用于主窗口专辑模式、全屏专辑与备用背景、任务栏封面、文件属性预览。
- 两条解码路径统一输出自有的、从上到下排列的 **预乘 BGRA** 位图：RGB 已乘
  对应透明度，可直接交给 `AlphaBlend`。解码流与 GDI+ 图片在函数退出前释放，
  返回的位图不依赖它们继续存在。
- 文件属性预览按比例居中，并将透明像素与系统窗口背景合成。
- 任务栏解码仍限制最长边为 1024。共用解码器检查输入大小和图像尺寸，
  对无效数据、非法尺寸或内存分配失败返回空图，让现有无封面处理接管。

没有要求用户安装 WIC，也没有增加随程序分发的图像解码 DLL。
GDI+ 的 `Bitmap::FromStream` 及其最低系统要求参见
[微软文档](https://learn.microsoft.com/en-us/windows/win32/api/gdiplusheaders/nf-gdiplusheaders-bitmap-fromstream)。
WIC 的原生格式支持参见
[WIC 概述](https://learn.microsoft.com/en-us/windows/win32/wic/-wic-about-windows-imaging-codec)。

## 验证范围

### 格式与像素

- PNG：RGB、RGBA 透明、调色板及 tRNS、16 位灰度、Adam7 隔行。
- JPEG：普通及渐进式；BMP、透明 GIF、TIFF。
- 原始尺寸、宽高比、缩小、1024 边长限制、预乘透明度、预览背景合成。
- 空数据、截断 PNG、非法预览尺寸。
- PNG 内嵌封面：ID3v2.2 / v2.3 / v2.4、FLAC PICTURE。
- 原版 FLAC 插件输出的 `image/png` 及 `image/*` PNG 数据。
- 插件拥有封面接口但返回空数据时，继续保留原版首项优先规则。

### 执行结果

| 环境 | 结果 |
| --- | --- |
| 宿主普通版 Release | `cover_image_tests`、`taskbar_preview_tests`、`fullscreen_album_tests` 3 / 3 通过 |
| 宿主兼容版 Release | 同三项回归 3 / 3 通过 |
| VirtualBox XP Professional SP3，5.1.2600 | WIC 未注册；GDI+ 和自动后备路径的格式、像素、内嵌封面及原版 FLAC 插件检查通过 |
| VirtualBox Win7 SP1，6.1.7601 | WIC、GDI+、自动路径的格式、像素、内嵌封面及原版 FLAC 插件检查通过 |

Win7 虚拟机中 `taskbar_preview_tests` 也通过，覆盖比例、透明度、切歌缓存、
无封面及 DWM 错误回退。最终兼容 EXE 的静态导入审计通过：x86、子系统 5.01，
19 个 DLL、645 项导入，保留 XP 启动所需的 `ttpcomm.dll!#3` 静态依赖。

虚拟机运行的是使用兼容构建选项、链接生产封面代码的本地测试程序。
插件测试使用与仓库相匹配的原版 `ttpcomm.dll` / `ttp_flac.dll`，放在独立诊断
目录中，没有覆盖已安装播放器。此结果针对封面读取、解码和像素合成；不代表
所有第三方插件版本、显卡驱动或所有图像变体均已验证。XP 本身没有 Win7 的
DWM 任务栏预览，XP 的封面功能主要用于播放器和文件属性等窗口。

测试源码及样本生成器仅在 `rebuild/tests/`，不提交测试子模块，不打包测试文件，
Actions 保持 `BUILD_TESTING=OFF`。运行日志保存在本地 `out/cover-check-20260919/`。
