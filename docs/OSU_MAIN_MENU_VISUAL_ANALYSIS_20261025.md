# osu! 主菜单视觉实现分析与 TTPlayer 接入方案

日期：2026-10-25。

## 1. 结论与范围

截图中的主体可以用一个轻量的 **2D 场景**实现：背景、圆形徽标、环形频谱、扩散环、双侧星星和边缘闪光，分别接受音频、谱面时间线和鼠标输入，最后按固定层序合成。

对当前同时支持 XP、Win7 和新 Windows 的 TTPlayer，推荐：

**现有音频引擎 + 连续音频分析窗口 + 可选 `.osu` 时间线 + 独立场景模型 + D3D9 批量绘制；小主窗及 HDC 接口保留 CPU 渲染路径。**

关键发现：

1. 中心圆的粉色、三角花纹、白圈、文字和阴影已经包含在 PNG 中；主要动画是贴图变换。
2. 环形频谱是 2048 点 FFT 的 1024 个幅值，驱动 1024 个纹理条，角度绕行四圈，频率索引继续循环位移。
3. 双侧喷星主要由谱面 Kiai 段落的进入事件触发；左右闪光由谱面拍点、拍号及左右声道峰值共同控制。
4. Logo 脉冲综合谱面相位、音频瞬态和鼠标悬停；单纯用音量缩放圆形不能得到相同效果。
5. 背景与前景的鼠标视差幅度不同，前景约为背景的三倍。
6. 现有 TTPlayer 的 512 帧可视化快照和 OSZ 音频/背景关联还不足以准确实现这些行为。

本次完成静态二进制分析、资源检查与接入设计，没有实现播放器新效果，也没有启动 osu! 做动态或逐帧对照。下文的“已确认”指指定文件中的静态证据；工程方案单独标为建议。

## 2. 分析对象与证据边界

### 2.1 本地文件身份

目标：`C:\Users\Sonic853\AppData\Local\osu!\osu!.exe`。

| 项目 | 结果 |
|---|---|
| 大小 | 4,541,216 字节 |
| SHA-256 | `3adc2d001aacced5f92ba8ae24121980d5e23f5653f37d14cf4765bca1abd9f9` |
| PE / CLR | x86、IL-only、32-bit-required；.NET Framework 4.0 |
| 文件版本 | `1.3.3.8`；不能据此推出实际 osu! 更新日期 |
| 程序集版本 | `0.0.0.0` |
| MVID | `aa9db4d0-6aa6-4bb7-2f5e-8c1ae12ad43a` |
| 元数据规模 | 2,907 个类型、21,398 个方法；大量名称混淆 |
| 主要绘图引用 | OpenTK `1.1.0.0`，自定义 Sprite / 顶点批处理 |

同时检查了同目录资源程序集：

| 文件 | SHA-256 |
|---|---|
| `osu!ui.dll` | `22e041674931161e2c2c15241fb6c5f3125f8d57e4660ef7c0b7fb64449253c7` |
| `osu!seasonal.dll` | `27decc7e0c959078325eb800b04626d6b9498b33cf118850c0e7b78dea470500` |
| `osu!gameplay.dll` | `ba3467a8db908d81a0729f78fdc5c8f1d1595d3da4e5a9a34be9a16e06da9f87` |

分析针对本地 classic/stable 程序，不以公开的 osu!lazer 代码替代该 EXE 的证据。

### 2.2 工具与覆盖

- 使用 `reverse-engineer-anything` 技能。REA MCP 未注册，使用 `rea-agents@6.5.0` CLI 完成元数据、选定类型成员及调用关系检查。
- C# 和关键 IL 使用本地 dnSpyEx 6.6.0 静态导出。REA 元数据检查本身没有反编译 C#，二者的证据来源分开记录。
- 全程序集成员展开触发 64 MiB 表示预算限制，之后改为选定主菜单、背景、FFT、频谱及批处理类型检查；选定范围完整，不能据此声称所有程序逻辑均已审计。
- 主 EXE 的批量导出因一个特殊资源输出路径报错退出；相关 C# 文件已写出并核查，资源 DLL 则另行成功导出。没有把非零退出码当成完全成功。
- PNG 从资源导出数据中提取原始 PNG 字节并核对尺寸/摘要，没有执行目标程序集或反序列化对象来取图。
- 没有读取用户账号配置、登录信息或网络会话。

私有证据目录：

`D:\Projects\Backup\TTPlayer\rebuild\tests\osu_menu_analysis_20261025`

