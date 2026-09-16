# BaiduMusic8209 歌词标题缩放修复（2026-09-17）

## 对照 Let's Seven

对照 `TTPlayer5719/Skin/Let's Seven.skn` 的 `Skin.xml` 和 `lyric_skin.bmp`：

- 歌词标题直接绘制在背景图中，并非单独的标题控件。
- `resize_rect="55, 44, 253, 136"` 将左侧55像素及顶部44像素保留为固定区域。
  标题完整位于九宫格左上角，因此改变窗口宽度不会拉伸文字。
- 关闭、置顶、桌面歌词按钮使用独立图片及 `align="right"`，随右边界移动。

用户更新后的 `Skin/new/BaiduMusic8209.skn` 同样把“歌词秀”嵌入背景图，
但 `resize_rect="4,34,364,346"` 只固定左侧4像素。标题在约x=14..48处，
落入九宫格顶部中间的横向拉伸区，因此标题随宽度伸长。

`src/ui/player_window.cpp` 的 `DrawResizableSkinBitmap` 按皮肤指定的四条切线绘制九宫格，
此处行为符合皮肤定义。问题是包内切线错误，不需要为此皮肤修改通用解析器或文字渲染。

## 修改

仅将 Baidu 皮肤的歌词窗口属性改为：

```xml
resize_rect="64,34,364,346"
```

按 Let's Seven 的固定标题区原理，为本皮肤留64像素左侧固定宽度；
顶部、底部、右边距、歌词内容区和按钮位置均不变。其余76个ZIP条目逐字节不变，
没有重绘图片、转换格式或复制配置。生成模板也已同步，避免以后重新生成时复现。

源目录与Release使用用户最新混合BMP/PNG包；Debug仍保留其原有PNG包，
分别仅修改上述属性，没有相互覆盖其他内容。无需重建EXE。

## 宿主机验证

使用当前Release程序及隔离配置，关闭声音、自动播放、联网歌词和Discord。

| 包 | 尺寸验证 | 结果 |
| --- | --- | --- |
| Let's Seven参考 | 322×156、474×156、654×236、恢复322×156；鼠标拉到502宽 | 标题像素始终不变 |
| 修复前Baidu | 368×350、520×350、700×430、恢复368×350；鼠标拉到548宽 | 拉宽分别有406、433、424个标题区像素变化，复现变形 |
| 修复后Baidu | 同上 | 标题像素变化均为0；右侧按钮字形及相对位置不变 |

按钮验证比较亮色字形掩码，避免把渐变背景拉伸造成的单级颜色抖动误判成按钮变形。
三个运行实例均保持响应并正常退出。

本地工具位于 `BaiduMusic8209/compat/`：

- `repair-lyric-title-resize.ps1`：基于现有包生成候选，只改指定属性，验证其余条目哈希。
- `test-lyric-title-resize.ps1`：窗口尺寸和鼠标拖边实测、截图与像素比较；支持
  `-ExpectBroken` 及 `-SevenReference`。
- 对照结果：`lyric-resize-test-20260917-014102/results.json`。
- 修复前：`lyric-resize-test-20260917-014106/results.json`。
- 修复后：`lyric-resize-test-20260917-014110/results.json`。
- 四个部署位置的完整原包备份：`lyric-title-resize-20260917/backups/`。

源目录修复前SHA256：`F67FF938A0F3F9E4AEBC028D4FED225FF392596B7C460B6055EF5192BADE27D2`。
源目录修复后SHA256：`DFA46B9BF80E31CD565D1B0AAF6BF4A25E396E055C5D653F0BFA5593869C845A`。
