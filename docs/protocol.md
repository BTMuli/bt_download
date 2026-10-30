# bt_download 本地调用契约

协议版本：`1.9`。传输为继承的 stdin/stdout 管道，编码为 UTF-8，每个 JSON-RPC 2.0 对象独占一行。单帧最大 1 MiB。客户端必须同时持续读取 stdout 和 stderr；stdout 不包含日志。

Windows 系统错误先从 Unicode 消息转换为 UTF-8，保留本地化内容。远端或未知来源的显示
文本仍可能含非法 UTF-8 字节，输出 JSON 和持久化目录时将这些字节替换为 U+FFFD，
保留合法 UTF-8、有效条目和业务字段，避免整帧失败并误报 `INVALID_PARAMS`。
输入 JSON 仍严格解析，不放宽客户端必传字段或类型约束。

客户端与引擎始终同版本发布，`engine.initialize` 的 `protocolVersion` 必须与引擎协议版本严格一致（`1.9`），不一致返回 `PROTOCOL_MISMATCH`；不再保留旧协议协商或回退路径。

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
| `task.peers` | `id`, 可选 `offset`/`limit`/`trackerUrl` | 返回分页 Peer，可按具体 Tracker 筛选 |
| `task.trackers` | `id`, 可选 `offset`/`limit`/`peerEndpoint` | 按本地接口及 v1/v2 返回 Tracker，可按关联 Peer 筛选 |
| `task.reannounce` | `id` | 请求任务重新汇报，遵守 Tracker 的最小间隔 |
| `task.setFilePriorities` | `id`, `priorities` | 更新多文件任务的单个或多个文件优先级 |
| `task.pause` / `task.resume` | `id` | 持久暂停或继续 |
| `task.stop` | `id` | 停止下载与上传，保留数据及记录，状态为 `stopped`；可用 `task.resume` 恢复 |
| `task.retry` / `task.recheck` | `id` | 重试错误或强制校验 |
| `task.remove` | `id`, 可选 `deleteData` | 默认只移除任务 |

### 强制重校验语义

`task.add` 可传 `manual: true` 标记手动添加的任务，默认 `false`；所有任务快照均返回
`manual` 布尔字段，并持久化到任务目录。旧目录缺少该字段时迁移为 `false`，不猜测来源。
手动任务不执行后台文件存在性探测；做种策略结束进入 `completed` 后保留为归档记录，
重启时不读取下载文件或恢复 torrent handle，不提供重新校验。归档前仍按原有做种设置上传，
上传本身需要访问文件。已归档任务可移除，默认保留文件。

`stopped` 与 `paused` 都禁止自动恢复；停止不表示下载完成。已完成任务停止为无操作。

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
检查只覆盖已选择的负载文件（优先级 0 的未选文件不参与），并按与 libtorrent 一致的
Windows 扩展长度路径访问文件，因此超过 `MAX_PATH` 的深目录负载不会被误判为缺失。

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

配置字段以字节/秒、字节、秒、分钟和计数为单位：`activeDownloads`、`downloadRateLimit`、`uploadRateLimit`、`connectionsLimit`、`connectionsPerTask`、`metadataTimeoutSeconds`、`additionalTrackers`、`seedingEnabled`、`seedRatioLimit`、`seedTimeLimitMinutes`、`conserveOnMeteredOrLowPower`、`constrainedUploadRateLimit`、`constrainedUploadTotalLimit`。速率 `0` 表示不限速；Magnet 元数据超时默认 300 秒，取值范围为 1 至 86400 秒。

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
- BangumiToday 默认传入 `conserveOnMeteredOrLowPower=true`，引擎独立启动时默认值为 `false`。系统报告按流量计费或节能模式时，BT 任务下载完成即停止做种；全局上传速率取 `uploadRateLimit` 与 `constrainedUploadRateLimit` 中较严格的非零值（后者默认 4 MiB/s）；单个 BT 任务在下载期间累计上传达到 `constrainedUploadTotalLimit`（默认 200 MiB）后暂停。两个受限值分别以字节/秒和字节计，`0` 表示不设该限制。检测每五秒刷新一次，可能在阈值之后产生少量上传。
- 配置整体验证并原子生效，非法配置返回 `INVALID_CONFIG`；
- 任务状态包含 `seeding`，快照包含 `uploadedBytes`、`shareRatio`、`seedingSeconds`、`seedRatioLimit`、`seedTimeLimitMinutes`、`seedStopReason` 和 `pauseReason`。受限状态停止做种时 `seedStopReason=constrained`；累计上传上限暂停时 `pauseReason=constrainedUploadTotalLimit`；
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
- 文件名优先使用最终响应的 `Content-Disposition`（`filename*` 优先于
  `filename`），其次使用重定向后的 URL 最后一个路径段，最后回退到初始 URL
  路径段；百分号解码后替换 Windows 非法字符并规避设备保留名；目标已存在时
  使用 `name (1).ext` 形式创建新名称，不覆盖用户文件；
