# 原版 5.7.9 与当前重建版：皮肤绘制全链路对照

日期：2026-11-18  
性质：**分析与取证，不是修复提交**。本轮未修改播放器生产代码。

## 1. 结论与证据口径

当前重建版已经恢复了皮肤加载、固定槽位、普通四态按钮、主/迷你布局、下载填充、歌词/EQ/列表外框等主要链路。剩余差异不应再概括成“尚未实现整套皮肤”，也不能因为使用父画布就判定所有画面错误。

本轮最重要的结论是：

1. **仍有普通真实皮肤可触发的差异**：长时 LED 格式、禁用 progress 帧、BMP 按钮状态透明洞、播放列表冷启动尺寸。
2. **图片映射规则仍有明确遗漏**：主窗 `bar_image`、列表选中背景、图片分隔条、分隔箭头，以及 BMP title 的不透明复制语义。
3. **透明轮廓与 XML 兼容边界仍不完全相同**：背景 8 位索引、全色键背景、结束标签大小写、无引号属性实体。
4. **最需要后续专项验证的是独立重绘时序**：visual 输出排除、多控件局部合批、辅助子 HWND 单独重画。不能拿稳定截图证明所有中间帧等价。

下面 R01–R13 均有本轮原版/当前版运行对照，但适用范围不同：有真实包命中的，也有构造合法 BMP/旧 XML 边界的。S/U 项则分别为静态差异和未完成的验证，不混入“已经截图复现”的清单。

### 1.1 比较基线

| 项目 | 身份 |
|---|---|
| 原版 | `D:\Projects\Backup\TTPlayer\TTPlayer.exe`，5.7.9，x86 |
| 原版 SHA-256 | `c7999ea5823469c1684148cac1bd196009824348ccac5cdd45978f8fb574e28c` |
| 当前运行版 | `D:\Projects\Backup\TTPlayer\rebuild\build\Release\TTPlayerRebuild.exe` |
| 当前 SHA-256 | `c51622a70c1fcb586d58e67e8109130f11c796eceb0add03b53cc547cf1e5512` |
| 根仓库 HEAD | `6b876ba95dca16ed881a91ee449f9b5dbe739a44` |
| `rebuild` 仓库 HEAD | `51b9ccf94e1cdb56dddb6f438634745bb1cdd9f4`，`皮肤绘制修正` |

**没有把根目录的旧 `TTPlayerRebuild.exe` 当作本轮“当前版”。** 当前版本已经包含上一轮 LED 失效范围修正。

原版隔离运行使用原配 DLL；重建版使用 Release 配套 DLL，未混换：

| DLL | 原版 SHA-256 | 当前 SHA-256 |
|---|---|---|
| `ttpcomm.dll` | `e349ef73e8a1d35b2c5debd2ddaeb5cc2513a90647766b7339d2605b34483b33` | `725437dd48490502a6df5f5f25761d6329f2750fa788b80fa2c5d02356e11e64` |
| `ttpres.dll` | `866726b4c9b712ef0cb5492a74d05bbb15a97f8f47595d743dcf268aa25853a5` | `8baa5835aafdaf28f5399aba629ad45e77ae7424b4c87cc145a3a25b1fb97bd7` |

### 1.2 取证范围与限制

- REA/Ghidra：原版包入口、解析器、图片对象、背景/region、主控件、附属窗口关键绘制函数；另外核对当前 Release 的 LED 格式化及部分合成伪代码。
- 源码：当前工作区 `src/skin`、主窗、visual、歌词窗、EQ、列表、桌面歌词工具栏；文件哈希见取证目录的 `source-identity.json`。
- 运行：每次复制到独立目录，生成测试配置，不覆盖用户正常配置；测试进程已经关闭。
- 坐标统一采用客户区像素；示例窗口无普通非客户区边框。矩形使用 `[left, top, right, bottom)`。
- 所有原版地址均为本报告原版 EXE 的 VA；不能套到当前 EXE。
- 伪代码存在寄存器参数漏显。影响结论的关键位置使用汇编或运行补证，不能将反编译签名当成真实 C ABI。
- 本报告覆盖**皮肤宿主的端到端主干及关键分支**，不是所有皮肤、所有消息次序的像素等价证明。歌词正文排版/卡拉 OK 全部字形、visual 各插件算法、浏览器 DLL 内部页面绘制不在本轮完整验收范围。

## 2. 原版的完整链路：不是按 XML 顺序依次贴图

### 2.1 包、解析、描述符、绑定

```text
启动/选择皮肤
  ├─ 外部 SKN 文件
  └─ <Default_Skin> → 模块 ZIP 资源
          ↓
004A747D  加载包 XML
  → 0041ED9D / 0041F73F  建立 ZIP 来源
  → 0041F32E             按名字读取成员
  → 004A7929             XML 文档及窗口用途分派
  → 004A8315             version=2、元数据、透明色
          ↓
固定用途槽位，而非任意 UI 节点树
  player/mini: 004A8536
  lyric:       004A88D0
  EQ:          004A8B73
  playlist:    004A8DC6
  desktop bar: 004A92A2
          ↓
004A9D8C → 00408CF4       BMP 外层检查、DIB 对象
Lyric.xml / PlayList.xml / Visual.xml 等附属描述
          ↓
0045D5FA → 0045DDEE → 00468363
保存/恢复配置、选择主/迷你描述、绑定各窗口
  主/迷你: 0046D0C1 → 004A43CD → 004710FD
  列表:    0046BCBE
  EQ/歌词: 各自的绑定/布局函数
```

创建窗口和绑定皮肤是两条生命周期路径。`0045FAD8 → 0044E50C / 00467B9B` 建立真实子 HWND；换肤通常重新绑定这些 HWND，而不是重新解释 XML 创建任意数量的控件。

`0041F73F` 明确区分 `CreateFileW` 和 `FindResourceW → SizeofResource → LoadResource → LockResource`。外部包不是默认皮肤的替身。成员查找中可以看到 OEM 名字转换及不区分大小写比较。

### 2.2 背景、缩放、窗口形状

