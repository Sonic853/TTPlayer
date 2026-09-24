# WaveOut 切换输出后音量异常变小：原版分析与修复

日期：2026-09-24。原版证据来自 `reverse/decompiled/TTPlayer.exe.pseudo.c`；
下列地址均为原版映像地址。WASAPI 是重建版新增输出，不属于原版四种输出。

## 1. 根因及实测

问题发生在 **WaveOut 输出对象的释放阶段**：重建版曾调用
`waveOutSetVolume` 应用播放器音量、左右平衡和淡入淡出，却没有在打开时读取
旧音量，也没有在关闭时恢复旧音量。

本机实际 API 测量，而非仅依据代码推测：

| 状态 | WaveOut 左右声道 DWORD | Core Audio 应用会话音量 |
| --- | --- | --- |
| 测试基准，播放器开始前 | `0xC000A000` | `0.750011` |
| WaveOut 播放，播放器音量 18%、平衡向右 35% | `0x2E141DF3` | `0.179995` |
| 修复前 Stop 后 | **仍为 `0x2E141DF3`** | 仍受低增益影响 |
| 修复后 Stop 后 | **恢复 `0xC000A000`** | 恢复原会话音量 |

低字是左声道，高字是右声道。测试特意使用不同的左右初值，避免只恢复主音量
而漏掉平衡的问题被掩盖。测试完成及异常退出时均恢复测试前的音量。

在本机和 Win7，这个 WaveOut 增益会反映到同进程的音频会话；后续 DirectSound
或 WASAPI 共享流还会再应用播放器自己的缓冲区／PCM 增益，导致叠加衰减。
例如原会话为 100%、播放器设为 20%，遗留 20% 会话增益后再乘 20% 的输出增益，
得到约 4%，明显小于期望的 20%。停止淡出、静音、平衡也会留下相应的状态。

不能把所有系统都描述为“修改了系统总音量”：现代系统的实测是应用会话层，
XP／旧驱动可能使用共享的 Wave 通道。影响范围取决于系统和驱动；独占、KS、
ASIO 也不保证经过同一混音路径。共同缺陷都是 WaveOut 没有清理自身的音量修改。

## 2. 原版 WaveOut 对象的完整生命周期

| 原版函数 | 实际行为 | 修复前重建版 |
| --- | --- | --- |
| `004E2414` | 构造输出对象；`+0x08` 句柄置空、`+0x0C` 原音量初始化为 `0xFFFFFFFF` | 无对应音量快照 |
| `004E24B6` | `waveOutOpen` 成功后调用初始化函数 | 可以打开设备 |
| `004E2585` | 把句柄保存到 `+0x08`，调用 `waveOutGetVolume` 保存到 `+0x0C`；读取失败则使用 `0xFFFFFFFF`；暂停／重置队列 | 缺少读取与保存 |
| `004E263A` | 保存播放器音量，以 `volume * 65535 / 100` 得到左右增益，根据平衡衰减一侧，再调用 `waveOutSetVolume` | 已应用音量、平衡和淡入淡出，但没有对应恢复 |
| `004E2471` | `waveOutSetVolume(handle, saved)`，再 reset、释放已准备缓冲、close | 只有 reset／unprepare／close |

关键的原版语义相当于：

```cpp
open(handle);
saved = waveOutGetVolume(handle);   // 失败时原版使用 0xFFFFFFFF
apply_player_volume_and_balance(handle);
// ... 播放、淡入淡出 ...
waveOutSetVolume(handle, saved);    // 对象销毁时必须配对恢复
reset_and_close(handle);
```

`saved` 是本次打开前的外部状态，不是播放器配置中的音量百分比，不是最后一次
淡出值，也不能在销毁时通过 `waveOutGetVolume` 临时读取——那时读到的已经是
播放器修改过的值。

## 3. 原版输出选择和切换

### 设备标识与创建

`004918B1 / 004918FE` 解释原版 16 字节设备键：尾部为零时，高 16 位决定
后端、低 16 位为序号；非零尾部按 DirectSound GUID 解释。
`004AB1C4` 在准备输出 PCM 格式、位深和缓冲后分派：

| 编号 | 输出 | 原版创建入口 | 音量所在层 |
| --- | --- | --- | --- |
| 0 | WaveOut | `004E24B6` | `waveOutSetVolume`，需保存／恢复外部状态 |
| 1 | DirectSound | `004C3170` | DirectSound 缓冲区音量／平衡；销毁时释放缓冲区及设备对象（`004C3127`） |
| 2 | Kernel Streaming | `004E0502` | 原版 `004E061B` 尝试设备拓扑音量节点，依赖驱动能力 |
| 3 | ASIO | `004E1450` | 原版通用音量／平衡槽是未实现返回，不应擅自把 WaveOut 控制写到 ASIO |