- 任务添加时先按初始 URL 预留名称。收到探测或首个响应头后，若解析到更好的
  文件名则更新目标名，并在 `displayName` 仍是预留名时同步更新；用户传入的
  `displayName` 不会被覆盖；
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
{"id":"2","jsonrpc":"2.0","result":{"task":{...},"pieceLength":16384,"pieceCount":1024,"completedPieces":"0101...","totalFiles":42,"contentFiles":31,"totalPeers":37,"httpConnections":0,"httpRangeSupported":null,"httpTransferMode":"unknown","httpResponseStatus":null,"files":[],"filesTruncated":false,"peers":[],"peersTruncated":false}}
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
- BT 元数据不可用时仅文件与分片信息为空；`task.peers` 仍返回连接中、握手中和已连接的
  节点与已知身份，`task.details.totalPeers` 使用当前已连接节点计数；HTTP 任务按单文件返回；
- 两个列表均为当前时刻的快照。Peer 列表先按端点排序，连接变化时仍可能移动窗口，
  不提供跨请求的一致快照。BangumiToday 文件页按排除 padding 的真实文件每页 50 个展示，
  缓存真实文件到原始索引的映射，刷新当前页对应的引擎窗口；Peer 每页 100 个，刷新当前页，
  Peer 当前页失效时回到首页；排序、客户端筛选及批量文件选择均只作用于本页。
  文件选择使用原始全局索引映射，隐藏 padding、按真实文件分页和排序后仍保留该索引；

Tracker 列表源、自动更新时间和最后同步错误由 BangumiToday 管理，不通过本地引擎协议传输。引擎未收到 `seedingEnabled` 时使用安全默认值 `false`；BangumiToday 客户端负责在新安装且用户已确认提示后显式传入产品默认值 `true`、`2.0` 和 `60`。

### Peer 客户端身份

`task.peers` 的每个条目包含 `endpoint`、`userAgent`、`peerId`、`peerIdClient`、
`progress`、`downloadRate` 和 `uploadRate`。身份字段按协议必传，不再返回混合来源的
`client` 字段：

- `userAgent`：独立捕获的 BEP 10 扩展握手 `v` 字段。未收到非空声明时为空字符串，
  不以 Peer ID 识别结果补齐；它不是对端向网站或 Tracker 发送的 HTTP UA。
- `peerId`：BT 基础握手中 20 字节 Peer ID 的 40 位小写十六进制编码，保留原始字节；
  尚未获得 Peer ID 时为空字符串。
- `peerIdClient`：仅通过 Peer ID 编码约定识别的客户端名称与版本；尚未获得 Peer ID
  时为空字符串，未识别格式为 `unknown`。支持 BEP 20 中常见客户端代码，以及 aria2
  的 `A2-MAJOR-MINOR-PATCH-` 格式。识别结果不等同于对端真实软件身份。

```json
{"endpoint":"127.0.0.1:6881","userAgent":"BangumiToday/0.8.0","peerId":"2d4c54323043302d303132333435363738396162","peerIdClient":"libtorrent 2.0.12","progress":0.5,"downloadRate":1024,"uploadRate":0,"choked":true,"interested":true,"remoteChoked":false,"remoteInterested":false,"connectionState":"connected","seed":false,"transport":"tcp","encryption":"none","sources":["tracker"],"trackerIndices":[0]}
```

扩展握手观察器随新建及恢复的 BT 任务注册，只记录声明，不修改 Peer ID、扩展协商或
传输行为。声明随连接销毁，重连时重新收集，Web Seed 不会被当作 BT 客户端声明。

