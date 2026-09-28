# 多 DSP 插件卡顿与输出缓冲恢复修复

日期：2026-09-29。对应 [问题分析及复现证据](DSP_MULTI_PLUGIN_STUTTER_ANALYSIS.md)。

## 修复内容

### 1. 恢复原版的 PCM 线程分工

`WinampDspChain::Process` 现在直接在调用它的音频工作线程上执行，按当前列表顺序串联处理。移除了将 PCM 投递到插件界面消息队列的路径。

依据原版 `004AC166 → 0042898D → 004280CD`：

- Ozone、DFX 等插件仍依次处理同一块 PCM，不并行混合同一块数据。
- 保持 16 位交错 PCM、16,384 帧阈值处的一次两半拆分，以及忽略插件返回帧数的原版约定。
- Init、Config、Quit 和插件窗口仍由稳定的插件界面线程管理。
- 配置回调允许与音频工作路径并行，这与原版 `004280BE` 调用 Config 时不持有 PCM 锁的行为一致。

DFX 的宿主查询仍真实执行，没有在正式代码中跳过查询、伪造成功或改写 DLL。移走 PCM 所在线程后，主界面停顿时查询能按插件原有超时返回，不再把整条音频链挂在界面消息线程上。

### 2. 在途回调和插件生命周期保护

- 多个播放会话共享同一 DSP 链时，PCM 回调串行执行。
- 重入同一条 PCM 链时不再次使用正在处理的 scratch 缓冲。
- 加载、卸载、排序使用同一处理锁保护；主界面提出变更时，如果 PCM 或 Config 正在执行，就保留最新请求，在安全的回调边界应用。
- 待处理请求在尝试获取处理锁前发布，避免最后一块 PCM 恰好结束时丢失卸载通知。
- 排序复用原实例，保留插件滤波器和延迟状态。
- 异常插件标记为旁路，保留实例到界面线程安全移除；不在音频线程调用 Quit 或卸载 DLL。这也恢复了原版异常标记的语义。
- 诊断消息使用独立锁，读取日志不再同步访问插件界面线程。界面读取日志时也不会等待正在查询主窗口的 PCM 回调。

### 3. 播放启动等待恢复

本次不增加嵌套主窗口消息循环，也不延长原来的 4 秒打开预算。

启动预填充期间，DFX 在音频线程上的查询恢复为有限等待；初始化期间的诊断读取也不再排到插件界面队列。原来“窗口线程等打开、PCM 等窗口查询”的复现路径已通过测试。

### 4. WASAPI 缓冲不足时等待补充

独立渲染线程继续保留。新增有效音频余量的低水位和恢复阈值：

- 正常 1,000 ms 配置下，剩余不足约 100 ms 时暂停设备，保留未播放的 PCM。
- 恢复到约 80% 配置容量后继续，减少少量数据反复到达时的断续播放。
- 对 100 ms 等较小缓冲，低水位不超过一个输出分段，避免阈值大于有效容量。
- 增加 `FinishInput`，使最后不足阈值的尾部音频仍能完整播放；seek/reset 清除该标记。
- 等待补充期间不推进源音频位置，不重新提交旧队列。

这是恢复原版 `004AC605` 的缓冲状态语义；原版没有 WASAPI 后端，设备接口部分仍是重建版实现。

### 5. DirectSound 旧环重播保护

新增独立的缓冲进度监测线程，约每 5 ms 查询实际游标，不依赖解码或 DSP 回调及时返回。

- 低水位时停止 `DSBPLAY_LOOPING`，补足后继续，避免读到已经消费的旧环内容。
- 监测线程持续累计游标，修复先前 DSP 阻塞超过一圈后丢失整圈时间的问题。
- 补数条件改用饱和的余量计算，避免 `stream_write_byte - played_bytes` 无符号下溢。
- seek 期间暂停监测并同步重置计数；退出前先停止、回收监测线程，再释放 DirectSound 对象。

## 验证

使用歌曲：

```text
C:\Users\Sonic853\Music\test\蔡明希（不才）,三体宇宙 - 夜航星 (Night Voyager).flac
```

48 kHz、24 位、双声道，时长约 303.701 秒；原始歌曲和插件 DLL 未改写。插件及配置复制到本地测试目录，使用真实 AddIn 解码器。

### 已完成的针对性回归

| 项目 | 结果 |
| --- | --- |
| Ozone＋DFX，主界面停止处理消息 3 秒 | 10 秒内源位置推进至 9.99 秒；真实 DFX 查询最长约 11 ms |
| 窗口线程直接同步启动两插件 | 约 531 ms 成功；此前为 4,000 ms 超时 |
| PCM 输出线程与插件界面线程不同 | 通过 |
| 插件界面线程故意停顿 500 ms | PCM 不排队等待该停顿，通过 |
| 两播放会话共享 DSP、半块拆分、两倍输出容量保护 | 通过 |
| PCM 执行期间请求停用 | UI 请求及时返回，回调结束后 Quit；没有在途卸载 |
| 模态 Config 期间处理和退出 | 通过 |
| 真实 Ozone／DFX 热启用、排序、停用和重新启用 | 通过 |
| 打开真实 Ozone 配置窗口时继续播放 | 通过 |
| 真实插件暂停、暂停中 seek、恢复、停止后重开、300 ms 尾部 | 通过 |
| WASAPI 共享／独占格式、缓冲恢复、暂停、seek、EOF | 204 项检查通过；设备不支持的独占单声道组合明确跳过 |
| DirectSound，两个插件各一次延迟 700 ms，1,000 ms 环 | 源帧回退 0 次；实际游标约 4,230 ms，引擎约 4,220 ms |
| XP SP3 虚拟机：DSP 线程／生命周期回归 | `DSP_RUNTIME_EXIT=0` |
| Win7 SP1 虚拟机：DSP 线程／生命周期回归 | `DSP_RUNTIME_EXIT=0` |