主窗父背景消息链：

```text
WM_ERASEBKGND / WM_PAINT / WM_PRINTCLIENT
  → 004A1E8E
  → 004100F7 / 00410112
  → 004105E2
      ├─ 原始背景 DIB
      └─ 已生成的背景缓冲
  → 00408E2B
      ├─ SetDIBitsToDevice
      └─ StretchDIBits(..., SRCCOPY)
```

背景生成：

```text
004A3DC9
  ├─ 固定尺寸：00408E2B 原尺寸复制
  └─ 可缩放：0040A94B 九宫格
                 ├─ 固定四角
                 ├─ 中部/边条 stretch
                 └─ resize_tile != 0 → 0047AA84 平铺，裁切最后残片
```

`0040A94B` 明确设置 `COLORONCOLOR`。普通有效分区下，当前九宫格主公式与其基本一致；不能继续把整个背景拉伸恢复工作报为缺失。

**原版也有离屏合成和缓存。** `0040FF73`、列表 `0046FAEF` 等 resize 路径先画背景，再从背景生成窗口 region，然后枚举可见子 HWND，调整 DC 原点并发送 `WM_PRINT(..., 5)`，将子内容画入缓冲。

所以正确区别不是“原版没有画布、当前有画布”，而是：

> 原版同时存在背景/resize 缓冲、独立子窗口 WM_PAINT、定时器直接 GetDC 三类路径；这些路径不总是恢复同一份底图，也不总按同一顺序覆盖。

固定 8 位背景用 `0048C9CA` 按首个匹配调色板索引生成 region；其他背景走 `004506AF / 00445709`。窗口 region 来自**背景本身**，不是所有按钮图像的并集。缩放缓冲中的子窗口打印发生在 region 生成之后。

### 2.3 三种“区域”不能混用

1. **顶层窗口 region**：窗口轮廓及窗口外点击是否穿透。
2. **子控件正常帧 region**：按钮命中与兄弟窗口遮挡；通常不随 hover 帧透明洞重新生成。
3. **某次绘制的更新/可见 region**：由失效区域、DC 裁剪、`WS_CLIPSIBLINGS` 和消息顺序共同决定。

正常帧 region、hover 透明色、鼠标命中和“当前屏幕上的像素”并不是同一个概念。这也是 R04 以及 visual 风险的关键。

## 3. 当前重建版对应链路

```text
SkinPackage::Open / OpenDefaultResource
  → LegacySkin::LoadPackage
  → LegacyXmlDocument
  → 固定用途 SkinElement / 各附属布局
  → 按引用准备图片、SkinImage 共享存储
  → LoadSkinPackage 的预检与提交
  → ApplyLoadedSkin / 各窗口 Apply...Skin
          ↓
布局、背景 region、按钮正常帧 region、状态
          ↓
主/迷你 WM_PAINT
  → PaintSkin
      背景
      → BMP 按钮
      → progress / volume
      → info / LED / icon
      → PNG source-over pass
      → 限定局部输出、扣除 visual 命中 region
      → BitBlt
      → stereo/status 屏幕 DC 文字 pass
          ↓
visual 子 HWND / worker、info timer 仍有独立绘制
```

关键源码：

| 环节 | 当前位置 |
|---|---|
| 包与成员读取 | `src/skin/skin_package.cpp:68–218,370–401` |
| 旧 XML 词法 | `src/skin/legacy_skin_xml.h:30–185` |
| 元数据、各窗口槽位 | `src/skin/skin.cpp:139–227,566–909` |
| BMP/PNG 与绘制 | `src/skin/skin_image.cpp:82–170,253–399` |
| 背景九宫格、region helper | `src/ui/player_window.cpp:770–927` |
| 换肤预检和提交 | `src/ui/player_window.cpp:2405–2564,6470–6549` |
| 主窗局部失效 | `src/ui/player_window.cpp:4368–4432` |
| 主窗绘制 | `src/ui/player_window.cpp:4548–4829` |
| 命中/绘制区域分离 | `src/skin/skin_layers.cpp:5–86` |

这些代码已经包含真实字形高度、旧/新 LED 范围并集、统一 LED 显示快照。**本报告不把上一轮已修正的短高度残影重新列为现存问题。**

## 4. 各控件/窗口从状态到像素的分支

### 4.1 主窗、迷你窗的按钮

原版：

```text
00452FFF
  → 0040A1DD  安装四态 BMP、原尺寸、正常帧 region
状态/capture/checked/EnableWindow 改变
  → InvalidateRect(child)
  → 0040F288
      WM_ERASEBKGND → 00426434：返回成功，不恢复底图
      WM_PAINT      → 0040FC78 → 0040A35B
  → BitBlt 或 0040890C / TransparentBlt
```

checked 通常选择 frame 2，但热状态 frame 1 优先；disabled 为 frame 3。播放/暂停在同一个按钮 HWND 上切换图像及 region，保留输入状态。

当前 `SkinButtonState` 已恢复普通四态、checked、capture、孔洞命中、null bitmap 保留旧像素等规则。剩余的 BMP 非 null 状态透明洞差异见 R04。

### 4.2 progress、volume 与 EQ slider

原版：

```text
输入/播放状态 → 数值、是否 enabled、thumb 位置
  → 旧/新 thumb strip 并集失效
  → 004667BB → 0046A99C
      背景/填充虚调用
        volume/EQ: 00451E07
        progress:  0045287B
      → 00451F59：当前状态 thumb
```

原版 progress 顺序为：背景 → 下载填充 → 播放填充 → thumb。`0045269C` 的 24 位下载填充暗化、保留色键，目前已实现，不是本轮 gap。

**bar 与 fill 不是同一种定位规则：**

- bar：客户区左上开始、源 `(0,0)`、按客户区宽高复制。
- fill：原图居中，再按 thumb 中心/下载比例裁切。
- thumb：四帧原尺寸、按几何位置绘制；disabled 使用 frame 3。

当前 EQ 的普通 bar/fill 路径较接近原版；主窗 progress/volume 的 bar 仍居中，见 R02。主 progress disabled 分支见 R03。

