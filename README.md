# bt_download

`bt_download` 是随 BangumiToday 分发的无界面下载伴随进程。它通过 stdin/stdout
上的 JSON-RPC 2.0（每行一个 UTF-8 JSON 对象）提供统一任务管理能力，BitTorrent
核心使用 libtorrent，HTTP(S) 文件传输使用 libcurl。

当前实现覆盖以下能力：

- `.torrent` 文件和 Magnet URI 添加；
- 普通 HTTP(S) 文件直链、Range 断点续传及安全目标文件命名；
- 查询、暂停、继续、重试、重新校验和移除任务；
- info-hash + 规范化保存路径去重；
- 下载目录可写检查和种子内部路径穿越检查；
- 全局并发、速度和连接数限制；
- 公共任务补充 Tracker 的校验、动态替换与私有种子隔离；
- 按分享率或时间先到即停的限量做种以及跨重启累计；
- 多文件种子按文件选择与优先级（跳过/下载），随 resume 数据跨重启恢复；
- 下载详情按 Tab 拆分：`task.details` 返回概览与文件/Peer 总数，`task.files` / `task.peers` 按窗口分页拉取；
- 协议 `1.5` 与 BangumiToday 随包严格同步，不做旧版本协商或回退；
- HTTP 与 BT 网络可接收应用下发的 Windows 系统代理快照，运行时热更新且不持久化凭据；
- 本地任务目录与 fast-resume 恢复、稳定错误模型和单调事件序号；
- HTTP 临时文件恢复、原子完成重命名和精确删除语义；
- Windows x64 Debug/Release 构建与协议/状态机/路径单元测试。

调用契约见 [docs/protocol.md](docs/protocol.md)。

参与开发前请阅读 [贡献指南](CONTRIBUTING.md)。所有提交均采用 Gitmoji 格式。

## 构建

需要 Visual Studio（Desktop development with C++）、CMake、Ninja 和 vcpkg。依赖由 `vcpkg.json` 固定。

```powershell
$env:VCPKG_ROOT = '<vcpkg 目录>'
cmake --preset windows-x64-debug
cmake --build --preset windows-x64-debug
ctest --preset windows-x64-debug
```

产物位于 `out/build/windows-x64-debug/bt_download.exe`。stdout 只输出协议帧；诊断日志只写 stderr。

Windows Release 分发应使用 `cmake --install out/build/windows-x64-release` 生成的完整目录，不能单独复制可执行文件。依赖 DLL、许可证与 SPDX SBOM 的生成和接入约束见 [docs/release.md](docs/release.md)。

## 性能基准

Windows Release 发布前应运行独立进程资源基准，采集空闲 CPU/工作集、双任务磁盘吞吐、请求延迟和进度事件频率，并保存 JSON 结果。完整命令、快速回归模式和指标口径见 [docs/performance.md](docs/performance.md)。

## 最小调用

进程启动后先输出 `event.ready`。客户端随后初始化；`protocolVersion` 必须与引擎协议版本严格一致（当前为 `1.5`）：

```json
{"jsonrpc":"2.0","id":"1","method":"engine.initialize","params":{"protocolVersion":"1.5","statePath":"C:\\Users\\me\\AppData\\Local\\BangumiToday\\bt_download"}}
```

添加任务：

```json
{"jsonrpc":"2.0","id":"2","method":"task.add","params":{"source":{"kind":"torrentFile","path":"C:\\Temp\\a.torrent"},"savePath":"D:\\Anime","start":true}}
```

添加普通 HTTP 文件任务：

```json
{"jsonrpc":"2.0","id":"3","method":"task.add","params":{"source":{"kind":"http","url":"https://example.com/releases/app.exe"},"savePath":"D:\\Downloads","start":true}}
```
