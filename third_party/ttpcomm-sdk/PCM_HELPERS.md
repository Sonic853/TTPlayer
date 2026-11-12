# PCM 格式与近期缓存辅助代码

2026-10-24 新增两个 header-only 辅助文件，不增加 `ttpcomm.dll` 导出或运行时依赖。旧宿主插件可以携带同一份头文件独立构建。

- `include/ttpcomm/pcm_format.h`：以 CoTaskMemAlloc 返回 WAVEFORMATEX／WAVEFORMATEXTENSIBLE。多声道或有效位数小于容器位数时使用扩展格式；校验掩码位数与声道数。调用者必须已经保证 PCM 字节排列与掩码一致。`standard_mask` 仅用于布局已知的 FLAC、已重排 Vorbis／Opus 等来源，不替未知布局作推断。
- `include/ttpcomm/pcm_history.h`：每个 Reader 保存最多 4 MiB 连续、完整帧对齐的已交付 PCM。调用者传入有效指针、非零 blockAlign 和完整帧容量，区分逻辑播放位置与物理解码位置。缓存外回退交由编解码器自己的精确定位实现；分配失败放弃缓存，不影响解码正确性。

缓存用于 RM／VQF 近期回退，不是编码帧状态快照，也不替代完整索引。格式辅助用于 AAC／APE／FLAC／OGG／WebM／TAK／RM；各工程的 `include/ttpcomm` 副本应保持与 SDK 原件相同。

验证记录：工作区 `rebuild/docs/AUDIO_PLUGIN_COMPLETION_20261024.md`。私有回归留在 `rebuild/tests`，不加入 SDK 或发行包。