### 4.3 LED、普通文字、滚动 info、图标

- LED：`0045CE05` 将毫秒拆成时/分/秒，`00451416` 存储时间并按变化位失效；`0045127C` 求字形尺寸，`004512E8` 调整真实子 HWND。
- LED 默认 flags 为 1；掩码 `0x2` 控制小时栏，`0x8` 控制负号，`0x4` 控制锚点方向。普通宿主不会因超过一小时就自动切换小时栏。
- LED 背景：`004515BC → 00450257`。后者使用 `DrawThemeParentBackground`，或转换坐标并请求父窗背景；不等同于按钮的“不擦背景”。
- 普通 Label：`00408FC9` 负责父背景/纯色底；`00409023` 负责字体、颜色、水平对齐和 `DrawTextW`。
- info：独立定时滚动/切换，相关函数 `00409250 / 0040942B / 004096AE / 0040989E`。当前已有直接 DC 动画、端点等待时不重复覆盖按钮等恢复。
- 图标：独立原尺寸 `DrawIconEx`，不把 XML 矩形当成缩放目标。当前常规路径没有发现新的通用尺寸缺口。

**主 UI timer 是 250ms，不是 200ms**：`0045FAD8` 明确 `SetTimer(..., 10, 0xFA, ...)`。当前 250ms 与此一致。info 的 40ms 动画和 visual 工作循环是另外两条时钟，不能互相代替。

### 4.4 visual

原版 `00457B11` 在工作线程/更新路径中获取 visual 自身 DC，调用 `00457E2D`，再释放 DC。它不是只等父窗完整重画。

当前也保留真实 visual 子窗及独立 worker；普通父窗重画时主动保留 visual 区域是有明确动机的。但最终输出扣除的是 `SkinLayers::Region(visual)`，即命中区域，不一定等于这次应该保护的绘制范围，见 U01。

### 4.5 歌词窗

原版 chrome/背景：

```text
0044919C / 004495C8 绑定与定位
  → 00449313 选择正常、mini、Transparent+TransSkin 模式
  → 正常模式 0044E8CC
  → 背景/缓存 + 004493DC title
  → 00408E2B
```

按钮仍各自走共享 SkinButton。当前 `player_window_lyrics.cpp:1831–2009` 分开处理父背景、迷你边框、title、按钮底图及当前帧；Transparent+TransSkin 停放 chrome，正文扩展到客户区。

当前已经考虑可伸缩歌词窗 XML 初始尺寸；不能把列表的 R05 推广成所有辅助窗口都忽略初始尺寸。BMP title 共享 R09 的静态差异；本轮 title 的运行补证是在 EQ，未逐个复测歌词窗口。

### 4.6 均衡器

原版 `0042955B` 绑定 balance、surround、preamp 和十段频率；十段由模板按“模板宽度 + eq_interval”推进。`00429AAA` 创建按钮/滑块，close 走较后的公共创建路径。

当前 `player_window_equalizer.cpp:371–418,610–817` 已覆盖 13 个 slider、实际范围、缺图/零宽帧区别及 fill 原尺寸裁切。没有证据再报“只画部分频段”“fill 一律拉伸”等旧问题。

仍需针对重叠皮肤核对按钮/slider 不同的 sibling clipping 及单独子 HWND paint，见 U04。

### 4.7 播放列表

原版将职责分给真实子控件：

| 内容 | 原版路径 |
|---|---|
| 绑定/回退 | `0047E6FC` |
| title | `0047EC2F → 00408E2B` |
| toolbar | `00467126 → 0046BA3A → 00454025 → 00454111` |
| 两栏选中行 | `00487C0D / 004897C1 → 0040A94B(...,0)` |
| splitter | `0041C091 / 0041C356` |
| scrollbar | CoolSB 回调 `00488463` |

当前 `PaintPlaylist` 在父画布中依次画背景、title、两栏、选择背景、分隔器、toolbar、滚动条和 close；原生 ListView 的系统绘制被接管。R05–R08 是具体行为差异，而不是根据“父画布架构不同”推断出来的故障。

### 4.8 桌面歌词工具栏

原版：

```text
00418B74 创建按钮/tooltip
  → 004186BC 绑定栏背景、图标、各按钮
父 paint: 0041A7A5 → 0041AA1C → 0041AB1F → 00408E2B
子按钮:   独立共享 SkinButton paint
状态切换: 004188FB → 00452FFF → 0040A1DD 重新绑定 play/pause
```

当前 `desktop_lyrics.cpp:915–1223` 创建真实子 HWND，但按父栏画布统一发布像素。PNG 是另外的 source-over 扩展。播放/暂停仅切绘制描述、未重新应用 HWND 几何/region，见 S04。

## 5. 已经运行确证的剩余差异

### R01．超过一小时的 LED 格式仍不同

**真实皮肤：`Skin/Winamp Modern (2).skn`。**

7200 秒静音 WAV 定位、暂停后的结果：

| 项目 | 原版 | 当前 |
|---|---|---|
| 已播放时间 | `116:06` | `01:56:06` |
| LED 字格数 | 6 | 8 |
| 实际范围 | `[26,39,104,56)` | `[26,39,130,56)` |
| 剩余模式另一次抓图 | `-03:53` | `-03:53` |

当前多出的 26px 进入 `stereo/status` 所在矩形。该结论不是 OCR 猜测：原版真实 LED 客户区尺寸、12 格 `number.bmp`、当前格式化函数及截图相互对应。

原版默认不启用小时栏，分钟可以增长到三位；当前 `FormatLedTime` 超过一小时自动输出 `HH:MM:SS`：

- 原版：`004511AC / 0045127C / 00451416 / 0045D4E2`。
- 当前：`src/ui/player_window.cpp:1272–1287,4943–4961`。
- 当前 Release REA：`00438EF5`，Evidence `ev_28df24d4e80eaac0e4cca9f5ad1bfd58403a2a8c5f4da2ef55e46e6b91598ed1`。

