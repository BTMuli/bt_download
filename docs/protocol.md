# bt_download 本地调用契约

协议版本：`1.4`。传输为继承的 stdin/stdout 管道，编码为 UTF-8，每个 JSON-RPC 2.0 对象独占一行。单帧最大 1 MiB。客户端必须同时持续读取 stdout 和 stderr；stdout 不包含日志。

客户端与引擎始终同版本发布，`engine.initialize` 的 `protocolVersion` 必须与引擎协议版本严格一致（`1.4`），不一致返回 `PROTOCOL_MISMATCH`；不再保留旧协议协商或回退路径。

下载详情按 Tab 拆分：`task.details` 只返回概览（任务、分片状态和文件/Peer 总数），Peer 与文件列表通过 `task.files` / `task.peers` 按需分页拉取。

## 生命周期

1. 客户端启动 `bt_download.exe`。
2. 引擎发出 `event.ready`，其中包含协议版本、引擎版本和 `features`。
3. 客户端调用 `engine.initialize`，传入 `protocolVersion`、绝对 `statePath`、可选 `userAgent`、可选 `config` 和运行期 `proxy`。
4. 客户端先调用 `task.list` 获取事实快照，再消费增量事件。
5. 退出时调用 `engine.shutdown`；引擎持久化后回复并退出。

## 方法

| 方法 | 必需参数 | 说明 |
| --- | --- | --- |
| `engine.initialize` | `protocolVersion`, `statePath` | 初始化会话并恢复目录（可选 `userAgent`、`config` 与运行期 `proxy`） |
| `engine.status` | - | 版本、运行时间、统计和配置 |
| `engine.configure` | 配置字段 | 运行时更新并持久化资源限制、补充 Tracker 和做种策略 |
| `engine.configureProxy` | 代理对象 | 热更新 HTTP 与 BT 代理；不持久化端点或凭据 |
| `engine.shutdown` | - | 保存、停止并退出 |
| `task.add` | `source`, `savePath` | 添加 torrentFile、magnet 或 HTTP(S) 文件任务 |
| `task.list` | - | 返回全量快照与当前事件序号 |
| `task.get` | `id` | 返回单任务快照 |
| `task.details` | `id` | 返回任务快照、分片完成状态和文件/Peer 总数（不携带列表） |
| `task.files` | `id`, 可选 `offset`/`limit` | 按窗口返回文件进度与优先级，含总数和截断语义 |
| `task.peers` | `id`, 可选 `offset`/`limit` | 按窗口返回当前 Peer，含总数和截断语义 |
| `task.setFilePriorities` | `id`, `priorities` | 更新多文件任务的单个或多个文件优先级 |
| `task.pause` / `task.resume` | `id` | 持久暂停或继续 |
| `task.retry` / `task.recheck` | `id` | 重试错误或强制校验 |
| `task.remove` | `id`, 可选 `deleteData` | 默认只移除任务 |

### 强制重校验语义

`task.recheck` 是显式的本地文件完整性校验，不等同于 `task.retry`：

- 该方法只适用于具有 piece hash 的 BT 任务；HTTP 文件任务没有可用于内容认证的
  元数据，调用时返回 `TASK_UNAVAILABLE`；

- 调用后任务立即进入 `checking`，并通过 `event.taskUpdated` 通知；引擎重新读取
  磁盘文件，按种子 piece hash 更新已下载和已验证字节；
- `seeding` 或 `completed` 任务也会被唤醒执行校验。若发现文件或数据块缺失，旧的
  做种停止原因不会阻止任务进入 `downloading`，任务会重新发现 Peer 并下载缺失数据；
- 校验完成且任务需要继续联网时，会主动重新公告 Tracker，以便尽快获得可用 Peer；
- 若数据仍然完整，不会重复下载，任务按当前做种策略回到 `seeding` 或 `completed`。