### Peer 连接诊断（1.7）

Peer 条目还必须包含 `choked`（本端阻止对端下载）、`interested`（本端感兴趣）、
`remoteChoked`（对端阻止本端下载）、`remoteInterested`、`seed`，类型均为 bool。
`connectionState` 为 `connecting` / `handshake` / `connected`，
`transport` 为 `tcp` / `utp` / `webSeed`，`encryption` 为
`none` / `handshake`（仅握手混淆）/ `rc4` / `tls`。
`sources` 为发现来源字符串数组，可包含 `tracker`、`dht`、`pex`、`lsd`、
`resumeData`、`incoming`，无来源信息时为空数组。

### 任务 Tracker（1.7）

`task.trackers` 返回 `{"trackers":[...],"totalTrackers":N,"offset":0,"nextOffset":null}`，
HTTP 任务返回空数组、总数为 0。窗口参数校验与文件/Peer 相同，默认及最大 limit 为 100，
避免大量 Tracker 的重复 URL 与接口状态超过单帧上限。分页按当前采样展开后的行数计数。
每行代表一个 Tracker 的一个本地监听接口及一个有效的 BT 协议版本；尚无接口时
提供一行等待状态，此时 `endpoint` / `protocol` 为空。字段全部必传：

- `url` 为 Tracker 服务器地址；`tier` 为层级；`endpoint` 为本地汇报接口的地址及监听端口，
  不是 Tracker 服务器地址；`protocol` 为 `v1` / `v2` / 空。
- `state`：`waiting` / `updating` / `working` / `error` / `paused` / `disabled`。
- `lastError`：最后错误文本，无错误时为空；`message`：Tracker 返回的警告或消息。
- `nextAnnounceSeconds`：采样时距下次汇报的秒数（同时考虑最小间隔），未安排或暂停时为 null。
- `peers`：该接口和协议版本最近一次 Tracker 响应返回的节点数，尚无响应时为 null。
  该值不代表新连接数，也不使用 scrape 的种子数代替。

`task.reannounce` 返回 `{"scheduled":true}`，表示请求已排入 libtorrent；不会绕过
最小间隔，不保证立即收到响应。HTTP 返回 `SOURCE_UNSUPPORTED`，暂停任务返回 `TASK_PAUSED`。
查询和重汇报不会添加 Tracker，沿用现有隐私种子及磁力元数据阶段的 Tracker 隔离规则。

### Tracker 与 Peer 关联（1.8）

`task.peers` 顶层必传 `trackerUrls` 字符串数组，同页各 Peer 的 `trackerIndices` 必传整数
数组，引用此响应内的 URL 字典；索引仅在当前响应有效。客户端解析成每个 Peer 独立的
URL 列表，不以通用的 `sources: ["tracker"]` 或连接时间猜测具体来源。HTTP/空页的字典
和各 Peer 无记录时的索引均为空数组。上方 Peer 示例的索引 0 引用顶层字典的首个 URL。

引擎通过随包 libtorrent overlay 的观察回调，记录真实 compact IPv4/IPv6 Tracker
响应，以及非 compact 节点异步地址解析完成后的原请求 URL；不额外发起汇报或改变节点
选择。一个 Peer 可以关联多个 Tracker，这表示该 Tracker 近期返回过该地址，不表示
它是首次发现来源、唯一来源或当前传输的中转服务器。入站连接的临时端口、DHT/PEX
节点及重启前的历史可能没有可匹配记录，空数组不表示这些节点从未来自 Tracker。

每任务记录最多 20,000 个“地址 + URL”组合，按每条最后返回时间保留 30 分钟；重复返回
会刷新时间与 LRU 顺序，达到上限淘汰最久未返回的记录。不持久化，移除任务/关闭引擎
清理；从任务移除的 Tracker 不再作为有效关联返回。URL 字典避免逐 Peer 重复传输地址。

