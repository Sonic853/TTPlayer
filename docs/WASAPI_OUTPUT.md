# WASAPI 输出（2026-09-24）

## 使用方式

在「千千选项 → 音频设备 → 输出设备」选择：

- `WASAPI (共享) - …`：允许其它程序同时播放；使用系统的采样率转换和声道适配。
- `WASAPI (独占) - …`：独占指定设备；采样率和声道必须由设备支持。

输出设备列表先列出全部 WASAPI 共享设备，再列出全部 WASAPI 独占设备。
各组保留设备枚举顺序，已保存的设备选择仍按设备标识恢复。

每种模式提供系统默认多媒体设备和各个已连接的播放设备。固定设备使用
MMDevice 的持久标识保存，不受枚举顺序变化影响；默认设备在每次打开播放流时
重新解析。设备断开后保留选择并报告错误，不会自动切到其它输出。

普通版及兼容版在 Windows 7 和更新系统均提供该输出。XP 不提供 WASAPI，
设备列表中不出现相关条目，原有 WaveOut / DirectSound / KS / ASIO 保留。
若把含 WASAPI 配置的目录复制到 XP，播放时会提示更换输出，程序仍能启动。

独占模式不支持的格式不会擅自改用共享模式。可以手动启用「采样率转换」，
选择声卡支持的采样率，或改用共享模式。系统禁止独占、设备被占用或已断开时，
保留 HRESULT 并给出相应错误说明。

## 实现边界

这是一项重建版新增功能，并非原版伪代码中的第五种输出。

- `OutputBackend` 保留原版编号 0–3，新增 4（共享）、5（独占）。
- `DeviceType` 使用 `wasapi:shared:` / `wasapi:exclusive:` 加设备 ID；
  `default` 表示系统默认设备。原版 GUID 配置解析不变。
- `WasapiSink` 通过 COM 激活 MMDevice、`IAudioClient`、`IAudioRenderClient`。
  所有 COM 对象和 PCM 队列只在音频工作线程使用和释放。
- 共享模式使用 `AUTOCONVERTPCM | SRC_DEFAULT_QUALITY`。
- 独占模式先测试所选位深，必要时尝试同采样率、同声道的 32/24/16 位 PCM
  容器；不隐式更改采样率和声道。实际流格式写入播放诊断。
- 使用有界的四块解码队列；原生缓冲区限制为 40–80 ms，并按剩余空间提交。
  音量、平衡和淡入淡出的增益在提交原生缓冲区时应用，不提前写入整段解码缓存。
- 进度由已提交帧数减去 WASAPI 尚未播放的帧数计算，避免欠载期间的静音
  推动歌曲进度。曲尾等待最后一帧播放完毕；暂停保留缓冲，定位停止并重置流。
- 暂停、恢复、定位、停止和歌曲末尾使用现有淡入淡出设置。上一首／下一首
  使用串行淡出、关闭、打开下一流的路径，避免独占设备被自己占用。
- 音效、ReplayGain、DSP、输出位深、显式 SSRC 与可视化仍经过现有处理链。
  独占模式也应用播放器音量与音效，因此不承诺任意设置下均为位精确输出。
- MIDI 继续使用现有独立后端；此改动不将 MIDI 合成器改接到 WASAPI。
- 不增加 MMDevAPI 等新 DLL 的静态导入。XP 上 COM 类不可用时干净失败。

同时补齐了独立 `ttplayer_output_device_probe` 的文字转换／翻译链接依赖，
该诊断程序也可以独立构建和枚举新增输出。

## 验证

本地测试源码位于 `tests/audio/wasapi_playback_tests.cpp`，不提交测试源码；
Actions 继续配置 `BUILD_TESTING=OFF`，不执行测试。虚拟机使用独立目录
`C:\TTPlayerWasapi`，没有修改用户音乐。

| 环境 | 结果 |
| --- | --- |
| Windows 11，普通版核心 | 168 项通过 |
| VirtualBox Windows 7 SP1，兼容版核心 | 148 项通过 |
| VirtualBox Windows XP，兼容版核心 | 22 项通过，包括旧输出播放、暂停、定位、曲尾 |
| 兼容版静态导入检查 | 19 个 DLL、655 个导入，XP / Win7 检查通过 |
| 原有设备协议及进度定位回归 | `native_output_contract_tests`、`progress_seek_tests` 通过 |
| 选项页面 | 共享／独占条目与详情正确，关闭后 XML 保存独占模式选择 |

测试覆盖 44.1/48 kHz、8/16/24/32 位、单／双声道的实际设备打开及帧排空，
设备列表进程间序列化、固定设备、失效设备、暂停时定位、恢复、切歌淡出与
独占设备释放后的再次打开。硬件不支持的独占格式以预期错误通过检查。

Win7 虚拟声卡仅接受此次矩阵中的 44.1 kHz 双声道独占格式；48 kHz 和
单声道被明确拒绝。共享模式不受该限制。此结果不能推广为所有物理声卡的能力。
上述自动播放测试使用静音 PCM，验证接口与生命周期，不代替物理设备听感评估。

## API 依据

- [IAudioClient::Initialize](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-initialize)
- [共享／独占格式协商](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-isformatsupported)
- [系统转换标志](https://learn.microsoft.com/en-us/windows/win32/coreaudio/audclnt-streamflags-xxx-constants)