引擎还会在任务处于 `seeding` 时后台检查 payload 文件的存在和大小（目标间隔约 1 秒）。
若用户或其他程序删除/截断文件，引擎会自动进入 `checking` 并重新下载缺失数据；用户主动
暂停的任务以及已因做种策略进入 `completed` 的任务不会因此自动唤醒，后者可调用
`task.recheck` 恢复。

`source` 有三种形态：

```json
{"kind":"torrentFile","path":"C:\\absolute\\a.torrent"}
{"kind":"magnet","uri":"magnet:?xt=urn:btih:..."}
{"kind":"http","url":"https://example.com/releases/app.exe"}
```

客户端在 `engine.initialize` 中可传入 `userAgent`，作为引擎的 BT 下载/上传 UA
（tracker、web seed 与 Peer 握手共用）及 HTTP 请求 UA，格式与 Bangumi 请求 UA 保持一致
（`BangumiToday/<版本>`）；未传或为空时使用默认值 `bt_download/<引擎版本>`
（即 `BT_DOWNLOAD_VERSION`）。

### 应用系统代理

`proxy` 是应用读取 Windows 当前用户代理后生成的运行期快照：

```json
{
  "enabled": true,
  "httpProxy": "http://127.0.0.1:7890",
  "httpsProxy": "http://user:password@127.0.0.1:7891",
  "bypass": ["<local>", "*.example.com"],
  "peerProxy": {
    "host": "127.0.0.1",
    "port": 7891,
    "username": "user",
    "password": "password"
  }
}
```

- HTTP 任务按源 URL 选择 `httpProxy` / `httpsProxy`，缺少对应端点时回退另一端点，
  并按 `bypass` 决定直连；
- libtorrent 使用 `peerProxy` 代理 Peer、Web Seed、HTTP(S) Tracker 和主机名解析。
  热更新时引擎暂停并恢复 libtorrent 会话，使既有连接按新配置重建。Windows 提供
  HTTP 代理时，BT 侧通过 HTTP CONNECT 建立 TCP 连接；
- 代理启用且 `peerProxy` 可用时，DHT、LSD、UPnP 与 NAT-PMP 会关闭，UDP Tracker
  也无法通过 HTTP 代理，避免这些通道绕过代理；
- `engine.configureProxy` 会保存活跃 HTTP Range 状态、重建连接并继续任务；
- `engine.status.proxy` 和初始化结果只返回 `enabled`、`httpConfigured`、
  `httpsConfigured`、`peerConfigured`，不会回显地址或凭据；代理对象不会写入
  `catalog.json`；
- 非法代理对象返回 `INVALID_PROXY`。PAC/WPAD 与应用现有代理能力一致，暂不支持。

配置字段以字节/秒、秒、分钟和计数为单位：`activeDownloads`、`downloadRateLimit`、`uploadRateLimit`、`connectionsLimit`、`connectionsPerTask`、`metadataTimeoutSeconds`、`additionalTrackers`、`seedingEnabled`、`seedRatioLimit`、`seedTimeLimitMinutes`。速率 `0` 表示不限速；Magnet 元数据超时默认 300 秒，取值范围为 1 至 86400 秒。

`activeDownloads` 是 BT 与 HTTP 的共享活动任务上限。HTTP 活动任务会占用槽位并
动态收缩 libtorrent 的下载预算；`downloadRateLimit` 在活动 HTTP 任务间均分，
BT 侧继续由 libtorrent 执行其下载限速。

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

文件项（由 `task.details` 或 `task.files` 返回）包含 `priority` 与 `isPadding` 字段。`isPadding=true` 表示 libtorrent 的对齐文件；它不对应实际下载文件，客户端应隐藏该项，但仍保留它在文件数组中的索引。`task.setFilePriorities` 按部分索引更新文件优先级，`priorities` 是文件索引到优先级的对象：

```json
{"jsonrpc":"2.0","id":"3","method":"task.setFilePriorities","params":{"id":"...","priorities":{"0":4,"2":0}}}
```