这是**时间表示规则**的问题，不是上一轮已修复的高度/失效范围问题。不能通过重新裁成 XML 的 3px 高度来掩盖。

### R02．主窗 bar_image 被居中，而原版左上复制

以 103×13 的坐标编码 BMP 替换 volume bar，控件保持 83×9，其他素材不变：

| 客户区取样 `(210,133)` | 原版 | 当前 |
|---|---|---|
| RGB | `(124,62,173)` | `(144,88,173)` |
| 对应源像素 | `(52,4)` | `(62,6)` |
| 定位规则 | 从 `(0,0)` 左上裁剪 | 大素材居中后裁剪 |

原版 `00451E07 / 0045287B → 00451D7D`；当前 `player_window.cpp:4620–4624,4685–4694`。

适用于 BMP 原生能力，不依赖 PNG。真实包 `043 金属特质BIG.skn` 的 bar 只比控件宽 1px，C++ 负数除 2 截断为 0，恰好掩盖问题；不能把该包未经取样直接当作位移实证。

**不要一起修改 fill 的居中规则。** fill 原本就应居中并按 thumb 中心裁切。

### R03．禁用 progress 当前仍画 frame 0，原版画 frame 3

真实包 `043 金属特质BIG.skn`，**无媒体启动**，原版 progress HWND 的 `IsWindowEnabled == FALSE`：

| 对 110 个非键色 thumb 像素比较 | 原版截图 | 当前截图 |
|---|---:|---:|
| 对 frame 0 不同像素数 | 91 | 0 |
| 对 frame 3 不同像素数 | 0 | 91 |

thumb 范围为 `[29,5,48,11)`，包含几何 helper 的 1px inset。点 `(30,5)` 原版为 disabled 灰色 `(82,82,82)`，当前为正常绿色 `(34,131,65)`。

- 原版 `00451F59` 无 capture 时按 `IsWindowEnabled` 选择 0/3。
- 当前 `player_window.cpp:4663–4669` 明确回落到 frame 0。
- ElegantLife/default 的相关正常与禁用帧相同，无法用它们证明 frame 0 选择正确。

播放后再停止的第一组探针中，原版仍然 enabled；因此本报告使用第二组**无媒体启动**证据，不将“停止”等同于 disabled。

### R04．BMP 状态新增透明洞：原版留旧像素，当前恢复底图

真实包 `031 iTunes milk new.skn`，主窗 playlist 按钮的局部 `(10,3)`：

- normal：`(212,212,212)`。
- hot：键色 `(255,0,255)`。
- 其下父背景：`(202,202,202)`。

真实鼠标进入后的**屏幕抓图**：

| | hover 前 | hover 后 |
|---|---|---|
| 原版 | `(212,212,212)` | `(212,212,212)` |
| 当前 | `(212,212,212)` | `(202,202,202)` |

原版按钮不恢复父背景，TransparentBlt 的透明点保留原 DC 内容；当前 `PaintSkin` 每次从背景重建画布，再复制到屏幕。

这是确证的历史像素留存语义差异。**不是说原版的残留总是更美观**；是否保留这种 bug-compat 应作为兼容决策，但不能称当前已经逐像素等价。

仅主窗此实例做了运行补证。辅助按钮也有重建背景的代码，应单独测试，不能直接声称所有辅助窗口都已实测复现。PNG 的 alpha 重组不按这条 BMP 历史规则判错。

### R05．列表冷启动忽略 XML 初始宽高

真实包 `013 ironger-BlueArmorPlayer.skn`，新配置没有保存的列表矩形：

| | 原版列表 HWND | 当前列表 HWND |
|---|---:|---:|
| 客户区尺寸 | **373×160** | **261×105** |
| 来源 | max(背景尺寸, XML 初始尺寸) | 背景尺寸 |

这次尺寸是在冷启动已创建但隐藏的真实列表 HWND 上读取的，不是由截图猜测。

- 原版 `00468363 → 0046BCBE`；汇编 `0046BD54–0046BD73` 同时比较宽、高，避免只相信漏显宽度的伪代码。
- 当前创建：`player_window_playlist.cpp:2987–3020`，只在有保存矩形时调整。
- 当前运行时换肤：同文件 `3053–3069`，已经有 XML 初始尺寸分支。

因此是**创建/重绑两条路径不一致**。只读盘点发现 68 个窗口定义具备触发条件；不等于逐包运行复现了 68 个故障，已有保存矩形也可能掩盖。

### R06–R08．列表选中背景、分隔条和箭头的图片映射不同

用 Winamp Modern 包加入三份坐标编码 BMP；在正式对照中，**两版列表客户区都显式调整为 290×145**，排除 R05 的尺寸干扰。

#### R06：selected_image 应保留 1px 边缘后 stretch，当前是整图 tile

原版两栏都先将源 RECT `InflateRect(-1,-1)`，再调用 `0040A94B(...,0)`；当前两栏都调用 `TileBitmap`。

9×7 选择图、同一曲目行，点 `(160,50)`：

- 原版 RGB `(102,110,7)`，对应源 `(3,3)`。
- 当前 RGB `(30,110,7)`，对应源 `(0,3)`，表现为每 9px 重新起始。

原版：`00487C0D / 004897C1`；当前：`player_window_playlist.cpp:4377–4378,4480–4481`。

真实候选包括 `Flying-玫紫.skn` 的 31×15 和 `LX-iPlay.skn` 的 100×19 选择图片；本轮没有把这些候选逐包截图验收。

#### R07：splitter_bar_image 同样不应整图 tile

7×17 bar，在点 `(64,57)`：

- 原版 RGB `(26,49,33)`，对应源 `(1,3)`。
- 当前 RGB `(26,10,33)`，对应源 `(1,0)`，第 17px 后重新起始。

原版 `0041C091 → 0040A94B(...,0)`；当前 `player_window_playlist.cpp:4424–4425`。

#### R08：splitter_arrow 应居中，当前右对齐

7px 宽 bar、3px 宽单帧箭头：

- 原版可见箭头 `[65,85,68,90)`，位于 bar 左起 2px。
- 当前 `[67,85,70,90)`，位于 bar 左起 4px。

