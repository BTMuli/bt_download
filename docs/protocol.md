# bt_download 本地调用契约

协议版本：`1.1`。传输为继承的 stdin/stdout 管道，编码为 UTF-8，每个 JSON-RPC 2.0 对象独占一行。单帧最大 1 MiB。客户端必须同时持续读取 stdout 和 stderr；stdout 不包含日志。

引擎接受 `1.0` 客户端，但该会话不能配置补充 Tracker 和限量做种，因而不会向旧客户端发送 `seeding` 状态。如果持久状态已经启用 `1.1` 能力，旧客户端初始化将返回 `PROTOCOL_MISMATCH`，避免静默终止正在做种的任务。高于引擎次版本或主版本不同的客户端同样返回该错误。

## 生命周期

1. 客户端启动 `bt_download.exe`。
2. 引擎发出 `event.ready`，其中包含协议版本、引擎版本和 `features`。
3. 客户端调用 `engine.initialize`，传入 `protocolVersion`、绝对 `statePath` 和可选 `config`。
4. 客户端先调用 `task.list` 获取事实快照，再消费增量事件。
5. 退出时调用 `engine.shutdown`；引擎持久化后回复并退出。

## 方法

| 方法 | 必需参数 | 说明 |
| --- | --- | --- |
| `engine.initialize` | `protocolVersion`, `statePath` | 初始化会话并恢复目录 |
| `engine.status` | - | 版本、运行时间、统计和配置 |
| `engine.configure` | 配置字段 | 运行时更新并持久化资源限制、补充 Tracker 和做种策略 |
| `engine.shutdown` | - | 保存、停止并退出 |
| `task.add` | `source`, `savePath` | 添加 torrentFile 或 magnet |
| `task.list` | - | 返回全量快照与当前事件序号 |
| `task.get` | `id` | 返回单任务快照 |
| `task.details` | `id` | 返回任务快照、分片完成状态、文件进度和当前 Peer；文件和 Peer 列表可能截断 |
| `task.setFilePriorities` | `id`, `priorities` | 更新多文件任务的单个或多个文件优先级 |
| `task.pause` / `task.resume` | `id` | 持久暂停或继续 |
| `task.retry` / `task.recheck` | `id` | 重试错误或强制校验 |
| `task.remove` | `id`, 可选 `deleteData` | 默认只移除任务 |

`source` 有两种形态：

```json
{"kind":"torrentFile","path":"C:\\absolute\\a.torrent"}
{"kind":"magnet","uri":"magnet:?xt=urn:btih:..."}
```

配置字段以字节/秒、秒、分钟和计数为单位：`activeDownloads`、`downloadRateLimit`、`uploadRateLimit`、`connectionsLimit`、`connectionsPerTask`、`metadataTimeoutSeconds`、`additionalTrackers`、`seedingEnabled`、`seedRatioLimit`、`seedTimeLimitMinutes`。速率 `0` 表示不限速；Magnet 元数据超时默认 300 秒，取值范围为 1 至 86400 秒。

产品默认值为并行任务 `4`、下载和上传均不限速、全局连接 `256`、单任务连接 `64`，下载完成后继续做种。

Tracker 与做种配置示例：

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
- 文件完整性校验完成后，状态直接转为 `seeding`，或在禁用/已满足限制时转为 `completed`；客户端可据此发送文件可用通知，不应等待做种结束。

## 文件选择与优先级

`task.details` 的文件项包含 `priority` 字段。`task.setFilePriorities` 按部分索引更新文件优先级，`priorities` 是文件索引到优先级的对象：

```json
{"jsonrpc":"2.0","id":"3","method":"task.setFilePriorities","params":{"id":"...","priorities":{"0":4,"2":0}}}
```

- 优先级取值范围为 `0` 到 `7`：`0` 表示跳过该文件（不下载），`1` 为低优先级，`4` 为默认优先级，`7` 为最高优先级；其余值保留给 libtorrent 的分层优先级；
- 未出现的文件索引保持原有优先级，引擎合并当前向量后整表下发；响应 `priorities` 返回更新后的完整优先级数组；
- 索引必须落在 `[0, 文件数)`，优先级必须是整数；`priorities` 必须是非空对象且条目数不超过 2000，否则返回 `INVALID_FILE_PRIORITY`；
- 元数据不可用时返回 `METADATA_UNAVAILABLE`；已完成或做种中的任务不允许修改（libtorrent 对种子任务不生效），返回 `TASK_UNAVAILABLE`；
- 修改在磁盘线程异步生效，引擎等待生效后才返回，并随后触发 fast-resume 保存；优先级随 resume 数据跨重启恢复；
- 被跳过的文件已下载部分不会被删除，也不会从 partfile 移出，UI 应在下载前或暂停时引导用户修改选择。

Tracker 列表源、自动更新时间和最后同步错误由 BangumiToday 管理，不通过本地引擎协议传输。引擎未收到 `seedingEnabled` 时使用安全默认值 `false`；BangumiToday `1.1` 客户端负责在新安装且用户已确认提示后显式传入产品默认值 `true`、`2.0` 和 `60`。

## 错误

JSON-RPC `error.data` 至少包含稳定的业务 `code` 与 `retryable`。调用方不得依赖面向用户的 `message` 做分支判断。

```json
{"jsonrpc":"2.0","id":"2","error":{"code":-32011,"message":"the torrent already exists at this save path","data":{"code":"DUPLICATE_TASK","retryable":false,"taskId":"..."}}}
```

常用业务码包括 `NOT_INITIALIZED`、`PROTOCOL_MISMATCH`、`INVALID_CONFIG`、`SOURCE_INVALID`、`SOURCE_UNSUPPORTED`、`UNSAFE_TORRENT_PATH`、`METADATA_TIMEOUT`、`SAVE_PATH_INVALID`、`SAVE_PATH_UNAVAILABLE`、`SAVE_PATH_NOT_WRITABLE`、`DISK_FULL`、`STORAGE_ERROR`、`DATA_VERIFICATION_FAILED`、`NETWORK_UNAVAILABLE`、`DUPLICATE_TASK`、`TASK_NOT_FOUND`、`TASK_UNAVAILABLE`、`PERSISTENCE_ERROR`、`TORRENT_ERROR` 和 `INTERNAL_ERROR`。任务快照的 `lastError` 使用同一组业务码和 `retryable` 语义。

## 事件

`event.taskAdded`、`event.taskUpdated`、`event.taskRemoved` 的 `params.sequence` 在单次引擎进程内单调递增。客户端发现序号缺口时应调用 `task.list` 重取全量快照。