- 优先级取值范围为 `0` 到 `7`：`0` 表示跳过该文件（不下载），`1` 为低优先级，`4` 为默认优先级，`7` 为最高优先级；其余值保留给 libtorrent 的分层优先级；
- 未出现的文件索引保持原有优先级，引擎合并当前向量后整表下发；响应 `priorities` 返回更新后的完整优先级数组；
- 索引必须落在 `[0, 文件数)`，优先级必须是整数；`priorities` 必须是非空对象且条目数不超过 2000，否则返回 `INVALID_FILE_PRIORITY`；
- 元数据不可用时返回 `METADATA_UNAVAILABLE`；已完成或做种中的任务不允许修改（libtorrent 对种子任务不生效），返回 `TASK_UNAVAILABLE`；
- 修改在磁盘线程异步生效，引擎等待生效后才返回，并随后触发 fast-resume 保存；优先级随 resume 数据跨重启恢复；
- 被跳过的文件已下载部分不会被删除，也不会从 partfile 移出，UI 应在下载前或暂停时引导用户修改选择。
- HTTP 任务固定为一个文件且优先级不可修改，调用 `task.setFilePriorities` 返回
  `TASK_UNAVAILABLE`。

## HTTP(S) 文件任务

- 初始 URL 和所有重定向都只允许 `http` / `https`，TLS 证书始终使用系统信任链校验；
- 文件名来自 URL 最后一个路径段，百分号解码后替换 Windows 非法字符并规避设备
  保留名；目标已存在时使用 `name (1).ext` 形式创建新名称，不覆盖用户文件；
- 下载写入 `<目标文件>.<task-id>.part`，成功关闭文件后在同目录重命名为目标文件；
- 新任务先用 `Range: bytes=0-0` 探测最终响应；支持时按文件大小启动最多 `8` 条
  Range 连接（同时受 `connectionsPerTask` 约束），小文件只使用一条；服务端忽略或
  拒绝 Range 时删除该任务的分段临时状态并自动降级为单连接；
- 多连接写入同一 `.part` 的互不重叠偏移，并使用 `.part.ranges.json` 原子记录区间进度、
  ETag/Last-Modified 和总长度；暂停或异常退出后从每个区间已完成位置继续；
- 每个分段响应必须是与请求完全匹配的 `206 Content-Range`。引擎通过 `If-Range` 绑定
  强 ETag 或 Last-Modified；资源版本变化、区间越界或总长度不一致时从头下载；
- HTTP 任务使用 `queued`、`downloading`、`paused`、`completed`、`error` 状态。
  快照中的 `infoHash=null`，上传、Peer、Seed 和做种字段为中性值；
- `task.details` 报告 `totalFiles=1`、`contentFiles=1`、`totalPeers=0`，
  `httpConnections` 为当前 HTTP 请求连接数，`task.files` 返回目标文件及进度，
  `task.peers` 返回空列表；
- `task.details` 同时把文件映射为最多 `256` 个虚拟字节分片：`pieceLength` 为每个
  区间的目标大小（最小 `256 KiB`），`pieceCount` 为区间数，`completedPieces` 按
  文件顺序使用 `0/1` 标记未完成/已完整传输的区间。该位图仅用于进度可视化，不表示
  BT piece，也不表示完成哈希校验；响应总大小未知时三个字段暂为 `0/0/""`；
- `task.remove` 默认保留目标文件和 `.part`；`deleteData=true` 只删除 catalog 中
  精确记录的目标文件与任务专属临时文件；
- 完成后的 `verifiedBytes` 等于文件大小，表示 HTTP 传输完整结束，不表示内容通过
  加密哈希认证。

## 下载详情按 Tab 拆分

协议将 Peer/文件列表从 `task.details` 中拆出。概览响应不再携带大列表，文件与 Peer 数量以 `totalFiles` / `contentFiles` / `totalPeers` 提供，客户端可直接渲染 Tab 计数。`totalFiles` 包含 padding 项，`contentFiles` 只统计真实文件；文件索引和分页仍以 `totalFiles` 为准：