两版 Y 相同，当前 X 确认右移 **2px**。

原版 `0041C356` 还会将单帧宽度限制到 bar 宽；当前 `player_window_playlist.cpp:4429–4435` 没有同样限制。

取证目录保存了两版完整图及选择行裁剪图，见 `auxiliary/`。

### R09．BMP title：原版不透明复制，当前额外应用色键

在 Winamp Modern EQ 不透明背景内放置 7×7 洋红 BMP title，位置 `[20,20,27,27)`：

| 点 `(23,23)` | 原版 | 当前 |
|---|---|---|
| RGB | `(255,0,255)` | 父底图 `(206,214,222)` |

原版 EQ `0042980D → 00408E2B`，没有色键参数；当前 `player_window_equalizer.cpp:718–719` 给 title 传入透明色。

歌词 `004493DC`、列表 `0047EC2F` 也是同样的不透明复制路径；当前对应 title 也传色键。因此**三处静态差异确认，EQ 一处运行确认**。

现有 title 的键色常落在背景本来就透明的角部，不能只凭素材含洋红就声称现成包可见错误。

### R10．固定 8 位背景：首个透明索引与“所有同 RGB”不同

构造 320×180 的 8 位 BMP，调色板索引 1、2 都是洋红，分别在两个小块中使用：

| `PtInRegion` | 原版 | 当前 |
|---|---|---|
| 使用索引 1 的 `(45,20)` | false | false |
| 使用索引 2 的 `(52,20)` | **true** | **false** |

原版 `0046D0C1 → 0048C9CA` 只把**首个匹配索引**挖掉；当前背景先转换为 RGB，再挖掉所有相同颜色。

当前 `SkinImage::CreateRegion` 的 `frames == 4` 按钮专用索引兼容已经存在，但不等于背景 region 也修复了。

范围是固定窗口原始 8 位路径；原版可缩放缓冲转成 32 位后，本来就走 RGB 规则。本轮实际包盘点未发现重复透明色索引背景，属于真实运行确证的罕见兼容边界。

### R11．全色键 BMP 背景：原版矩形回退，当前启动回退默认皮肤

将主背景换成 320×180 的全洋红 24 位 BMP：

- 原版接受，客户区保持 **320×180**，背景复制出洋红；`GetWindowRgn` 返回 0，与静态 `SetWindowRgn(NULL)` 路径相符。
- 当前拒绝该主背景形状，启动回退到内嵌默认皮肤，客户区 **327×141**。

**NULL region 不等于空 region。** 原版 region 生成器没有不透明扫描段时返回 NULL，安装 NULL 是清除自定义形状、恢复矩形；当前主窗 helper 返回空 HRGN，再由预检拒绝。

当前辅助窗还存在另一种静态边界：换肤得到空 region 时丢弃它，但不清除旧 region；新 HWND 可能仍是矩形，已有异形 HWND 则可能保留旧轮廓。**后者本轮未做运行复现。**

PNG 全 alpha=0 的真空覆盖属于扩展，不能一概改成 BMP 的 NULL 回退。

### R12–R13．两项 XML 词法差异改变了实际皮肤选择

#### R12：结束标签大小写

只把末尾 `</skin>` 改成 `</SKIN>`，用途节点仍小写：

- 原版接受，显示构造的 320×180 背景，采样 `(58,99,135)`。
- 当前拒绝，启动回退默认 327×141。

原版 `004C7177 → 004C6B86(..., false)` 忽略结束标签 ASCII 大小写；当前 `legacy_skin_xml.h:176` 严格比较。

这不意味着用途节点分派也应改成不区分大小写；那是另一条规则。

#### R13：无引号属性的命名实体

使用 `image=a&amp;b.bmp`，包内同时放两份不同颜色的有效 BMP：

| 引用结果 | 原版 | 当前 |
|---|---|---|
| 文件名 | 字面 `a&amp;b.bmp` | 解码后 `a&b.bmp` |
| 取样 RGB | `(58,99,135)` | `(173,81,42)` |

原版 `004C76CB` 的无引号分支直接复制字节；带引号分支才调用实体处理。当前两种分支都调用 `Value()`。

本轮未发现真实包使用这两类写法；它们是低优先级的旧语法兼容边界，不应与普通皮肤渲染错误混为一谈。

## 6. 静态确认、尚未运行复现的差异

| ID | 差异与依据 | 限定 |
|---|---|---|
| S01 | volume bar 当前传色键；原版 `00451E07` 的 bar 分支为 SRCCOPY。当前 `player_window.cpp:4692–4693`。 | 有键色像素时才区分；本轮真实 BMP bar 样本未命中。与 R02 的坐标问题独立。 |
| S02 | scrollbar bar：原版 `00488463` 对 part 2/3 各从源 `(0,0)` 重新平铺；当前 `player_window_playlist.cpp:4651–4665` 从整条 scrollbar 顶部连续铺，再盖按钮/thumb。 | 非均匀纹理有相位差；BlueArmor 的 10×29 背景是候选，尚未记录 thumb 中间位置运行图。 |
| S03 | splitter bar/arrow：原版 `0047E6FC` 两图均有效才绑定，否则清空并回到 5px；当前只要 bar 有效就采用图片宽度。 | 本轮样本未发现仅一图有效。另有箭头点击折叠路径遗漏，见下文。 |
| S04 | 桌面栏 play/pause：原版 `004188FB → 00452FFF` 重设尺寸/region；当前 `UpdatePlayback` 只改 `playing_` 和失效，几何/region 仅在 `ApplyBarSkin` 更新。 | 同尺寸同形素材可能看不出；不能每次直接改成 pause XML 原点，原版运行换图保留当前原点。 |
| S05 | 桌面栏 tooltip：共享按钮输入 handled 后提前 return，使后面的 `TTM_RELAYEVENT` 收不到普通 MOVE；工具又没有 `TTF_SUBCLASS`。 | `desktop_lyrics.cpp:666–698,949–983`。属于视觉提示路由，不是文案缺失，未运行等待 tooltip 验收。 |
| S06 | 普通 stereo/status Label：原版 `00409023` 未加 DT_NOPREFIX，当前 `DrawSkinText` 始终加。 | 文字含 `&` 才有区别，不推广到 info，常规资源未必可触发。 |
| S07 | progress 键盘步长：原版 `0045AD86` 设置短曲约 1000ms、长曲 5000ms；`00452413` 汇编明确用 `+0x6C` 加减。当前 `MainSliderGeometry` 省略 step，默认 1，而 progress 单位为 ms。 | 已补齐静态算术，不再只是伪代码猜测；本轮未运行键盘步长测试，也不是纯贴图错误。 |
| S08 | 运行时选择已消失的皮肤包：原版 `0045D5FA` 改选默认；当前 `LoadSkinPackage` 打开失败即返回、保留当前皮肤。 | 启动缺包回退已实现，不重复报旧 bug。是否保留当前更安全的失败策略，应单独决策。 |