包含 REA JSON、C#/IL 导出、资源清单、原始 PNG、`notes-menu.md` 和 `integration-notes.md`。这些资料放在本地 tests 中，不加入播放器发行包或 Action 测试。

### 2.3 可复查的方法入口

以下 token 均属于本节指定 SHA-256 的 EXE，名称别名为本报告解释。

| 别名 | 类型 / 方法 token | 证据用途 |
|---|---|---|
| MainMenu | 类型 `0x020003A9`；初始化 `0x06001B71` | Logo、边缘闪光、粒子资源和容器 |
| MainMenu.Draw | `0x06001B8C` | 各容器及频谱的实际绘制顺序 |
| MainMenu.Shockwave | `0x06001B8D` | 每拍扩散环 |
| MainMenu.Update | `0x06001B8E` | 电平、Logo、闪光、Kiai 喷星及前景视差 |
| MainMenu.Idle | `0x06001B8F`、`0x06001B90` | 空闲居中、菜单展开和透明度 |
| Background.Update | `0x06000B84` | 背景视差、放大和暗度 |
| FFT.Update | `0x06002961` | BASS FFT 调用、增益及静音衰减 |
| Ring.Init / Radius | `0x06001289` / `0x06001288` | 1024 条初始化、角度、位置 |
| Ring.Step / Bars | `0x0600128D` / `0x0600128E` | 时间分片、频率位移、长度及 alpha |
| Ring.Colour | `0x0600128F`、`0x06001291` | 颜色选择和过渡 |
| Audio.Kiai / Beat | `0x0600493A` / `0x0600495B` | 当前 Kiai 和拍号变化 |
| TimingPoint.Kiai | `0x06002320` | `(effects & 1) != 0` |
| Particle.Add / Update | `0x060009E4` / `0x060009E5` | 发射初速度和重力积分 |
| Sprite.Blend | `0x06000A90`、`0x06000A91` | 普通 alpha 与加色混合 |
| VertexBuffer.Draw | `0x06002DC9` | OpenTK ES20 顶点缓存及索引绘制 |
| CircularHitTest | `0x06000747` | Logo 的圆形命中 |
| LogicalScale | `0x060011B1` | UI 比例为有效高度 / 480 |

## 3. 实际绘制架构

### 3.1 原程序使用什么

窗口宿主涉及 WinForms，但这些主菜单效果由自定义 Sprite、纹理、顶点缓冲和 OpenTK 图形调用完成。代码中可以追到 `BufferData`、`BufferSubData`、`DrawElements` 和混合状态切换。

图形上下文包含默认 OpenGL 与 ANGLE/D3D9/D3D11 选择、失败降级分支。这里只能证明存在这些路径，不能凭安装目录的 `libEGL.dll`、`libGLESv2.dll` 判断截图当时实际选用了哪条路径。

反编译中出现 `Microsoft.Xna.Framework` 命名空间，不等于主菜单由外部 XNA 游戏运行库绘制；本目标的实际调用链应以 OpenTK 和本地 Sprite 实现为准。

混合方式：

```text
普通 alpha: C_out = C_src * A_src + C_dst * (1 - A_src)
加色混合:  C_out = C_src * A_src + C_dst
```

环形频谱、双侧喷星、侧闪和每拍扩散环走加色路径。截图中亮白、高亮叠加的效果由贴图及混合即可形成；资源中存在 Bloom shader 并不能证明这张主菜单必须经过全屏 Bloom。

### 3.2 层序决定遮挡

正常主菜单阶段的顺序：

```text
背景 / 背景交叉过渡 / 暗度
  → 效果容器：侧闪 → 扩散环 → 喷星
  → 环形频谱
  → Logo 与菜单容器：主徽标 → overlay/halo → 同容器菜单项
  → 顶部 / 底部 UI
  → 可选下落装饰
  → 基类和全局 UI / 光标
```

容器内部按 depth 升序：侧闪 0.05、扩散环 0.2、喷星 1；主徽标 0.98、overlay 0.981。然而容器调用先后优先于它们内部的 depth：**星星的 depth=1 仍会被随后绘制的 Logo 覆盖**。实现时不能把所有对象放进一个全局 depth 排序。

启动过渡另有分支：在 intro 进度小于 100 时，Logo 容器先于频谱绘制，还可能插入启动遮罩。因此启动画面与稳定主菜单的层序不能混为一谈。