重建版 KS 的软件音量和 ASIO 的原样 PCM 行为详见 `NATIVE_OUTPUT_RECOVERY.md`。
新加的 WASAPI 在提交 PCM 时应用播放器增益，没有通过修改系统总音量来实现。
因此不能通过“给所有新输出强制设置系统音量 100%”修复本问题。

### 选项修改与重新播放

- `0046228D` 处理选项通知；`mask & 0x400` 设置主窗口 `+0x4364` 标志。
- `0045BF4B` 在继续播放路径检查这个标志和已有播放项；需要重建时清除标志，
  发送 `0x7EB` 重新播放请求，而不是简单把现有输出对象改成另一种类型。
- 创建新播放对象的路径会清理旧播放对象并清除重建标志。
- WaveOut 的输出类型查询为 0，原版不进入 DirectSound 类型 1 的双对象交叉淡出。
  重建版按用户先前要求增加了 WaveOut 串行淡出再打开下一首，仍必须先释放旧句柄。

重建版已有的即时输出切换是扩展行为：`ApplyOptionsChangeMask` 保存位置和暂停
状态，应用选项，通过 `PlayCurrent → AudioEngine::Play → Stop/Reap` 关闭旧输出，
再创建新输出，最后 `RestoreAfterOutputRestart` 恢复进度／暂停。
本次实测该顺序能保持状态，未把原版的延后生效行为强加回这个已有功能。
真正缺失的是旧 WaveOut 输出关闭前的音量恢复。

## 4. 本次修复

1. WaveOut 成功打开后、第一次 `ApplyVolumeLocked` 之前，读取并保存完整的
   左右声道 DWORD；快照只属于当前工作线程的当前句柄。
2. 在该输出统一退出路径恢复快照，覆盖立即停止、停止淡出、自然结束、
   解码／播放失败和对象析构。
3. 重建版先停止并清空队列，再在 `mutex_` 下撤销公开的 `device_`，随后恢复
   音量并关闭句柄。这样既避免恢复音量时放大尚未播放的缓存，也避免 UI 音量
   或淡出线程再次覆盖恢复值。这与原版恢复后 reset 的调用次序略有不同，
   保留了原版“退出后恢复打开前音量”的语义，并适配重建版的独立淡出线程。
4. 读取失败时保留原版 `0xFFFFFFFF` 回退；读取／恢复失败写入诊断。
5. 不修改播放器保存的音量，不把外部的非满音量强制设为 100%，不修改其它输出
   的音量接口或驱动配置。

注意：旧版本若已把某个会话音量持久化为低值，之前正确的值没有被保存，无法
可靠地自动猜回。升级后若新开程序仍然偏小，可在 Windows 音量混合器中将该
程序的音量恢复一次；之后此次修复会保留并恢复这一基准。

## 5. 验证

本地专用测试在 `tests/audio/output_switch_volume_tests.cpp`，不上传，Actions
继续使用 `BUILD_TESTING=OFF`。测试使用静音 PCM，但直接读取真实设备左右音量
和 Core Audio 会话音量，不以“计时器继续运行”作为音量正确的证据。

| 环境 | 检查 | 结果 |
| --- | --- | --- |
| Windows 11 普通版核心，修复前 | 保存基准后播放低音量 WaveOut，再停止 | 可重复失败：`0xC000A000 → 0x2E141DF3` 未恢复 |
| Windows 11 普通版核心，修复后 | 84 项 | 全部通过 |
| VirtualBox Win7 SP1，兼容版核心 | 84 项 | 全部通过；驱动量化后的 `0xBFFF9FFF` 被准确恢复 |
| VirtualBox XP，兼容版核心 | 44 项 | 全部通过；不要求 XP 提供 WASAPI |
| 现有回归 | `track_change_fade_tests`、`progress_seek_tests` | 全部通过，18.41 秒 |
| 兼容版导入检查 | 19 个 DLL、656 个导入，子系统 5.01 | XP／Win7 检查通过；新增导入为 XP 已有的 `waveOutGetVolume` |

覆盖立即停止、停止淡出、空流失败、自然曲尾、析构、WaveOut 切到 DirectSound、
WASAPI 共享、WASAPI 独占，以及每一种切换在播放／暂停状态下的位置恢复。
XP 验证 WaveOut 到 DirectSound。KS 和物理 ASIO 没有在这次虚拟机中做实际
驱动播放验证；其前置 WaveOut 清理路径与已测路径相同。

## 6. API 对照

微软同样要求音频应用在修改 WaveOut 音量前读取旧值，并尽早恢复：
[Changing the Volume of Waveform-Audio Playback](https://learn.microsoft.com/en-us/windows/win32/multimedia/changing-the-volume-of-waveform-audio-playback)。
左右 DWORD 编码及设备能力见
[waveOutSetVolume](https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/nf-mmeapi-waveoutsetvolume)。
