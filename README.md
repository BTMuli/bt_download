# bt_download

`bt_download` 是随 BangumiToday 分发的无界面 BitTorrent 伴随进程。它通过 stdin/stdout 上的 JSON-RPC 2.0（每行一个 UTF-8 JSON 对象）提供任务管理能力，下载核心使用 libtorrent。

当前实现覆盖第一阶段可运行闭环：

- `.torrent` 文件和 Magnet URI 添加；
- 查询、暂停、继续、重试、重新校验和移除任务；
- info-hash + 规范化保存路径去重；
- 下载目录可写检查和种子内部路径穿越检查；
- 全局并发、速度和连接数限制；
- 本地任务目录与 fast-resume 恢复、稳定错误模型和单调事件序号；
- Windows x64 Debug/Release 构建与协议/状态机/路径单元测试。

完整需求和分阶段里程碑见 [docs/requirements.md](docs/requirements.md)，当前实现边界见 [docs/implementation.md](docs/implementation.md)，调用契约见 [docs/protocol.md](docs/protocol.md)。

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

## 性能基准

Windows Release 发布前应运行独立进程资源基准，采集空闲 CPU/工作集、双任务磁盘吞吐、请求延迟和进度事件频率，并保存 JSON 结果。完整命令、快速回归模式和指标口径见 [docs/performance.md](docs/performance.md)。

## 最小调用

进程启动后先输出 `event.ready`。客户端随后初始化：

```json
{"jsonrpc":"2.0","id":"1","method":"engine.initialize","params":{"protocolVersion":"1.0","statePath":"C:\\Users\\me\\AppData\\Local\\BangumiToday\\bt_download"}}
```

添加任务：

```json
{"jsonrpc":"2.0","id":"2","method":"task.add","params":{"source":{"kind":"torrentFile","path":"C:\\Temp\\a.torrent"},"savePath":"D:\\Anime","start":true}}
```