## 4. 资源与中心 Logo

### 4.1 已核查资源

| 包内资源 | 基础尺寸 | 高分辨率尺寸 | 观察结果 |
|---|---:|---:|---|
| `osu!ui/menu-osu` | 540×540 | 1080×1080 | 粉色底、三角花纹、白圈、osu! 文字和阴影已烘焙在图中 |
| `osu!ui/menu-vis` | 115×38 | 230×75 | 白色细长圆端轮廓及短实心部分，可产生截图中空心/实心谱条 |
| `osu!ui/menu-flash` | 310×192 | 620×384 | 有透明度渐变的闪光贴图 |
| `osu!ui/menu-osu-shockwave` | 520×520 | 1040×1040 | 独立扩散环资源 |
| `osu!gameplay/star2` | 33×34 | 66×68 | 带光晕的五角星 |
| `osu!gameplay/star`、`star3` | 46×47、49×47 | 91×92、98×94 | 其它星形候选资源 |

`osu!seasonal.dll` 也有同名菜单资源。具体资源调用参数经过字符串混淆，本次没有解密所有 key 或动态记录资源优先级。因此：

- PNG 内容、尺寸、存在性是直接证据。
- `menu-vis`、`menu-flash` 与截图/对应 Sprite 几何的关系是强匹配；不能把尚未解密的 key 当成已确定绑定。
- 当前截图的星星究竟选 `star`、`star2`、`star3` 或皮肤覆盖，仍需运行时资源记录确定。
- 中心默认纹理已经有三角形；没有证据要求再用程序逐帧生成三角网格。

### 4.2 Logo 脉冲公式