```json
{"jsonrpc":"2.0","id":"2","method":"task.details","params":{"id":"..."}}
```

```json
{"id":"2","jsonrpc":"2.0","result":{"task":{...},"pieceLength":16384,"pieceCount":1024,"completedPieces":"0101...","totalFiles":42,"contentFiles":31,"totalPeers":37,"httpConnections":0,"files":[],"filesTruncated":false,"peers":[],"peersTruncated":false}}
```

`task.files` / `task.peers` 使用相同的窗口语义：`offset` 默认 `0`，`limit` 默认分别是 `2000` 与 `500`（也是单次窗口上限）。`filesTruncated` / `peersTruncated` 表示当前窗口之后仍有数据；此时 `nextOffset` 为下一页起点，否则为 `null`：

```json
{"jsonrpc":"2.0","id":"3","method":"task.files","params":{"id":"...","offset":0,"limit":500}}
```

```json
{"id":"3","jsonrpc":"2.0","result":{"files":[{"path":"...","size":1048576,"completedBytes":524288,"priority":4,"isPadding":false}],"filesTruncated":true,"totalFiles":1200,"contentFiles":1000,"offset":0,"nextOffset":500}}
```

- `offset` 必须是非负整数，`limit` 必须是 `1` 到窗口上限的整数，否则返回 `INVALID_PAGINATION`；
- 窗口起点等于或超过总数时返回空列表且 `*Truncated=false`、`nextOffset=null`；
- BT 元数据不可用时文件与 Peer 列表为空、总数与窗口为 `0`；HTTP 任务始终按
  单文件返回；
- 两个列表均为当前时刻的快照，Peer 列表分页在连接变化时可能移动窗口，客户端应仅用于展示；

Tracker 列表源、自动更新时间和最后同步错误由 BangumiToday 管理，不通过本地引擎协议传输。引擎未收到 `seedingEnabled` 时使用安全默认值 `false`；BangumiToday 客户端负责在新安装且用户已确认提示后显式传入产品默认值 `true`、`2.0` 和 `60`。

## 错误

JSON-RPC `error.data` 至少包含稳定的业务 `code` 与 `retryable`。调用方不得依赖面向用户的 `message` 做分支判断。

```json
{"jsonrpc":"2.0","id":"2","error":{"code":-32011,"message":"the torrent already exists at this save path","data":{"code":"DUPLICATE_TASK","retryable":false,"taskId":"..."}}}
```

常用业务码包括 `NOT_INITIALIZED`、`PROTOCOL_MISMATCH`、`INVALID_CONFIG`、`INVALID_PROXY`、
`INVALID_PAGINATION`、`SOURCE_INVALID`、`SOURCE_UNSUPPORTED`、
`UNSAFE_TORRENT_PATH`、`METADATA_TIMEOUT`、`SAVE_PATH_INVALID`、
`SAVE_PATH_UNAVAILABLE`、`SAVE_PATH_NOT_WRITABLE`、`DISK_FULL`、
`STORAGE_ERROR`、`DATA_VERIFICATION_FAILED`、`NETWORK_UNAVAILABLE`、
`HTTP_STATUS_ERROR`、`HTTP_REDIRECT_ERROR`、`HTTP_RESUME_UNSUPPORTED`、
`TLS_ERROR`、`TARGET_FILE_EXISTS`、`DUPLICATE_TASK`、`TASK_NOT_FOUND`、
`TASK_UNAVAILABLE`、`PERSISTENCE_ERROR`、`TORRENT_ERROR`、
`HTTP_TRANSFER_ERROR` 和 `INTERNAL_ERROR`。任务快照的 `lastError` 使用同一组
业务码和 `retryable` 语义。

## 事件

`event.taskAdded`、`event.taskUpdated`、`event.taskRemoved` 的 `params.sequence` 在单次引擎进程内单调递增。客户端发现序号缺口时应调用 `task.list` 重取全量快照。
