# 可选 AddIn HTTPS 请求库

更新：2026-09-25。

## 当前结构

```text
TTPlayerRebuild.exe
  原歌词参数、XML、LRC 处理
  https_provider.cpp：可选 C ABI 适配器
  原 WinHTTP 请求实现
AddIn/ttp_https.dll
  URL、DNS、TCP、代理／CONNECT、TLS、HTTP 解析、重定向、响应存储
  Mbed TLS 4.2.0 + TF-PSA-Crypto 1.2.0 + Mozilla 根证书
```

portable_https.cpp/.h 和底层 mbed_tls_min.h 已从播放器移除，传输迁至相邻独立项目 mbedtlsmin/src/https_client.cpp。
共享的公开 C 头文件为 include/ttplayer/net/ttp_https.h。

## 加载与回退

首次 HTTPS 请求按 EXE 绝对目录加载 AddIn/ttp_https.dll，并检查入口、ABI 版本和函数表。
缺失、损坏或 ABI 不兼容时，直接进入原 WinHTTP，其请求实现已核对保持不变。
EXE 同目录、工作目录和 PATH 的同名 DLL 不参与查找。
DLL 已加载后的证书／传输错误正常报错；自定义代理认证也由 DLL 处理；IE 集成登录策略或无法处理的系统 PAC 结果仍允许明确返回 WinHTTP 分支。
回退后受系统 WinHTTP TLS 能力限制。

成功加载的 DLL 保留到进程结束；替换或改变可用性后需重启。
音频插件扫描忽略该 DLL，避免作为解码器列入错误列表。

get/release 使用稳定 C ABI。响应由 DLL 分配及释放，不跨 DLL 传递 STL、异常或 CRT 所有权。
HTTP 明文歌词服务和旧歌词插件内部请求保持原行为。

## 构建与分发

在 mbedtlsmin 独立构建 ttp_https，得到 build/Release/AddIn/ttp_https.dll。
rebuild 的 TTPLAYER_HTTPS_DLL 只接受已构建 DLL，并将其复制到输出目录的 AddIn。
未配置时仍能独立构建播放器，缺少 DLL 仍能运行。
当前发行包只需要 AddIn/ttp_https.dll，不需要旧 mbed_tls_min.dll。
Actions 没有新增相邻源码构建或测试步骤；测试留在本地 tests/lyrics。

## 验证

本机、XP、Win7 均通过真实歌词搜索下载、TLS 1.2／1.3、32 路并发、错误证书拒绝和取消测试。
缺失、损坏、ABI 不兼容和错误目录四种场景均恢复原 WinHTTP。
音频扫描排除及生产 HTTP 解析器的边界测试也通过。

指定服务器为 [https://lyrics.qianqian.plus/api/search/](https://lyrics.qianqian.plus/api/search/)。
搜索“周杰伦／晴天”得到 1 个结果，下载 1388 个 wchar 的歌词。
上述是首次封装的验证基线（390,144 字节／381 KiB）；新增代理支持另见下节。

## 代理扩展（ABI 2）

适配器现已传入 proxy_username 和 proxy_password，解决此前只有“存在凭据”标志导致回退 WinHTTP 的问题。
自定义 HTTP CONNECT 支持 Basic、Digest（MD5、SHA-256、sess、auth/auth-int）、NTLM 和 Negotiate；Windows SSPI 只负责认证令牌，HTTPS 仍使用 DLL 的 Mbed TLS。
服务器栏支持 http://、socks4://、socks4a://、socks5://，其余字段不变。
新 DLL 保留 ABI 1；新 EXE 遇到旧 ABI 1 DLL 时按“不兼容组件”卸载并走原 WinHTTP。建议 EXE 与 AddIn/ttp_https.dll 配套更新。
系统 IE/PAC 配置的读取沿用原逻辑；自动登录策略仍由系统 WinHTTP 控制。
详细支持范围与本次验证状态见相邻 mbedtlsmin/docs/PROXY_SUPPORT.md。

### 代理扩展验证完成

本机、XP、Win7 各通过 47 项。真实歌词服务器经 Basic、Digest 和 SOCKS5 从播放器完成搜索和下载；HTTP ABI 探针经五种认证代理确认 TLS 1.3、证书校验正常。错误密码、错误证书、取消与三组各 32 路并发也通过。
测试发现并修复了 XP 的 ABI 1 函数表动态初始化问题，现用编译期常量表。最终 DLL 为 420,352 字节（410.5 KiB），SHA-256：`01bd20ca6a2e8efd55a4e057492391b31d6784fb0c1b1e55664781dbba092389`。
Negotiate 验证覆盖工作组内的 NTLM，未验证域 Kerberos。测试仅在本机与虚拟机运行，Actions 的 BUILD_TESTING=OFF 保持不变。