**分隔器折叠路径补证：** 原版 `0041C8F5` 在未拖动且释放仍命中箭头时合成双击；`0041C941` 汇编将非零宽度存入 `+0x68` 并切到 0，再次操作恢复保存宽度。当前 DOWN/UP 只有拖动，双击不处理 splitter，箭头固定第一帧。因此这条状态/布局更新链没有恢复；本轮没有实际点击对照。

## 7. 未验证的重绘与资源风险：不能冒充已复现故障

### U01．visual 命中 region 被当成最终输出保护区

`player_window.cpp:4798–4809` 用 `layers.Region(visual)` 从父输出 DC 扣除；`skin_layers.cpp:40–42,65–72` 返回的是扣除前方命中区域后的 region。

原版 slider/info/普通静态等并非一律带 `WS_CLIPSIBLINGS`。滑块单独 paint 可能临时画到 visual 上，之后才被 visual 独立帧盖回。当前即使已画入 canvas，也可能被最终 exclusion 永久挡掉这一次输出。

**静态机制存在冲突，但具体窗口中间帧尚未复现。** 建议冻结/控制 visual 更新后分别只失效 slider、status、按钮，再进行父完整重画与下一帧 visual，对比各阶段，而不是只看最终稳定截图。

### U02．多个局部失效合批的次序

`SkinLayers` 完整模式会让“不裁剪兄弟”的后续控件扣除较早 paint region；传入 `repaint` 时不执行同样扣除。实际 `PaintSkin` 又固定先 volume 后 info 等。

因此 `{info, volume}` 同批局部重画与完整重画，在重叠区域可能得到不同结果。当前内部 full/partial 机制不一致可静态确认；**原版某一次消息序列最终应由谁覆盖，仍需单独记录**。

### U03．info/icon DOWN 带来原版没有的失效

当前非四态控件 DOWN 也调用失效；原版 info/icon 的相关 DOWN 主要是按 WS_TABSTOP 决定焦点，没有相同显式重画。重叠内容可能因此额外被覆盖。

原版 `004347EE / 0045196C` 已由 REA 补证；实际可见结果仍依赖先前像素和 timer 阶段，不作已复现结论。

### U04．辅助子 HWND 单独 paint/print 与 EQ 兄弟裁剪

- 原版 slider 子 HWND 独立 `WM_PAINT/WM_PRINTCLIENT` 会画背景、fill、thumb。
- 当前 EQ slider、部分列表/桌面子控件的 `WM_PAINT` 只 Begin/End，实际内容等待父合成。
- EQ 原版按钮增加 sibling clipping，而 slider 保留传入样式；当前 `SkinLayers` 的 EQ 描述默认全部裁剪兄弟。

正常父窗更新可能足够维持画面，不能称这些子控件“普遍空白”。应专测仅失效子 HWND、单独打印、遮挡恢复和按钮/slider 重叠。

### U05．极端尺寸、透明方向与缓存/提交边界

- 原版复制 helper 的零宽/零高具有“用整幅图片相应尺寸”的 sentinel；当前常直接跳过非正尺寸。特别是无色键 fill/零下载比例，仍需构造运行。
- 负 `biHeight` 的 8/32 位 BMP region 可能存在原版倒置的 bug-compat 边界；本轮未测，不应推广到通常 bottom-up BMP。
- 四个真实背景的 `resize_rect` 超出源图范围。原版直接运算，当前钳制；属于有样本的防御性差异，未证明当前画面错误，不建议直接撤掉约束。
- 当前一轴小于源图时整体缩放，与原版继续九宫格不同；通常最小窗口尺寸限制会阻止该情况。
- 换肤预检前置已经保留旧皮肤；但提交后抛异常、退出 mini 后晚期失败的完整回滚，仍需故障注入。
- ZIP 条目内存预算、缓存目录重解析点/竞态、碎片化 region 的性能是待验证风险，不是本轮已证明的安全漏洞。

## 8. 已恢复项与有意扩展：不应作为新 bug 重报

| 项目 | 当前判断 |
|---|---|
| LED 真字形范围、旧/新范围并集、统一显示快照 | 上一轮已修复；不回退。上一轮 44 个专项用例、4 组 CTest 的数字不是本轮重新运行的数字。 |
| Winamp 短高度 LED 62px 差异降为 0 | 上一轮结论保留；R01 是另一项长时格式问题。 |
| 普通四态、checked/disabled、capture、孔洞命中 | 主要状态机已有，不再报“没有四状态”。主 progress R03 是独立分支错误。 |
| null bitmap 按钮保留、按钮 NULL region 回退 | 相应按钮路径已恢复；R10/R11 是背景路径，R04 是有效 BMP 的透明状态像素。 |
| 24 位 downloaded fill 暗化 | 已实现。 |
| 按引用读取、缓存内容核验、version 检查 | 已恢复，不再引用旧文档中的“全包解压/只比长度”等描述。 |
| 重复窗口/用途槽位逐字段合并 | 已恢复。 |
| 歌词初始尺寸、Transparent+TransSkin、mini_border 分支 | 主要恢复已存在，不能套用旧列表/歌词缺口。 |
| EQ 13 个滑块、原尺寸 fill、close 创建顺序 | 已存在。 |
| 左右 scrollbar、三图完整性、13px fallback、thumb 分段 | 已存在；S02 只是背景相位。 |
| 无图 toolbar 七格文字与整数余量 | 已恢复，不因专用 helper 含特例就推定全部错误。 |

