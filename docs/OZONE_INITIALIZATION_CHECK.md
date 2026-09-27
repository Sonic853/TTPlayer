# 真实 Ozone 初始化检查

> 本文记录先前使用隔离临时注册表完成的初始化验证。后续已经实现 Ozone 文件型注册表适配，并通过空原生目标分支下的初始化、登记写入、重启和迁移测试，见 [文件型兼容层实施记录](PLUGIN_FILE_REGISTRY_IMPLEMENTATION.md)。

日期：2026-09-27。

## 结论

**真实 `dsp_izOzone.dll` 已在 XP、Win7 虚拟机上完成初始化、同实例配置、实际 PCM 处理和退出检查。**
插件目录内已有的登记数据由 DLL 自行接受后，`Module+0x1C` 的处理对象成功建立，输出 PCM 实际发生变化，Quit 将该指针清空。
此前的初始化超时与缺少登记数据后进入注册对话框相符，不是已经证实的 DSP ABI 或消息线程故障。

本轮没有发现需要新增生产代码修复的问题；没有修改 Ozone 二进制或登记校验。本轮仅完善本地测试和文档，上一轮发行包无需因此重新构建。

## 样本与环境

- DLL：`Plugins/dsp_izOzone.dll`，x86，iZotope Ozone 1.03，3,022,923 字节。
- SHA256：`e3bb0eef979ea8016fb1278b373c7c70ae4507719524bfefe802b5bc3c800e59`。
- 宿主：本次修复后的真实 `WinampDspChain`，不是模拟 DSP。
- 系统：已运行的 XP 与 Win7 虚拟机，分别执行独立测试。
- 配置：复制 `Plugins/Ozone/iZOzone.cfg` 到测试目录对应的 `Ozone` 子目录，使用测试目录作为工作目录。
- 登记：仅使用现有添加 REG 中的 `EmailAddress`、`RegName`、`RegCode` 三个字符串，未引入历史 `Minimized` 设置。

## 测试隔离与清理

测试父进程创建具有唯一名称的易失测试分支：

```text
HKCU/Software/TTPlayerRebuild/Tests/Ozone-<进程号>-<时间戳>
    Software/iZotope/Ozone/Winamp2
```

父进程将三项原有登记值放入该分支。只有专用子进程调用 `RegOverridePredefKey`，将自身 HKCU 映射到这份测试副本，然后加载真实 DLL。
播放器、宿主机和虚拟机原有 Ozone 分支不进行登记导入；不记录登记值的具体内容。

父进程不参与重定向，负责等待子进程、在异常／超时后结束测试子进程并清理这次创建的分支。
两台 VM 最终均输出 `OZONE TEMP REGISTRY REMOVED`。传入 VM 的临时登记数据文件也已在使用后删除，本地中间副本已清理。

**这是测试隔离，仍会短暂创建 VM 注册表测试分支，不是产品已实现“零系统注册表写入”。**
它只作用于独立测试子进程，不能直接照搬到包含 Shell、COM 和其它插件的播放器进程。

## 检查结果

| 检查 | XP | Win7 |
| --- | --- | --- |
| 子进程读取隔离登记键 | 成功，状态 0 | 成功，状态 0 |
| Init 返回，实际 userData 非空 | 通过 | 通过 |
| 44.1 kHz、16 位双声道 PCM | 4092 / 4096 个采样值改变 | 4092 / 4096 个采样值改变 |
| 48 kHz、16 位双声道 PCM | 4094 / 4096 个采样值改变 | 4094 / 4096 个采样值改变 |
| Config 后仍为同一处理对象 | 通过 | 通过 |
| Config 后再次处理 PCM | 通过 | 通过 |
| Quit 清空 Module+0x1C | 通过 | 通过 |
| 测试进程正常结束、临时登记分支删除 | 通过 | 通过 |

输入是固定的双声道测试波形；“采样值改变”用于排除只挂了空对象或完全未调用音效的假通过。
两套系统的变化数量相同，但这不是逐字节输出比对，也不是与原版播放器的音质等价性证明。

## 与上次超时的对应关系

未提供登记副本时，两台 VM 的原有 `Software/iZotope/Ozone/Winamp2` 键查询返回 2（不存在），Init 未在 15 秒内返回，进程中出现 `#32770` 对话框。

二进制调用路径：

1. Init `5941A390` 调用登记检查 `594199B0`。
2. 登记键打开／值读取失败时，转入 `59419A83`。
3. `59419AA1` 调用 `DialogBoxParamA`，资源 ID 254，对话框回调 `5941A090`。
4. 登记检查通过后才创建处理对象并写入 `Module+0x1C`。

本轮通过登记副本和配置资源完成了后续分支，证实真实处理对象能够在修复后的宿主中建立并运行。
如果用户环境仍缺少插件接受的登记数据，该插件自己的注册提示仍可能出现；宿主不代替它作出登记成功判断。

## 测试代码与证据

本地测试：`tests/winamp_dsp_runtime_tests.cpp`、`tests/ozone_initialization_test.h`。
结果记录：`tests/artifacts/dsp_static/ozone-initialization-results.json`。
这些文件保留在本地测试目录，不上传测试代码、不放入发行包、不加入 Action。

本轮没有覆盖所有预设、完整 GUI 控件、长期播放、多声道、与原版逐采样对照或完整注册表便携化。
DSP 宿主修复及免注册表方案见 [WINAMP_DSP_RECOVERY.md](WINAMP_DSP_RECOVERY.md)。