虚拟机测试使用独立目录 `C:\DSP-Multi-Fix-20260929`。该项是测试插件的线程、拆分、容量、模态 Config、排序、异常及在途卸载回归，不应表述为两台虚拟机都完成了真实 Ozone／DFX 整曲播放。

### 指定 FLAC 整曲验证

WASAPI 共享模式，Ozone → DFX，包含一次 3 秒主窗口停顿：

- 墙钟耗时 303,703 ms，最终源位置 303,701 ms，正常 EOF，退出码 0。
- 每个插件处理 14,236 块，共 14,577,664 帧。
- 最大观察进度落后 41 ms；DFX 宿主查询最长 11.201 ms。
- 主窗口线程同步启动约 531 ms；停止和销毁正常完成。

日志：`tests/artifacts/dsp_multi_plugin_diagnosis/wasapi-full-song/result.log`。

先前同样的 DirectSound 用例出现源帧从 59,520 回到 12,000，以及引擎漏计约 1,000 ms。本次设备在缓冲不足时暂停，所以不能拿五秒墙钟时间要求设备无条件推进五秒；应检查是否重播以及设备与源时间线是否一致。

以上实时测试输出静音，依据是实际设备位置、PCM 帧标记和回调计时，没有声卡回录。界面停顿期间测试线程也停止轮询，不能把轮询间隔本身当成音频停顿。

### 独占模式与反向插件顺序

WASAPI 独占模式，DFX → Ozone，同样包含一次 3 秒主界面停顿：

- 墙钟耗时 303,703 ms，最终源位置 303,701 ms，正常 EOF，退出码 0。
- 两个插件各处理 14,236 块；最大观察进度落后 47 ms。
- DFX 真实窗口查询最长 11.371 ms；同步打开约 532 ms。
- 停止和析构正常完成。

日志：`tests/artifacts/dsp_multi_plugin_diagnosis/wasapi-full-song-exclusive-reverse/result.log`。

### 整曲 PCM 一致性

以相同初始配置、相同 FLAC 解码输入和每块 1,024 帧，对比修复后的宿主适配层与直接调用原始 Ozone → DFX 插件。两条路径均处理 14,236 块：

- 输出均为 58,310,656 字节，逐字节一致。
- SHA256 均为 `7a0f3efe5182e6de2ce0d2c6c2f4d84811e9da1d350c8f5dc63e2accffbcdb8b`。
- 返回帧数异常 0 次，相邻块完全重复 0 次；退出码均为 0。

这验证本次线程调整保留了该组合在指定输入和配置下的 PCM 处理结果，不代表所有第三方插件的所有配置均已验证。

结果目录：`tests/artifacts/dsp_multi_plugin_diagnosis/ozone-dfx-{host,direct}-1024-14236-flac-fixed/`。

### WaveOut / DirectSound 定位回归

使用指定 FLAC，普通模式和迷你模式下的进度拖动、向前／向后定位、暂停及暂停时定位均通过，退出码 0；同时通过定位时取消淡入淡出和皮肤 LED 命中区域回归。

日志：`tests/artifacts/dsp_multi_plugin_fix/progress-seek/result.log`。

## 文件与交付

主要改动：

- `src/audio/winamp_dsp.cpp`、`include/ttplayer/audio/winamp_dsp.h`
- `src/audio/dsp_dispatcher.h`
- `src/audio/wasapi_sink.cpp`、`include/ttplayer/audio/wasapi_sink.h`
- `src/audio/audio_engine.cpp`

本地测试代码留在 `rebuild/tests`，不加入发行包和 Actions。测试结果在：

- `tests/artifacts/dsp_multi_plugin_fix/`
- `tests/artifacts/dsp_multi_plugin_diagnosis/wasapi-*/`

本轮不新增前端错误状态列，也不改变共用 `registry.json` 的读取／写入策略。

通用 Release 构建已通过 XP／Win7 静态导入检查：播放器 20 个 DLL、714 项导入；更新器 13 个 DLL、291 项导入。

输出：

- `build/Release/TTPlayerRebuild.exe`，版本 `2026.09.29`。
- `build/Release/TTPlayerRebuild-2026.09.29.zip`。

ZIP 仅包含播放器、更新器、已核验的 HTTPS 组件和 `SHA256SUMS.txt`。已逐项核对 ZIP 内校验和，确认其中 EXE 与输出目录的 EXE 相同。打包使用现有已验证发行包中的 HTTPS 组件，未将本地暂存的不同组件误装入包。
