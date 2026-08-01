# 性能基准

Windows 性能基准会启动同目录下真实的 `bt_download.exe`，本地 Tracker 和 Seeder 则运行在基准宿主进程中。这样进程 CPU、工作集和 I/O 计数只覆盖伴随进程，不包含造数、种子哈希和做种开销。

## 发布前运行

先构建 Release 产物：

```powershell
cmake --preset windows-x64-release
cmake --build --preset windows-x64-release
```

再运行完整基准并保存 JSON 结果：

```powershell
out/build/windows-x64-release/bt_download_performance_benchmark.exe `
  --output out/benchmarks/windows-x64-release.json
```

完整模式在初始化并预热后采样 5 分钟空闲窗口，属于发布前权威结果。开发阶段可使用 `--quick` 把空闲窗口缩短为 5 秒；其结果包含 `"authoritative": false`，不能替代发布验收：

```powershell
out/build/windows-x64-debug/bt_download_performance_benchmark.exe --quick
```

基准返回码为 `0` 表示所有预算通过，`2` 表示完成测量但至少一项超出预算，`1` 表示基准自身或下载流程失败。`--engine <path>` 可指定另一个引擎产物。

## 场景与指标

- 冷启动到 `event.ready` 小于 2 秒；
- 空闲窗口的 CPU 时间低于一个逻辑核心的 1%，峰值工作集低于 80 MiB；
- 两个并行的 64 MiB 本地下载期间，峰值工作集低于 250 MiB；
- JSON-RPC 本地请求 P95 小于 100 ms；
- 同一任务的普通下载进度事件间隔不小于 450 ms。450 ms 是针对 Windows 调度抖动的判定容差，输出仍同时记录实际最大频率和产品目标 2 Hz；
- 下载阶段保存有效负载吞吐、Windows 进程 I/O 读取/写入吞吐及 CPU，当前仅建立可比较基线，不设置与硬件无关的吞吐下限。进程 I/O 计数包含缓存 I/O，不等同于物理磁盘设备计数。

测试使用私有种子和回环 Tracker/Peer，不访问公网。结果 JSON 包含模式、参数、各项原始数值、预算和逐项判定，应作为发布流水线制品保存，并在相同机器、电源模式和安全软件条件下比较版本趋势。
