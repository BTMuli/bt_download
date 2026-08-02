# bt_download 本地调用契约

协议版本：`1.0`。传输为继承的 stdin/stdout 管道，编码为 UTF-8，每个 JSON-RPC 2.0 对象独占一行。单帧最大 1 MiB。客户端必须同时持续读取 stdout 和 stderr；stdout 不包含日志。

> Tracker 补充、限量做种和 `seeding` 状态属于拟议的 `1.1` 扩展，尚未由当前实现提供。完整语义见 [tracker-and-seeding.md](tracker-and-seeding.md)，不能按本文的 `1.0` 已实现能力使用。

## 生命周期

1. 客户端启动 `bt_download.exe`。
2. 引擎发出 `event.ready`。
3. 客户端调用 `engine.initialize`，传入 `protocolVersion`、绝对 `statePath` 和可选 `config`。
4. 客户端先调用 `task.list` 获取事实快照，再消费增量事件。
5. 退出时调用 `engine.shutdown`；引擎持久化后回复并退出。

## 方法

| 方法 | 必需参数 | 说明 |
| --- | --- | --- |
| `engine.initialize` | `protocolVersion`, `statePath` | 初始化会话并恢复目录 |
| `engine.status` | - | 版本、运行时间、统计和配置 |
| `engine.configure` | 配置字段 | 运行时更新并持久化资源限制 |
| `engine.shutdown` | - | 保存、停止并退出 |
| `task.add` | `source`, `savePath` | 添加 torrentFile 或 magnet |
| `task.list` | - | 返回全量快照与当前事件序号 |
| `task.get` | `id` | 返回单任务快照 |
| `task.pause` / `task.resume` | `id` | 持久暂停或继续 |
| `task.retry` / `task.recheck` | `id` | 重试错误或强制校验 |
| `task.remove` | `id`, 可选 `deleteData` | 默认只移除任务 |

`source` 有两种形态：

```json
{"kind":"torrentFile","path":"C:\\absolute\\a.torrent"}
{"kind":"magnet","uri":"magnet:?xt=urn:btih:..."}
```

配置字段以字节/秒、秒和计数为单位：`activeDownloads`、`downloadRateLimit`、`uploadRateLimit`、`connectionsLimit`、`connectionsPerTask`、`metadataTimeoutSeconds`。速率 `0` 表示不限速；Magnet 元数据超时默认 300 秒，取值范围为 1 至 86400 秒。

## 拟议的 `1.1` 扩展

`engine.initialize` 的 `config` 和 `engine.configure` 拟新增：

```json
{
  "additionalTrackers": ["udp://tracker.example:6969/announce"],
  "seedingEnabled": true,
  "seedRatioLimit": 2.0,
  "seedTimeLimitMinutes": 60
}
```

- `additionalTrackers` 最多 512 条，只接受合法的 `udp`、`http`、`https` Tracker URL；
- `seedingEnabled=false` 表示文件完成后立即停止；启用时至少一个停止条件大于 0；
- 分享率与时间条件同时启用时，任一条件先满足即停止；
- 配置整体验证并原子生效，非法配置返回 `INVALID_CONFIG`；
- 任务状态新增 `seeding`，快照新增 `uploadedBytes`、`shareRatio`、`seedingSeconds`、`seedRatioLimit`、`seedTimeLimitMinutes` 和 `seedStopReason`；
- 文件可用通知发生在完整性校验完成时，不等待做种结束。

`1.1` 的 Tracker 列表源、自动更新时间和最后同步错误由 BangumiToday 管理，不通过本地引擎协议传输。

## 错误

JSON-RPC `error.data` 至少包含稳定的业务 `code` 与 `retryable`。调用方不得依赖面向用户的 `message` 做分支判断。

```json
{"jsonrpc":"2.0","id":"2","error":{"code":-32011,"message":"the torrent already exists at this save path","data":{"code":"DUPLICATE_TASK","retryable":false,"taskId":"..."}}}
```

常用业务码包括 `NOT_INITIALIZED`、`PROTOCOL_MISMATCH`、`INVALID_CONFIG`、`SOURCE_INVALID`、`SOURCE_UNSUPPORTED`、`UNSAFE_TORRENT_PATH`、`METADATA_TIMEOUT`、`SAVE_PATH_INVALID`、`SAVE_PATH_UNAVAILABLE`、`SAVE_PATH_NOT_WRITABLE`、`DISK_FULL`、`STORAGE_ERROR`、`DATA_VERIFICATION_FAILED`、`NETWORK_UNAVAILABLE`、`DUPLICATE_TASK`、`TASK_NOT_FOUND`、`TASK_UNAVAILABLE`、`PERSISTENCE_ERROR`、`TORRENT_ERROR` 和 `INTERNAL_ERROR`。任务快照的 `lastError` 使用同一组业务码和 `retryable` 语义。

## 事件

`event.taskAdded`、`event.taskUpdated`、`event.taskRemoved` 的 `params.sequence` 在单次引擎进程内单调递增。客户端发现序号缺口时应调用 `task.list` 重取全量快照。