以下为扩展/稳定性策略，不按 5.7.9 原生缺失判错：

- PNG/GDI+、alpha coverage、source-over、动画及较新/社区节点。
- 明确的 UTF-8、ZIP UTF-8 名字、路径分隔符等宽容支持。
- toolbar `<item>`、`Color_SelText`、桌面 A+/A−、mini_lyric 配置。
- 用户要求的轨道按住连续拖动、progress 松开后 seek；不能以原版不同为由撤销。
- XML/图片/ZIP 边界检查、包内资源路径限制等安全加固。
- 原版 alpha=255 会清除 WS_EX_LAYERED；当前有意保留以减少换肤/菜单重定向问题。未证明这是 alpha 数值错误。
- 原版同步淡入并泵消息、当前 timer 异步淡入：终态可相同，中间帧/重入不是严格等价，但不自动归为 bug。

## 9. 建议的后续修复/验证顺序

**本轮只给建议，没有实施下面的改动。**

1. **优先修正局部且确定的普通路径**：R01 LED 格式、R02 bar 定位、R03 disabled thumb、R05 列表创建尺寸。
2. **修正列表图片映射**：R06/R07 复用并测试“1px 边缘九宫格 stretch”；R08 箭头居中/限宽；同时补 S02 的分段相位回归。
3. **明确 BMP 历史兼容政策**：R04 像素留存、R09 title/bar 的 opaque copy、R10/R11 背景 region。避免把 PNG source-over 混入同一规则。
4. **再处理状态/消息链**：桌面 play/pause 重新绑定、splitter 折叠、tooltip relay、键盘 step。
5. **最后扩大兼容边界和时序验证**：XML 两个词法点、visual、多控件合批、辅助单独子窗 paint、极端 fill/region。

建议最小回归矩阵：

| 主题 | 必须有的反例 |
|---|---|
| LED | 59:59→60:00、99:59→100:00、剩余模式、主/mini、左右锚点 |
| bar/fill | 小于/等于/大于控件，含键色/无键色，横/纵；不能把 bar 的规则套给 fill |
| disabled thumb | 四帧明显不同；无媒体、不可 seek、capture 取消 |
| BMP 状态 | normal 不透明、hot 变键色；在完整重画/PrintWindow 前读取屏幕 |
| 初始布局 | 无保存矩形、有保存矩形、运行换肤、主/辅助相对坐标 |
| 列表图片 | 四边不同的 selected、非周期 splitter、两图缺一、两帧不同箭头 |
| scrollbar | 非均匀 bar，thumb 起/中/末位置，两个 page part 分别核验 |
| region | 8 位重复索引、无匹配索引、全键色；NULL 与空 HRGN 分开 |
| 更新时序 | 单个失效、同批两个失效、直接 DC 动画、父完整重画、下一帧 visual |

## 10. REA 证据索引

完整记录、参数、目标 SHA、局限均在：

- [REA bundle](../artifact/skin_render_chain_audit_20261118/rea-full.evidence.json)
- [索引](../artifact/skin_render_chain_audit_20261118/rea-index.json)
- [原版选定伪代码](../artifact/skin_render_chain_audit_20261118/original-selected.pseudo.c)
- [原版选定汇编](../artifact/skin_render_chain_audit_20261118/original-selected.asm.txt)
- [当前 Release 选定伪代码](../artifact/skin_render_chain_audit_20261118/current-selected.pseudo.c)

bundle 共 167 条记录，复用了前轮记录，也保留了旧重建二进制的历史记录；**只有 SHA 为本报告当前基线的记录用于证明当前 Release**。其中原版目标 125 条、当前 Release 目标 11 条，其余为历史/其他范围记录。派生的 `original-selected`/`current-selected` 已按上述两个 SHA 筛选。

关键 Evidence：