主菜单读取 `BASS_ChannelGetLevel` 的左右声道峰值，记为 `L`、`R`；它们通常处于 0..32768。该 API 使用约 20ms 音频窗口，得到的是声道峰值，不是 RMS；通道音量不直接包含在返回值中。[BASS 官方说明](https://www.un4seen.com/doc/bass/BASS_ChannelGetLevel.html)

令 `D = frameDeltaMs / 16.6666667`。主菜单部分分析逻辑由约 60Hz 的独立标志推进：

```text
A  = (L + R) / 65536 * musicVolumePercent / 100
As = 0.9 * As + 0.1 * A

phase = 当前拍内的进度（0..1）
target = clamp(0.5 - 0.5 * phase + (A - As), 0, 1)
q = 0.5 ^ D
u = previousU * q + target * (1 - q)

hover 时 h += 0.012 * D，否则 h -= 0.012 * D；限制在 0..0.1
easeOutQuad(t,a,b) = a + (b-a) * (2*t-t*t)

logoScale = easeOutQuad(u, 1.05+h, 1+h)
haloScale = easeOutQuad(u, 1.05+h, 1.08+h)
```

有效谱面时相位来自音频时间、timing offset 和 beatLength；原代码在该分支存在整数化，应在严格对照时保留。停止状态使用 1000ms 自由运行周期，不能把停止与暂停一概处理成同一行为。

伴随 halo 的透明度：

```text
strength = Kiai ? 1 : 0.6 + 0.4 * clamp((L+R-30000)/35536,0,1)
strengthSmoothed = 0.8*strengthSmoothed + 0.2*strength
haloAlpha = easeOutQuad(u, Kiai ? 0.1 : 0.4, 0) * strengthLatchedAtBeat
```

主 Logo 和 halo 共用纹理，后者在 Kiai 且非停止时用加色混合，其余状态走普通 alpha。每次新拍还会创建后方扩散 Sprite：从当前 Logo 比例扩大至其 1.4 倍，1000ms 淡出，初始 alpha 为 `0.1*strengthSmoothed`。

这解释了画面中同时存在的“中心呼吸”“边缘泛光”“向外扩散”，它们不是同一个缩放动画。

### 4.3 命中、空闲与菜单展开

- Logo 命中是圆形距离判断，交互直径参数为 `300*scale*uiScale`；初始化中的 300 不是动画时长。
- 无有效鼠标活动约 6 秒后进入居中简化状态，菜单项淡出。
- Logo/halo 用 2 秒 ease-out 移到横向中心；恢复活动后用 400ms 移回横向偏移 -120 的展开位置。
- UI alpha 在活动时按 `+0.08*D` 增长，空闲时按 `-0.001*D` 降低，保留最低 0.01。
- 居中状态背景目标亮度为 1，展开菜单状态为 0.4。

作为 TTPlayer 的纯视觉效果，建议先实现视觉场景，不把 osu! 的游戏菜单交互一起复制进播放器；鼠标右键、双击和播放器菜单仍应服从播放器自己的交互含义。

## 5. 环形频谱

### 5.1 FFT 来源

调用值为 `0x80000003`，即 `BASS_DATA_FFT2048`，结果数组长 1024；常态主菜单 FFT 增益为 1.6，另有 intro 分支使用 2.0。

该调用没有启用 `NOWINDOW` 或 `INDIVIDUAL`：按 BASS 约定，使用 Hann 窗且声道合成单份频谱；频率 bin 按 `k*sampleRate/2048` 排列。44.1kHz 下一个分析窗约 46.4ms，频率间隔约 21.53Hz。[BASS FFT 官方说明](https://www.un4seen.com/doc/bass/BASS_ChannelGetData.html)

没有有效播放输入时，FFT 缓冲逐渐衰减，小于 0.01 的值清零。条自身还会再执行长度衰减，两级状态不能无条件合并。

### 5.2 角度与频率位移

1024 个 Sprite 初始化公式：

```text
theta[i] = 2*pi * (0.4 + i/1024 * 4), i=0..1023
direction[i] = (cos(theta[i]), sin(theta[i]))
position[i] = centre + radius * direction[i]
rotation[i] = theta[i]
```

因此数学上每隔 256 个条回到同一方向，四组叠加。并非 1024 根各占唯一角度的均匀圆环。

时间推进按累计约 10ms 分片；跨过分片边界时：

```text
shift = (shift + 50) % 1024
```

随之改变的是“频率值映射到哪个条”，不是简单旋转整个圆环或 Logo。实际实现还保留跨帧累计余量；超过 1000ms 的单次时间步直接跳过，避免长卡顿补算。

### 5.3 长度、透明度和半径

每个时间片内，原代码从 `i=1023` 倒序访问，将结果应用到 `bar[1023-i]`：

```text
decay = 0.949999988079071 ^ (dtMs / 16.6666666667)
x = max(previousX, fft[(i+shift)%1024] * 3) * decay

if x < 0.01:
    x=0; alpha=0
else:
    alpha=max(0, globalAlpha * 0.4 * min(1,(x-0.04)/0.08))
```

`x` 是贴图横向缩放，不是像素长度；纵向缩放初始化为 0.5，锚点是 `CentreLeft`。若用普通实心线段替代，会丢失纹理轮廓、重叠亮度和比例。

半径跟随 Logo 状态：

```text
radius = 150 * (1.05+h+0.03*(1-u)^2)
```

intro 的特殊前阶段额外加 8。频谱整体 alpha 还随 Logo 左移而衰减：中心状态系数 1，展开到 x=-120 时约为 0.3；Kiai 乘 1，否则乘 0.7。

颜色路径支持传入颜色及 200ms 过渡，另有条件使用默认 `(128,128,160)`。本次未解密该条件的全部命名，不能据此把所有皮肤、账号或模式分支简化成“永远白色”。

### 5.4 对重建的含义

- 必须新增独立的 2048 点分析，不应改变原 TTPlayer 频谱或已有韵动脉冲/水波荡漾的 512 点算法。
- FFT 归一化需要用正弦、静音和峰值样本对照 BASS 输出，不能只匹配数组长度。
- 严格模式保留 1024 条及四重叠加；性能模式可以降低条数，但应明确效果会变化。
- 1024 条可以合并到少量纹理批次，无需 1024 次绘制 API 调用。

## 6. 双侧喷射星星

### 6.1 触发条件

当前 TimingPoint 检查 `(effects & 1) != 0`，即 **bit 0、掩码 1**。这是 Kiai 标记；其格式含义与官方说明一致。拍长来自非继承 timing point，继承点中的负 beatLength 不能当成 BPM。[osu! TimingPoints 说明](https://osu.ppy.sh/wiki/en/Client/File_formats/osu_%28file_format%29#timing-points)

喷星条件同时要求：

1. Kiai 从 false 进入 true。
2. 尚未喷射过，或距上次喷射超过 500ms。
3. 当前音频时间距对应 timing point 的时间小于 500ms。

所以音量突然增大不一定喷星；直接 seek 到 Kiai 段落中间也不应机械地补喷一轮。

### 6.2 数量与运动

一次创建左右各 100 颗，共 200 颗，分约一秒错峰喷出。采用高为 480 的逻辑坐标系；以下数值为逻辑单位，不是物理像素。

```text
i = 0..99
start = globalNow + i*10ms
end = start + randomInt(300,1300)ms
left.position  = (100,490)
right.position = (logicalWidth-100,490)

m 为随机整数 -1、0、1
left.vx  = m*(i-50)/50*450  + randomInt(-50,50)
right.vx = m*(i-50)/50*(-450) + randomInt(-50,50)
vy = randomInt(-800,-700)
```

随机函数边界应以相应实现为准，上述表示调用参数。每颗星还有：

- scale 从 1 增长到随机 1..2.8；另一全局模式分支将上限改为 2.1。
- alpha 从 1 降至 0。
- rotation 在两个随机 -2..2 弧度值之间插值。
- 上述动画使用各自 start/end 和 ease-out。
- 白色纹理、加色混合。

物理推进：

```text
vy += 8*D*(1+gravityBoost/100)
position += velocity*frameDeltaMs/1000
gravityBoost=80，因此约等于 864 逻辑单位/秒²
```

这构成从屏幕下方上喷再弯落的扇形轨迹。当前 Add 重载的 bool 参数没有赋值到反弹字段；虽然更新器有可选反弹代码，本次不能宣称这里会启用反弹。

最佳实现是固定容量粒子池、CPU 积分、GPU 批量纹理四边形。200 颗星不需要计算着色器或复杂粒子引擎。

## 7. 左右边缘闪光

两个 Sprite 分别放在逻辑屏幕左右边界，纵向中心 240；使用同一渐变纹理，左侧横向镜像，纵向 scale 为 4，additive=true。

当播放未停止、新拍发生、场景淡入完成时：

- Kiai 中：每拍触发，偶数拍左边、奇数拍右边。
- 非 Kiai：正拍号且 `beatIndex % meter == 0` 时，两边同时触发。

左右峰值分别决定目标 alpha：

```text
left  = min(1, max(0,(2*L-20000)/45536)/2)
right = min(1, max(0,(2*R-20000)/45536)/2)
duration = max(300,int(beatLength))
```

清除旧 Fade 动画，从当前 alpha 在 64ms 内线性升至目标，随后在 `duration-64` ms 内用 ease-in 二次曲线淡至 0。典型满幅目标约 0.5。停止时另有 50ms 淡出。

因此它具有左右声道区别、拍号区别和 Kiai 区别。只用一份 mono RMS、每帧画整屏白色遮罩，会同时改变覆盖位置、亮度、时机和衰减。

## 8. 背景视差、暗度与其它装饰

### 8.1 双层视差

当视差选项启用且满足场景条件时：

```text
backgroundOffset = (mousePixels-screenCentrePixels)/uiScale/180
foregroundOffset = (mousePixels-screenCentrePixels)/uiScale/60
backgroundScale = (1.01,1.01)
```

Sprite 绘制时减去容器 offset，因此视觉向鼠标移动的反方向偏移。前景约为背景的三倍，制造层次感。背景额外放大 1%，用于覆盖位移后的边缘；禁用视差则 offset=0、scale=1。

它移动的是应用内背景贴图，不涉及修改 Windows 桌面壁纸，也不需要移动实际窗口。实现时应在当前客户区坐标内计算，不能把多屏桌面坐标直接带入。

暗度逐步接近目标：

```text
overlayAlpha += (1-targetBrightness-overlayAlpha)
                * min(1,D) * (specialMode ? 0.1 : 0.04)
```

差值不超过 0.001 时直接收敛。两份背景容器共享暗度和视差，支撑切图时的过渡。

### 8.2 另一套下落装饰

程序还有独立下落装饰类型 `0x0200038B`，与双侧上喷星星分开：

- 从顶部 y=-50、随机横坐标生成，alpha 随机 0..0.7，scale 0.1..0.6。
- 约 60Hz 检查发射机会，Kiai 下概率为 0.5，普通段落约 0.04..0.2。
- 下落、横向风漂移及旋转，鼠标水平移动影响风量。
- 可在底部堆积并约 60 秒淡出。
- 纹理和启用状态受其它模式/设置控制，绘制在主菜单 UI 后面。

用户截图中的双侧喷泉应按第 6 节恢复，不能用本节随机飘落效果代替。

## 9. TTPlayer 当前能力与缺口

以下行号基于分析时的工作区，完整引用清单保存在私有 `integration-notes.md`。

| 位置 | 现状 | 新效果需要 |
|---|---|---|
| `include/ttplayer/audio/audio_engine.h:118` | 左右各最多 512 个 int16 样本、sample_rate、revision | 连续窗口、音频帧位置/时间戳、timeline revision |
| `src/audio/audio_engine.cpp:3230`、`:3558` | 已按实际输出游标读取可视化 PCM，支持跨输出块 | 从同一输出时间线取得 2048 点窗口及约 20ms 双声道峰值 |
| `src/audio/audio_engine.cpp:4081` | 播放状态/位置、seek revision、seek pending、观测时间 | 将分析结果与该时间线绑定，避免用未听到的解码数据 |
| `src/ui/player_window_visual.cpp:938` | 原频谱调用 ttpcomm ordinal 105 | 保留原行为，增加新效果专用分析器 |
| `src/ui/player_visual_effect.cpp:72` | pulse/ripple 固定 512 FFT 和原衰减规则 | 新场景独立 FFT、包络、拍点信号 |
| `src/ui/player_window_visual.cpp:1527` | jthread 更新，再 GetDC/Paint；主要在播放中推进 | 独立场景时钟，暂停尾迹、鼠标视差仍有明确行为 |
| `src/ui/player_visual_effect.cpp:161`、`:226` | GDI/GDI+、DIB mask、逐像素 CPU 混色 | 全屏采用 GPU 批处理 |
| `src/audio/osz_archive.cpp:79`、`:117` | 读取 AudioFilename、背景，并按音频去重 | 关联对应 `.osu`，补充 timing、meter、Kiai |
| `src/ui/cover_image.cpp:133` | WIC/GDI+ 图像解码及 PBGRA | 复用图片解码，补充渲染器纹理上传 |
| `src/ui/player_window_visual.cpp:1904`、`:2127` | 现有全屏、显示器、歌词叠加布局 | 新 renderer host 与同样的窗口生命周期集成 |
| `include/ttplayer/settings/settings.h:159`、`:183` | type 为 0..6，profile 长度 7 | 扩充枚举、验证、菜单、选项、XML、profile |
| `include/ttplayer/skin/skin_plugin_api.h:81` | 外部皮肤为 HDC / 固定样本接口 | 保持 ABI；CPU 后端向旧接口输出 |

### 9.1 不能拼接“最近四帧可视化数据”

44.1kHz 下 512 个样本仅约 11.6ms，而 25FPS 两帧相隔约 40ms。最新快照之间可能跳过音频，也可能因输出更新节奏重复。把四次快照拼成 2048 点 FFT 会制造不连续数据，导致假高频和错误瞬态。

应建立带采样位置的分析环形缓冲，或从已有输出队列查询连续窗口。音频线程仅复制有限数据/发布索引，FFT 和场景计算在非实时线程执行。

还要区分 post-DSP、pre-volume 与 post-volume：原 Logo 公式再次乘音乐音量，如果 TTPlayer 提供的 PCM 已包含该增益，就不能再乘一次。多声道也不能随意只取前两个声道便声称与 BASS 相同。需要定义分析信号位置并做音量、平衡、多声道对照。

### 9.2 OSZ 中同一音频不一定只有一条时间线

同一个音频可关联多个难度谱面，甚至不同 Kiai/timing。当前按音频去重适合播放列表，但新增视觉数据不能在去重时把所有谱面来源一起丢掉。

建议保留候选 `.osu` 的稳定标识与时间线，默认选择规则明确、可复现，必要时允许选基准谱面。文件枚举先后不应决定每次喷星时刻。

## 10. 最佳实现路线比较

| 路线 | 优点 | 限制 | 建议 |
|---|---|---|---|
| 继续全屏 GDI/GDI+ | 能复用现有 HDC；兼容旧皮肤 | 全屏旋转贴图、加色和背景混色的 CPU/内存开销较大 | 用于小区域与兼容路径 |
| D3D9 批量 2D | 覆盖 XP 目标；所需几何/混合足够简单；便于纯 C++ 集成 | 要处理设备丢失、窗口/线程和纹理恢复 | 当前首选 GPU 后端 |
| OpenGL | 与原程序绘图接口更近 | 本项目暂无后端；旧 Windows/虚拟机驱动能力需重新验证 | 没有必要仅为接口相同优先选用 |
| D3D11 | 适合后续现代系统扩展 | 无法单独覆盖 XP；仍需第二条旧系统路径 | 未来按需求增加 |
| 嵌入 .NET/OpenTK/BASS 播放器 | 可复用另一套技术栈 | 增加依赖、窗口及音频同步复杂度 | 本项目不推荐 |

D3D9 核心属于 XP SP2 等系统的 DirectX 组件；D3DX 是另一个可选组件。可用 Windows SDK 的 D3D9 接口，加现有图片解码器创建纹理，避免为了图片加载引入旧 D3DX 运行库。[Microsoft DirectX 组件说明](https://learn.microsoft.com/en-us/windows/win32/dxtecharts/directx-setup-for-game-developers)

这是基于 TTPlayer 的兼容目标作出的工程判断，不是宣称原 osu! 必须使用 D3D9。

### 10.1 分层接口

```text
实际输出 PCM / 音频时钟     可选 .osu timing      鼠标 / 设置
             ↓                  ↓                  ↓
        AudioVisualAnalysis + BeatTimeline + 场景输入快照
                              ↓
          BeatScene：固定层序、脉冲、粒子、视差、过渡
                              ↓
             有界的 Sprite / Quad 绘制列表
                              ↓
              D3D9 全屏后端 / CPU 小区域后端
```

建议模块职责：

- `AudioVisualAnalysis`：2048 FFT、双声道峰值、时间戳和幅度标定。
- `BeatTimeline`：红线 BPM/offset/meter、各 timing point 的 Kiai 状态、seek 定位。
- `BeatScene`：纯数据状态更新，不依赖 HWND、HDC 或 D3D。
- `SpriteRenderer`：普通 alpha / additive、纹理缓存、矩形裁剪、同层批处理。
- `VisualHost`：窗口、呈现线程、尺寸/显示器变化、设备恢复。

模块名为建议，尚未创建这些生产代码。

### 10.2 精确时间线与普通音乐模式

**有 `.osu`：** 以音频实际播放位置查找时间线，按原拍长和 Kiai 驱动；seek 时定位当前状态，只在满足进入条件时发射。

**没有 `.osu`：** FFT、左右峰值、鼠标视差仍可直接实现；拍点可用节拍分析或用户 BPM/offset，高潮可由谱通量/能量变化估计。这属于音乐响应模式，无法保证与某个人工谱面的 Kiai 时间完全一致。

两种模式应共享渲染器，时间线来源不同。Kiai 是谱面作者标记，不应仅因为某段音乐很响就声称已恢复出原标记。

### 10.3 FPS、线程与性能

- FPS 可以调整。逻辑时间、音频时间和绘制帧率分开；60Hz 的平滑/随机逻辑可以固定步进，渲染按用户 FPS 插值。
- 严格模式保留原 10ms 频谱推进及 60Hz 部分逻辑；优化模式改成时间常数时要单独做一致性比较。
- 使用固定粒子池、预分配顶点缓存、纹理图集或同纹理批次。不要为每颗星创建窗口、每帧解包图片或重新分配纹理。
- 同层允许合批，不能为了合批跨越第 3 节规定的遮挡顺序。
- 图片异步解包/解码，GPU 上传由渲染所属线程完成。
- 明确纹理的 alpha 约定：现有解码器提供 PBGRA（颜色已乘透明度），不能再直接套用 straight-alpha 的 `SrcAlpha` 混合，否则会重复乘 alpha、让光晕变暗。应转换成直通颜色后匹配原混合式，或让预乘纹理使用对应的 `One` 源因子；普通与加色路径都需验证。
- 主窗小区域和旧皮肤 HDC 回调使用 CPU 后端；避免每帧从 GPU 读回像素再给 GDI。
- 新全屏采用无边框窗口模式，适配歌词叠加、Alt+Tab 和多显示器。

现有 `GetDC` 工作线程不能直接替换成随意跨线程 D3D 调用。创建、Reset、TestCooperativeLevel 及最终 Release 应在相应窗口消息线程完成。建议 VisualHost 拥有 HWND、消息循环和唯一 GPU 线程；其它线程只发布不可变数据。[Microsoft D3D9 线程约束](https://learn.microsoft.com/en-us/windows/win32/direct3d9/multithreading-issues)

隐藏/最小化时停止无意义呈现；恢复时限制时间补偿，避免喷射积压。显卡设备丢失时保持音频运行，重建纹理及 backbuffer；必要时使用 CPU 路径。[Microsoft 设备丢失说明](https://learn.microsoft.com/en-us/windows/win32/direct3d9/lost-devices)

性能不能只按粒子数量估计：1080p 单张 32bpp 图约 7.9MiB，60FPS 仅一次全表访问就约 475MiB/s；多次 CPU 清空、混色、复制会继续增加流量。这是数据量估算，尚非该机器上的性能实测。

## 11. 建议实施顺序与验收

### 第一步：固定基准

记录本报告的 EXE/资源摘要，选固定音频和 `.osu`，确定实际资源、背景来源、启用设置及图形后端。用同一音频位置而非“启动后几秒”对齐画面。

### 第二步：连续音频分析与谱面时间线

补齐带播放位置的连续 PCM，验证 FFT 幅度和双声道峰值；为 OSZ 保留 timing/meter/Kiai 关联，覆盖多难度、变 BPM、继承点、负 offset、seek 和暂停。

### 第三步：独立场景及小画面基准

先恢复 Logo、频谱、双喷星、侧闪、背景，再加入空闲/过渡等外围行为。固定随机种子和时间，验证层序、曲线、发射数量、重力及 FPS 独立性。

### 第四步：D3D9 与播放器集成

接入批量绘制、透明与加色、资源恢复；增加视觉枚举/选项/持久化/全屏 profile。主窗小区域和旧皮肤接口走兼容后端，保留现有视觉行为。

### 第五步：动态对照与系统测试

| 维度 | 必测情况 |
|---|---|
| 原程序对照 | 同音频、同谱面、同时间；普通拍/Kiai 入口/段中/退出；左右单声道输入 |
| Logo | 鼠标进入/离开圆形、空闲居中/展开、暂停/停止、不同音量 |
| 频谱 | 正弦/静音/脉冲、多采样率、BASS 幅度标定、四重叠加及方向 |
| 星星 | 每次左右各 100，10ms 错峰，seek 至段中不误喷，长卡顿不无限补发 |
| 图像 | 4:3、16:9、16:10、超宽；高 DPI；背景视差边缘、普通与加色混合 |
| 窗口 | 主窗/全屏、歌词覆盖、菜单模态、负坐标、混合 DPI、多屏切换 |
| 兼容 | XP、Win7、Windows 11；无 GPU、设备丢失、锁屏恢复、CPU 路径 |
| 性能 | 25/30/60/高 FPS 的时序一致性、4K、长期运行、纹理/粒子资源上限 |

这些是后续验收项目，本次未执行，也未据此标注任何系统“通过”。测试代码和素材继续留在 `rebuild/tests`，不让 Action 执行本地对照测试。

## 12. 尚未确认的事项

1. 当前运行设置实际选择的 OpenGL/ANGLE 后端。
2. 混淆资源 key 的全部明文、皮肤/季节包覆盖顺序及截图星星的精确资源。
3. 截图当时歌曲对应的谱面、音频位置、Kiai 标记和随机状态。
4. BASS FFT/峰值与 TTPlayer 现有 PCM 信号位置、DSP 和增益的数值等价性。
5. CPU/GPU 采样过滤和混色舍入造成的像素差异，以及实际运行性能。

以上不影响确认总体实现路线，但在完成动态对照前，不能承诺已经做到逐像素一致或所有系统等速运行。

## 附录：REA 证据索引

| 文件 | evidence_id | 覆盖 |
|---|---|---|
| `exe-rea.json` | `ev_6daa0b55f9afb1d6d107a1a8f888c601ee6bce73e4ee46721aee46a10c853721` | EXE 静态元数据 |
| `ui-rea.json` | `ev_0720196b3ff14838ef7205484c3ed5c6bf5eb7927eedc3388653151266c3d847` | UI 资源程序集 |
| `seasonal-rea.json` | `ev_5a936865586de139f84676a7673966cc4e3dba999e8aa55f6c68d2480677e24f` | 季节资源程序集 |
| `ring-members-rea.json` | `ev_103a178a9170485a1d7c077d96724462ced7c4dea61eeca168ac7d70f7efe386` | 环形频谱选定类型 |
| `visual-members-rea.json` | `ev_d8f1e93551bc38c46a08c6a3bc869775cafeec87d13e9e1bd97afd153489c527` | 主菜单/背景/FFT/批处理选定类型 |

C# 算法摘录属于 dnSpyEx 导出证据；REA IDs 用于文件身份及选定成员关系核查。具体纹理尺寸、PNG 摘要在 `resource-selected.json`；关键环形频谱 IL 在 `ring-init.il`、`ring-step.il`。