`task.peers.trackerUrl` 按完整 URL 精确匹配，在计数和分页之前筛选；`totalPeers`、
`nextOffset` 与 `peersTruncated` 均描述筛选后的窗口。`task.trackers.peerEndpoint`
按 `task.peers.endpoint` 的原始地址精确匹配，先筛 Tracker URL，再展开接口/协议和分页。
不传筛选参数时沿用全量查询，未知或过期关联返回空页。界面支持 Tracker 详情跳转到
关联 Peer、Peer 行跳转到关联 Tracker，以及返回和清除筛选；关联筛选跨页生效。

新增回调改变 libtorrent 插件接口，必须使用 `vcpkg-configuration.json` 的 overlay
同时重建引擎和依赖 DLL，不能复用未打补丁的 libtorrent。详见
[overlay 说明](../vcpkg-ports/README.md)。协议仍按同版本发布，不增加旧协议回退。

### HTTP 实际传输状态（1.7）

`task.details` 必传以下字段（BT 任务固定为 null / `unknown` / null）：

- `httpRangeSupported`：bool 或 null。实际响应确认 Range 支持或拒绝；尚未确认时为 null。
- `httpTransferMode`：`probing`、`range`、`resuming`、`sequential`、`restart`、`unknown`。
  `restart` 表示本次传输已从区间下载或续传降级为从头下载，资源版本变化也可能触发它。
- `httpResponseStatus`：最近结束请求的最终 HTTP 状态码；尚未收到响应时为 null。
  探测尚未结束时可报告当前最终响应头，重定向的中间状态不会作为最终值展示。

暂停、出错和完成后保留本进程最近一次诊断，活动连接数归零；删除任务清理诊断缓存。

### 单任务下载耗时（1.9）

所有任务快照必传非负整数 `downloadMilliseconds`，表示该任务累计记录的下载毫秒数，
与预计剩余时间、做种时间分别存储。只有 `state=downloading` 且 `downloadRate>0` 时
计时；速度为零、暂停、停止、排队、校验、做种、完成和错误期间不增长。重试及继续
沿用同一任务累计值，删除任务时清理。

引擎按单调时钟消费互不重叠的采样区间，保留不足一毫秒的余数；快照采样约每 500 ms
一次。查询、事件和保存共用同一个任务计时器，不按刷新次数追加固定时长。状态切换时
结算上一段活跃区间，应用关闭期间不计入；重启只恢复累计值，运行期时钟锚点不写磁盘。

累计值写入 catalog schema 4。正常退出在等待 resume 数据前保存，下载期间每 5 秒
检查一次脏数据并原子保存；强制终止可能丢失最近一次检查点后的增量。schema 1–3
迁移将此前未记录的耗时初始化为 0，不根据任务年龄或下载字节推算历史耗时。前端将
累计值显示在单任务卡片的预计耗时旁及详情信息页，不自行启动计时或累加。
跨进程恢复时，已有有效区间状态可证明此前使用过 Range；其余未知诊断不会推断为成功。

### 引擎网络诊断（1.7）

`engine.status.network`（初始化响应也提供）包含以下必传字段：

- `listening`、`listenPort`：当前实际监听状态和端口。
- `dhtEnabled`、`dhtRunning`、`dhtNodes`（未运行时 null）、`dhtSampled`：生效设置、
  运行状态、各本地 DHT 实例路由表的活动节点数合计及是否已采样。异步采样约每 5 秒刷新。
- `lsdEnabled`、`upnpEnabled`、`natPmpEnabled`：libtorrent 当前生效设置。
- `listeners`：最近监听结果，含 `address`、`port`、`socketType`（可读字符串）、
  `state`（`listening` / `error`）、`error`。
- `portMappings`：最近端口映射结果，含 `address`、`transport`（`upnp` / `natPmp`）、
  `state`（`mapped` / `error`）、`externalPort`（失败时 null）、`error`。

监听或端口映射成功不能证明公网入站已可达；查询不发起公网探测。代理配置变化会清理旧的
DHT 与映射采样，代理关闭的发现和映射能力按生效设置显示。前端在下载设置中按需查询。

`task.retry` 只允许 `state=error` 且 `lastError.retryable=true` 的任务；否则分别返回
`TASK_NOT_FAILED` 或 `TASK_NOT_RETRYABLE`。可重试不代表错误条件已消失：磁盘空间、
权限、目录或网络需要先恢复。修改源或保存路径通过重新添加任务完成，重试不修改它们。

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