| 用途 | 原版函数/当前函数 | Evidence ID |
|---|---|---|
| 包入口 | `004A747D` | `ev_c4d01cd69425c75e2f57be0907c63201435495070902069ae065382016c1f92d` |
| 文件/默认 ZIP 资源 | `0041F73F` | `ev_507f0d81f7e2be8f7a965635a88e231866a9d9f072969a7a84023a11b9cc496b` |
| 主背景消息分派 | `004A1E8E` | `ev_97a28afc02982fca417e3c42c6042750d3f2a801295bf8f4fe9bf4e5b4fdd4c6` |
| 主背景输出 | `004105E2` | `ev_f12847a821eb8a51eab39607371e27eeafe13038ae545dfdb095789bb5e6063c` |
| resize 缓冲/子窗打印 | `0040FF73` | `ev_233c6a1949e0afe024192cd8b54b54df66f1671afcae73475fbe74fe43f8b30d` |
| 九宫格 | `0040A94B` | `ev_27c13b71fd94df7a894de5cf6f4c1090c13aa1c3c1d99c4bc85c57b03ef523cf` |
| LED 默认 flags | `004511AC` | `ev_45faa002457bc9d6542a6c471448b34b4e3f75b51454ccc7eb94638826107cec` |
| LED 字格范围 | `0045127C` | `ev_c26e4eda2900ddccc2a673b5fe10a364169bdf5c6d609d506e632f84fa2ae8e5` |
| LED 更新时间 | `00451416` | `ev_4373a99829b4b8c993fe69ef8f5def5603eac572efb42f3120c79bb6757e2bde` |
| 主 UI 250ms | `0045FAD8` | `ev_a844052bf702978d41f66dd05c4ee6c4643fe31339c1d6650e53068300e910e3` |
| slider bar/fill | `00451E07` | `ev_d32b9cb3a917ae0aaca179b79be97e309f74cec50161ca8ed38f524105c010cd` |
| progress 分层 | `0045287B` | `ev_62e318a084eeef94a18694c912987b863d564b65bb0aa6d5f131c3d589fcf765` |
| disabled thumb | `00451F59` | `ev_a75780efd5c94596075479c0fe658e4e7de948599c7c06f7d3e7c0263823ac12` |
| 按钮直接透明绘制 | `0040A35B` | `ev_4bfbbceeb1b1577b7f749b296a9029ab2f741ddfad53cc1fa33b70bbd7db22a2` |
| 按钮擦除不画背景 | `00426434` | `ev_e6015a03dfa5f18a6079c32dd9d2bdc043e01bcfb8e57ea4468372f0d4889fdc` |
| 列表初始两轴尺寸 | `0046BCBE` 汇编 | `ev_4c58d9352996a90e93dbbc75cfb24753dddd04c58ce8b4ed7e66c699f74483a2` |
| 选择背景 | `00487C0D` | `ev_3817635a2ab43c852672aeab7cb161d8457932cd602bad7cf820be6f1f76b071` |
| 左栏选择背景 | `004897C1` | `ev_b4e17ec5d5ae6a02b7fdb5cef3da7b301da567496232c2ae110f3071a8b257e3` |
| 图片分隔条 | `0041C091` | `ev_f62c9cc834a1f40fb9972c96f9deb547a726731d3a1b77ab4e2369bdd30ac991` |
| 分隔箭头 | `0041C356` | `ev_799694f5463f9c96694da93d780dd8488f8368c86a22aa85a77c2482a9cbf7c7` |
| 分隔折叠/恢复 | `0041C941` 汇编 | `ev_58d484eb9f00d0259760d4ae5ce94f0e5728dde46ba5ac3eec994510454e4e3d` |
| scrollbar 各 part 绘制 | `00488463` | `ev_03e19d5eca8d323273d86b958aa73b7902f2e53d019ccb67de5327f214c071cb` |
| EQ title | `0042980D` | `ev_cb70b6278b97ddc82034ef8dc069c51c707be863b6153ac522fd70b63b2d93ee` |
| DIB opaque copy | `00408E2B` | `ev_79722a71141105be1b0ccaee1f4b6a5b67de16ca35b5fe61e6708115a7dbaa75` |
| 固定背景 region | `0046D0C1` | `ev_6646b0c9f18a3f670ed58cd1ad8f706b2bf44e4bb06edcc7495572b8d545ea73` |
| 8 位首索引 region | `0048C9CA` | `ev_0a15b7a9036fa4c52cc72cfc79178a23545859347a39df7052f24dde929ce9e9` |
| 结束标签比较 | `004C7177 / 004C6B86` | `ev_b730bbebbb448e0ff949b41a78ee2dd21505df10e0f3c786261f63ebc19e9090` / `ev_1446fec28e2e5e55f40f63365cc5b3010266f99132becf5b6b86fcc2bba7b76d` |
| 属性词法 | `004C76CB` | `ev_453446406fa0df8689db4f4661e9c1d8a382f449f3ffcdeadd1c74ddc3cb97ac` |
| visual 直接 DC | `00457B11` | `ev_9db801cdd3e68f8ac916e4504aa4c25c7b6190ff03c249d8d6495ced1185fa2e` |
| 桌面播放切图 | `004188FB` | `ev_c30c368fc31d31ed87b50f5126f7e325aa7938af6e8cad92efa7a73f1b1bba76` |
| 键盘步长使用 | `00452413` 汇编 | `ev_cbccb1dd9f5bc599b8a30420e453910b1d09895d974d9f01c926e09044aa3eb7` |

若上表手工转录与 bundle 有出入，以 `rea-index.json` 的完整 ID/参数及 bundle 为准；报告检查脚本会核对 Evidence ID 是否确实存在。

## 11. 运行附件与复现边界

取证目录：[skin_render_chain_audit_20261118](../artifact/skin_render_chain_audit_20261118/)

| 子目录/文件 | 内容 |
|---|---|
| `long-led/` | 长时间/剩余模式截图、运行元数据 |
| `bar-origin/` | 坐标编码 bar、合成包、预期/实测 RGB |
| `boundaries/bmp-hole/` | hover 前后真实屏幕图 |
| `boundaries/cold-playlist/` | 冷启动窗口/子控件元数据 |
| `compatibility/disabled-thumb/` | 无媒体启动图、thumb 裁剪、逐帧非键色比较 |
| `compatibility/duplicate-palette/` | 8 位首索引轮廓探针 |
| `compatibility/null-background-region/` | NULL/空背景回退探针 |
| `compatibility/closing-case/` | 结束标签大小写探针 |
| `compatibility/unquoted-entity/` | 两个不同文件名/颜色探针 |
| `auxiliary/` | 归一化窗口尺寸后的 title、selected、splitter、arrow 对照 |
| `package-member-identity.json` | 两版合成包每个成员的内容哈希及一致性结论 |
| `source-identity.json` | 当前源文件哈希及两个仓库 HEAD |
| `manifest.json` | 全部归档文件的 SHA-256/大小 |
| `validate_evidence.py` | 只读检查哈希、Evidence 引用、包成员一致性及关键运行观测；不是播放器功能单元测试 |
| `ttplayer_skin_*_probe_20261118.py` | 各运行探针；复用前轮已归档 `probe.py` 的隔离运行函数 |

两个合成 ZIP 的原始 SHA 有时因写入时间不同而不同；已逐成员核验**名字、长度、内容哈希相同**，不是用不同素材比较两个程序。

没有保留 57.6MB 长音频或各次运行的 EXE/DLL 副本；脚本可以重新生成音频，运行元数据保存了实际二进制身份。截图/矩形/像素结果属于本轮运行观测；REA 伪代码属于静态证据，两者没有混称。

### 最终判断

当前最值得做的是**修正有限的几何、帧选择和图片映射规则，并为历史像素留存补专门状态测试**，而不是推倒重写皮肤框架。对于 visual 和合批局部重绘，应先建立可控消息序列的运行证据，再决定如何调整合成策略。
