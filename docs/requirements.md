# BangumiToday 内置 BT 下载引擎需求分析

> 状态：提案（Proposed）  
> 分析基线：BangumiToday `v0.8.0`（提交 `ac798cf`，2026-08-01）  
> 首期目标平台：Windows 10/11 x64  
> 本文中的“BT 下载”指根据 `.torrent` 元数据或 Magnet URI 下载其指向的内容，不是仅把 `.torrent` 文件保存到本地。

## 1. 结论

建议将本仓库建设为一个**随 BangumiToday 打包的无界面伴随进程**：

- 下载核心采用 libtorrent，并通过本仓库自有的窄接口隔离其 C++ API；
- BangumiToday 使用 `Process.start` 启动 `bt_download.exe`，通过标准输入/输出上的 JSON-RPC 调用；
- 引擎负责 BT 协议、任务生命周期、断点恢复和资源限制；BangumiToday 负责 RSS、目录选择、任务展示和用户交互；
- 默认与 BangumiToday 同生命周期，不注册系统服务、不开放远程端口、不要求管理员权限；
- 首期替代 Motrix 的核心链路，同时移除 BT 任务对 Flutter Widget 生命周期的依赖。

这仍属于“内置下载”：用户不会安装或配置另一个下载软件，但资源密集型和不可信网络处理被隔离在应用 UI 进程之外。

## 2. 背景与现状

### 2.1 当前调用链

BangumiToday 当前的大致流程为：

```text
RSS 条目
  -> BTDownloadTool.downloadRssTorrent() 保存 .torrent 元数据
  -> 调用 mo://new-task 并打开 .torrent 文件
  -> Motrix 解析种子并下载实际内容到 BMF 目录
```

因此，现有 `BTDownloadTool` 名称容易产生误解：它只下载 `.torrent` 元数据，实际负载由 Motrix 下载。

### 2.2 已有内置实现及问题

主项目仍保留基于 `dtorrent_parser`、`dtorrent_task` 的调试版内置下载代码，入口受 `kDebugMode` 限制。现有实现存在以下结构性问题：

- 每张 `RssDownloadCard` 自行创建和持有一个 `TorrentTask`，Widget 销毁时直接停止任务，下载生命周期与页面生命周期耦合；
- 多任务各自维护监听器、DHT、Tracker 和 UI 高频刷新，资源无法在全局统一调度；
- Hive 中只保存 RSS 标题、链接和目录，缺少稳定任务 ID、info-hash、精确状态、错误、文件列表和恢复版本；
- 完成后延迟删除任务和状态文件，导致“历史记录”“完成态”和“断点恢复”语义混杂；
- 暂停、停止、重新开始和删除数据的语义不完整，异常退出后的恢复不可验证；
- 去重依赖 `MiniRssItem` 对象比较，不能可靠表达同一 info-hash、不同保存目录等场景；
- BT 的网络、磁盘和哈希工作与 Flutter 应用处于同一故障域。

项目历史也印证了这些问题：`v0.4.0` 因下载严重消耗性能而隐藏内置入口，`v0.6.0` 又因媒体播放与下载存在重大问题而移除相关功能；目前重新加入的实现仍只在调试模式可见。

### 2.3 约束

- BangumiToday 是 Flutter 桌面应用，当前正式分发重点为 Windows 和 MSIX/Microsoft Store；
- 当前 RSS/BMF 已具备 `.torrent` URL 获取、Mikan 镜像改写和用户下载目录配置；
- 新引擎必须随应用分发，不能要求 Motrix、aria2 或其他客户端已安装；
- BT 工作负载可能长时间运行，并大量使用网络、磁盘、内存和哈希计算；
- 网络输入、种子内部路径和 Tracker 响应均应按不可信数据处理；
- 下载和分发第三方内容的合法性由用户及内容来源决定，产品不能暗示规避版权、网络或组织政策。

## 3. 目标与非目标

### 3.1 目标

- 在未安装 Motrix 的干净系统上完成从 RSS 条目到负载文件落盘的全流程；
- 支持 `.torrent` 文件和 Magnet URI；
- 支持添加、查询、暂停、继续、重试、校验和移除任务；
- 应用或引擎重启后可恢复未完成任务，不重复下载已校验的数据；
- 下载引擎崩溃不应带崩 Flutter UI，UI 能识别故障并重启引擎；
- 提供全局并发、上传/下载速率、连接数和磁盘使用方面的限制；
- 提供稳定、可版本化、可自动测试的本地调用契约；
- 能被可靠地打入现有 Windows 构建和 MSIX 包，并通过打包安装场景验证。

### 3.2 首期非目标

- 通用 BT 客户端、独立 GUI、托盘程序或 Web 管理界面；
- 对局域网或互联网开放 RPC；
- 种子搜索、RSS 订阅、Bangumi/BMF 数据管理；
- 边下边播、顺序下载优化、远程控制；
- 匿名网络、代理规则编辑器、IP 过滤器管理；
- 应用退出后继续后台下载；
- macOS、Linux、移动端正式支持；
- 按任务/分类覆盖做种策略、无限做种、最小做种时间和高级上传队列规则。

## 4. 用户场景

### 4.1 从 RSS 下载

1. 用户在 RSS 或 BMF 页面点击“下载”。
2. BangumiToday 确定 BMF 下载目录，并沿用现有镜像规则获取 `.torrent` 文件。
3. 应用将种子文件路径和目标目录提交给引擎。
4. 引擎校验输入、解析元数据、按 info-hash 去重并创建任务。
5. UI 立即显示任务状态，随后展示进度、速度、Peer 数和错误。
6. 文件完整校验后 UI 发出一次“文件可用”通知，并允许用户打开目录或文件。
7. 若已启用限量做种，任务进入 `seeding`；满足分享率或时间条件后再进入 `completed`，不重复发送下载完成通知。

### 4.2 从 Magnet 下载

1. 应用提交 Magnet URI 和目标目录。
2. 引擎先进入 `metadata` 状态，通过 DHT/Tracker/Peer 获取元数据。
3. 元数据可用后，引擎进行磁盘空间检查并转入排队或下载状态。
4. 元数据超时必须形成可重试错误，不得让任务永久停留在不透明状态。

### 4.3 中断恢复

1. 下载进行中，BangumiToday 正常退出或进程异常终止。
2. 引擎尽最大努力保存 fast-resume 数据；Windows Job Object 保证伴随进程不会成为孤儿进程。
3. 下次启动时，任务目录和 resume 数据由引擎恢复。
4. UI 从引擎重新拉取完整快照，不以旧的 Hive 下载列表作为事实来源。

### 4.4 更新补充 Tracker

1. 用户在设置页选择一个或多个 Tracker 文本列表源，也可维护手工 Tracker。
2. 用户手动刷新，或应用发现距上次成功刷新已满 24 小时后自动刷新。
3. 应用限制并解析各源响应，允许部分成功，将手工项与最后成功远程快照合并后下发给引擎。
4. 引擎二次校验，只把补充 Tracker 应用于公共任务；私有种子和尚未确认公开属性的 Magnet 不注入。
5. 全部刷新失败时继续使用上次成功快照，并在设置页展示可重试错误，不中断现有任务。

### 4.5 限量做种

1. 文件完整校验后，启用做种的任务进入 `seeding`，并立即通知用户文件已经可用。
2. 引擎累计上传字节与实际做种时间；暂停和引擎未运行期间不累计时间。
3. 分享率达到 `2.0` 或做种达到 `60` 分钟时停止网络活动，保存 resume 并进入 `completed`。
4. 暂停、退出或崩溃恢复后继续使用既有累计值；运行时修改限制只重新评估活动做种任务，不自动重启已完成任务。

详细设置模型、私有种子规则和验收用例见 [Tracker 与限量做种需求](tracker-and-seeding.md)。

## 5. 功能需求

优先级定义：P0 为替代 Motrix 上线所必需；P1 为首个稳定版本建议具备；P2 为后续增强。

| ID | 优先级 | 需求 | 验收要点 |
| --- | --- | --- | --- |
| FR-001 | P0 | 引擎生命周期 | 应用可启动、握手、查询版本、优雅关闭并在崩溃后重启引擎；同一用户会话只允许一个受控实例。 |
| FR-002 | P0 | 添加来源 | 接受绝对路径 `.torrent` 文件和合法 Magnet URI；无效 bencode、缺少 info-hash、超大元数据或不支持的协议返回稳定错误码。 |
| FR-003 | P0 | 创建任务 | 输入至少包含 `source`、`savePath` 和可选展示名；返回独立于 RSS 条目的稳定 UUID、info-hash（可用后）与任务快照。 |
| FR-004 | P0 | 状态机 | 至少覆盖 `metadata`、`checking`、`queued`、`downloading`、`seeding`、`paused`、`completed`、`error`；状态转换可重放且不会因 UI 页面切换而改变。 |
| FR-005 | P0 | 任务控制 | 支持暂停、继续、重试、强制校验和移除；暂停是可持久恢复状态。 |
| FR-006 | P0 | 进度查询 | 快照包含总大小、已完成/验证/上传字节、进度、分享率、做种时间、上下行速度、Peer/Seed 数、保存路径和最后错误。 |
| FR-007 | P0 | 事件推送 | 引擎主动推送任务增删改和完成事件；同一任务的普通进度事件最多 2 次/秒，关键状态变化不节流。 |
| FR-008 | P0 | 持久化恢复 | 任务目录与 libtorrent fast-resume 数据写入可写的 AppData；写入需原子化或具备事务保护，损坏单个任务不能阻止引擎启动。 |
| FR-009 | P0 | 去重 | 以 info-hash 与规范化保存路径作为主要去重键；重复添加返回已有任务或 `DUPLICATE_TASK`，不能启动第二份隐式下载。 |
| FR-010 | P0 | 路径与空间校验 | 目标目录必须为绝对、可写路径；元数据可用后检查磁盘空间；拒绝种子中的绝对路径、盘符、`..` 越界及其他目录穿越。 |
| FR-011 | P0 | 完整性 | 所有完成数据必须通过种子声明的 piece hash 校验；校验失败的数据不能计入完成。 |
| FR-012 | P0 | 网络发现 | 支持 HTTP/HTTPS/UDP Tracker、DHT、PEX、LSD、TCP/uTP 中由所选引擎稳定提供的能力；私有种子必须遵守其限制并禁用不允许的发现方式。 |
| FR-013 | P0 | 上传与做种告知 | 下载期间允许受限上传；文件完成后按限时/限比策略做种。首次启用前说明 IP 暴露和上传流量，升级用户不得静默改变既有完成即停止行为。 |
| FR-014 | P0 | 删除语义 | “移除任务”默认保留已下载数据；删除数据必须使用单独参数并由 UI 二次确认，且只能删除该任务解析出的文件。 |
| FR-015 | P0 | 错误模型 | 错误至少区分来源无效、元数据超时、目录不可写、磁盘不足、重复任务、网络不可用、数据校验失败和内部错误；包含可重试标记。 |
| FR-016 | P0 | 全局配置 | 支持活动下载数、下载限速、上传限速、全局/单任务连接数、补充 Tracker 和做种停止策略；配置变更可在运行中生效并持久化。 |
| FR-017 | P1 | 文件选择 | 多文件种子可在元数据可用后选择文件和优先级；未选择时默认下载全部文件。 |
| FR-018 | P1 | 完成历史 | 保留有限数量的已完成任务，便于查看和再次校验；清理历史不等同于删除数据。 |
| FR-019 | P0 | Tracker 补充与更新 | BangumiToday 可从用户选择的文本列表源每日更新并合并手工 Tracker，引擎负责二次校验、去重和应用；同步失败保留最后成功快照，私有种子禁止注入公共 Tracker。 |
| FR-020 | P1 | URL 直传 | 引擎可选支持直接接收 HTTP/HTTPS `.torrent` URL；P0 阶段仍可复用 BangumiToday 当前的 Dio 下载与镜像改写。 |
| FR-021 | P0 | 限量做种策略 | 新安装默认做种至分享率 `2.0` 或 `60` 分钟任一先到即停；状态、累计上传、累计做种时间和停止原因可查询并跨重启恢复。 |
| FR-022 | P2 | 跨平台 | 在不改变上层调用契约的前提下增加 macOS/Linux 构建。 |

## 6. 非功能需求

### 6.1 性能与资源预算

以下为首轮性能基线，技术验证后可依据真实设备修订，但不能取消可量化约束：

- 引擎空闲 5 分钟后的平均 CPU 使用率低于一个逻辑核心的 1%，无轮询忙等；
- 无任务时工作集目标不超过 80 MiB；默认两个活动任务、64 MiB 级磁盘缓存时，稳定工作集目标不超过 250 MiB；
- 除首次哈希校验、磁盘满载和系统调度抖动外，控制命令本地响应 P95 小于 100 ms；
- 冷启动到 `ready` 事件目标小于 2 秒，100 个持久任务的目录恢复目标小于 5 秒，不含必要的数据重校验；
- 默认最多 2 个活动下载任务，哈希/磁盘线程数和连接数必须受控；
- UI 不直接处理 Peer 或 piece 级事件，进度事件必须聚合，避免 Riverpod/Widget 高频重建；
- 性能验收同时采集 BangumiToday UI 帧耗时、引擎 CPU、工作集、磁盘吞吐和事件频率。

### 6.2 稳定性

- 单个损坏种子、异常 Tracker 或不可访问目录不得使引擎进程退出；
- 引擎异常退出不得使 BangumiToday 退出；应用应显示“下载引擎不可用”并允许重启；
- 每次关键状态变化以及固定周期内保存 resume 数据，正常关闭时执行最终保存；
- stdout 只允许输出协议帧，日志写入 stderr；BangumiToday 必须持续消费两条流，防止管道写满造成死锁；
- 任务事件携带单调递增序号；客户端检测到序号缺口后重新拉取全量快照；
- 协议请求必须支持超时，重复控制请求应尽量幂等。

### 6.3 安全与隐私

- 首期只允许父进程通过继承的 stdio 管道调用，不监听 TCP/UDP 管理端口；BT 协议本身所需监听端口除外；
- 引擎以当前用户权限运行，不提权、不注册 Windows 服务、不写安装目录；
- 将 `.torrent`、Magnet、Tracker、Peer 消息和文件名视为不可信输入，限制长度、集合数量和嵌套深度；
- 对目标目录和种子内部路径做规范化校验，删除操作还要验证最终路径仍位于该任务根目录；
- 日志不得记录完整 Magnet URI、带查询参数的 RSS/Tracker URL、私有 Tracker passkey 或用户目录之外的敏感信息；
- UI 首次启用时应说明 BT 会向其他 Peer 暴露用户 IP，并产生上传流量；
- 依赖版本必须锁定，生成第三方许可清单；安全更新不能依赖运行时下载新的可执行代码。

### 6.4 兼容与分发

- P0 支持 BangumiToday 当前的 Windows x64 Debug、Release 和 MSIX 安装构建；
- `bt_download.exe` 及其运行库必须随包提供，干净系统无需安装编译器、Python、Boost、Motrix 或 VC 开发环境；
- 可执行文件从包内只读位置启动，状态和 resume 文件写入 LocalAppData；
- 必须验证未打包运行、侧载 MSIX、升级安装和 Microsoft Store 构建四种场景；
- 原生依赖的 ABI、编译参数和许可证通知进入发布产物；引擎版本与 BangumiToday 版本可独立查询。

### 6.5 可观测性与可测试性

- 日志使用结构化字段：时间、级别、组件、任务 ID、错误码；默认不输出 Peer 逐包日志；
- 提供 `engine.status`，返回协议版本、引擎版本、libtorrent 版本、运行时间、任务统计和资源配置；
- 协议层、任务状态机、持久化迁移和路径校验需要单元测试；
- 集成测试使用自建本地 Tracker/Seeder 和可自由分发的小文件，不依赖公开动漫种子；
- E2E 覆盖添加、暂停、退出、恢复、完成、去重、磁盘不足、引擎崩溃和删除确认；
- 性能基准结果应作为发布前检查项保存，避免再次出现“功能可用但 UI/系统不可用”的回归。

## 7. 推荐架构

```text
┌──────────────────── BangumiToday / Flutter ────────────────────┐
│ RSS/BMF UI -> BtEngineClient -> Riverpod task projections      │
│                     │ stdin/stdout JSON-RPC                    │
└─────────────────────┼──────────────────────────────────────────┘
                      │ child process, same user
┌─────────────────────▼ bt_download.exe ─────────────────────────┐
│ Protocol adapter -> Task service -> libtorrent facade/session  │
│                         │                    │                  │
│                  task catalog/resume       payload files       │
│                    (LocalAppData)      (user-selected folder)  │
└────────────────────────────────────────────────────────────────┘
```

### 7.1 进程边界

选择伴随进程而不是 Flutter 进程内库，主要是为了：

- 隔离原生库崩溃、网络解析异常和高 CPU/磁盘负载；
- 让任务属于全局引擎，而不是某个页面或 Widget；
- 独立测量、限制和重启下载引擎；
- 将来增加其他桌面平台时保留同一上层协议。

默认使用 `ProcessStartMode.normal`。BangumiToday 同时读取 stdout 与 stderr；引擎检测 stdin EOF 并保存后退出。Windows 端还应把子进程放入带 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` 的 Job Object，覆盖父进程被强杀、Dart 未执行清理逻辑的情况。

### 7.2 调用协议

建议使用 JSON-RPC 2.0 的请求/响应结构，每个 UTF-8 JSON 对象占一行（NDJSON）：

```json
{"jsonrpc":"2.0","id":"01J...","method":"task.add","params":{"source":{"kind":"torrentFile","path":"C:\\...\\a.torrent"},"savePath":"D:\\Anime","start":true}}
```

```json
{"jsonrpc":"2.0","id":"01J...","result":{"task":{"id":"...","state":"queued","infoHash":"..."}}}
```

```json
{"jsonrpc":"2.0","method":"event.taskUpdated","params":{"sequence":42,"task":{"id":"...","state":"downloading","downloadedBytes":1048576}}}
```

协议要求：

- 首个引擎消息为 `event.ready`，携带协议版本和引擎版本；
- BangumiToday 随后调用 `engine.initialize`，传入状态目录和全局配置；
- 所有请求均有 ID、超时和稳定错误码；未知字段向前兼容，破坏性变更提升主协议版本；
- 普通进度通知可合并，完成、错误和删除通知不可丢弃；
- 单帧大小设置上限，日志或第三方库输出严禁混入 stdout；
- 如果以后需要应用退出后继续下载，再另行设计具备身份验证的命名管道或本地服务，不能直接把当前协议暴露到 localhost。

建议的 P0 方法：

| 方法 | 用途 |
| --- | --- |
| `engine.initialize` | 完成版本协商，设置状态目录和配置 |
| `engine.status` | 健康检查和版本/统计查询 |
| `engine.configure` | 更新限速、并发、连接限制、补充 Tracker 和做种策略 |
| `engine.shutdown` | 保存状态并优雅退出 |
| `task.add` | 从 `.torrent` 路径或 Magnet 创建任务 |
| `task.list` / `task.get` | 获取全量或单任务快照 |
| `task.pause` / `task.resume` | 暂停或继续任务 |
| `task.retry` / `task.recheck` | 重试错误或强制校验 |
| `task.remove` | 移除任务，可显式选择是否删除数据 |

### 7.3 状态所有权

- 引擎的任务目录是下载状态的唯一事实来源；
- BangumiToday 可保存“任务 UUID 与 Bangumi 条目/BMF 的关联”，但不能保存并回写引擎运行态；
- 每次连接或重连时，客户端先 `task.list` 获取快照，再消费增量事件；
- resume 文件以 info-hash 命名或建立索引，格式版本与引擎版本一并记录；
- 持久化升级必须有向前迁移策略，无法迁移时保留原文件并将单任务标记为可诊断错误。

### 7.4 libtorrent 封装边界

业务层不得直接暴露 `torrent_handle`、alert 类型或 libtorrent 枚举。封装层只输出本项目的 Task、Settings、Statistics 和 Error 模型。这样可以：

- 固定对 BangumiToday 的接口；
- 独立升级或替换底层引擎；
- 集中处理 alert 聚合、resume 保存、私有种子策略和路径安全；
- 在协议层使用假引擎完成确定性测试。

libtorrent 已提供 DHT、Magnet 元数据交换、PEX、LSD、uTP、限速、多线程磁盘 I/O 和 fast-resume 等所需基础能力，适合把工程重点放在生命周期、接口和产品约束，而不是重新实现 BT 协议。

## 8. BangumiToday 集成改造

主项目需要配合完成以下改造，但不在本仓库 P0 引擎实现范围内：

1. 新增 `BtEngineClient`，负责定位/启动伴随进程、协议请求、超时、重启和事件解析。
2. 应用启动时把下载引擎作为可选服务初始化；失败不能阻塞 BangumiToday 主界面。
3. 保留现有 `BTDownloadTool.downloadRssTorrent` 作为 P0 的种子获取和镜像适配层，随后可重命名为 `TorrentMetadataDownloader`。
4. 将 RSS、Anibt 和 BMF 的下载按钮统一改为创建引擎任务，不再调用 `mo://` 或依赖 `.torrent` 文件关联。
5. 下载管理页改为渲染引擎快照；页面销毁只取消 UI 订阅，不暂停或停止任务。
6. Hive 只保留任务与 Bangumi/BMF 的业务关联；旧 `DttHiveModel` 数据需一次性迁移或清理，不能和新任务目录双向同步。
7. 设置页负责 Tracker 列表源、自动更新、最后成功快照和做种策略；只把解析后的 Tracker 与做种限制下发给引擎。
8. 文件校验完成即可通知和打开/播放；限量做种结束只更新任务状态，不重复发送下载完成通知。
9. 构建流程先编译/获取固定版本的 `bt_download.exe`，复制到 Flutter Windows Release 目录，再生成 MSIX；CI 校验文件、版本和第三方许可证均存在。

## 9. 默认产品策略

为避免实现阶段反复猜测，P0 按以下默认值推进；产品决定变化时应更新本文：

- 应用退出：暂停任务、保存 resume、退出引擎；不留后台进程；
- Tracker：默认提供 Motrix 推荐来源的可关闭候选，距上次成功更新满 24 小时自动刷新；失败保留最后成功快照，公共列表不注入私有种子；
- 完成后做种：新安装默认分享率 `2.0` 或 `60` 分钟先到即停，受全局上传限速控制；从旧版本升级时保持关闭，直到用户明确启用；
- 活动下载：默认 2 个，其余排队；
- 下载限速：默认不限速；上传限速：提供保守默认值并允许用户修改；
- 多文件种子：P0 下载全部，P1 提供文件选择；
- 移除任务：默认保留数据和 `.torrent` 元数据；删除数据需要明确选择和二次确认；
- 同一 info-hash、同一保存目录：视为同一任务；不同保存目录允许由用户明确创建另一任务；
- 完成历史：首版至少保留到用户手动清理，后续增加数量或时间上限；
- 平台：先保证 Windows x64；接口不写死盘符和反斜杠，为后续跨平台保留空间。

## 10. 方案比较

| 方案 | 优点 | 主要问题 | 结论 |
| --- | --- | --- | --- |
| 继续调用 Motrix | 成熟、改动小 | 需要外部安装、协议关联和独立任务管理，不满足需求 | 排除 |
| 在 Flutter UI 进程继续使用纯 Dart `dtorrent_task` | 可复用现有代码，无原生构建 | 历史上已有性能问题；任务与 UI 生命周期耦合；协议成熟度、资源控制和崩溃隔离不足 | 不作为正式方案 |
| 在 Flutter 进程内通过 FFI 调用 libtorrent | 成熟引擎、无伴随进程协议 | C++ 崩溃影响主应用；回调、线程和生命周期跨 FFI 复杂；仍与 UI 同故障域 | 备选，不推荐 P0 |
| 随包分发 libtorrent 伴随进程 | 用户零安装；成熟协议能力；进程隔离；接口可测试、可演进 | 增加原生构建、IPC、MSIX 打包和进程监管工作 | 推荐 |
| 自研 BT 协议栈 | 完全可控 | 工作量和安全风险极高，重复实现 DHT/uTP/PEX/resume 等成熟能力 | 排除 |

## 11. 里程碑

### M0：技术验证

- 构建固定版本的 libtorrent x64 Release；
- 通过 stdio 完成 `ready`、`task.add`、进度事件和 `shutdown`；
- 使用本地 Seeder 下载并校验一个单文件和一个多文件种子；
- 验证暂停、进程终止、重启恢复；
- 把伴随程序打进当前 Flutter Release 与 MSIX，验证启动、写 AppData 和用户目录下载；
- 记录 CPU、工作集、磁盘和 UI 帧耗时；
- 验证 Microsoft Store 包能力、签名和杀毒软件误报风险。

M0 任一关键项失败，应先修正架构或打包方案，而不是直接进入 UI 全量改造。

### M1：引擎 MVP

- 完成 P0 协议、状态机、任务目录、resume、错误模型和资源配置；
- 完成补充 Tracker 校验/动态应用、`seeding` 状态和限时/限比停止条件；
- 完成路径安全、删除保护、私有种子规则和结构化日志；
- 建立单元、协议契约、集成和恢复测试；
- 产出版本化 Windows 构建和第三方许可清单。

### M2：BangumiToday 集成

- 接入统一 Client/Store 和下载管理页；
- 替换 RSS/BMF/Anibt 的 Motrix 调用；
- 完成旧 Hive 数据处置、下载完成/做种结束通知分离以及 Tracker/做种设置页；
- 完成未打包与 MSIX E2E、性能回归和灰度开关。

### M3：稳定版

- 处理 M2 遥测和用户反馈中的崩溃、恢复、网络兼容问题；
- 评估 P1 文件选择、完成历史和按任务覆盖的高级做种策略；
- 达到性能预算后移除 Motrix 作为使用前提，正式开放内置下载入口。

## 12. 发布验收清单

- [ ] 测试机器未安装 Motrix，也未关联 `.torrent`，仍可完成下载。
- [ ] `.torrent` 与 Magnet 两类任务均可下载并通过 piece hash 校验。
- [ ] 切换页面、关闭下载管理页不会改变任务状态。
- [ ] 暂停后重启应用，任务保持暂停；下载中重启，任务能从已校验进度恢复。
- [ ] 引擎被强制结束时 BangumiToday 不退出，并能提示、重启、恢复任务。
- [ ] 重复添加不会产生第二份隐式下载或破坏现有文件。
- [ ] 磁盘不足、目录不可写、无 Peer、元数据超时均有可理解且可重试的状态。
- [ ] 移除任务默认保留文件；删除数据不能越过任务根目录。
- [ ] 私有种子不通过 DHT、PEX、LSD 泄露 info-hash 或 Peer 信息。
- [ ] Tracker 多源同步允许部分失败、全部失败保留旧快照，私有种子和未确认公开属性的 Magnet 不注入公共 Tracker。
- [ ] 新安装按分享率 `2.0` 或 `60` 分钟先到即停；暂停/重启不丢失累计量，升级安装不自动开启做种。
- [ ] 默认限流和两个并发任务满足性能预算，BangumiToday UI 无明显卡顿。
- [ ] Release、侧载 MSIX、升级 MSIX 均能启动引擎并访问既有 resume 数据。
- [ ] 发布包包含引擎版本信息、许可证通知和依赖清单，无运行时下载可执行文件。

## 13. 风险与待确认事项

| 风险/问题 | 当前建议 |
| --- | --- |
| MSIX/Store 是否允许并可靠启动随包子进程 | 列为 M0 阻断项，必须在实际 Store 配置下验证；引擎不注册用户服务、不提权、不写安装目录。 |
| 应用退出后是否继续下载 | P0 明确不继续。若未来需要，重新设计身份验证、单实例和后台生命周期。 |
| 是否需要长期做种 | P0 只实现分享率/时间任一先到即停，不提供无限做种；未来再评估单任务覆盖和高级上传队列。 |
| 公共 Tracker 列表的可用性与隐私 | 列表是第三方输入且不承诺可用性；保留最后成功快照、允许完全关闭，并禁止向私有种子注入。 |
| 是否首版支持文件选择 | P0 全选，P1 支持；若实际多文件种子经常包含无关文件，可提升优先级。 |
| 选择 libtorrent 的具体版本和链接方式 | M0 根据稳定分支、漏洞、包体积、OpenSSL/CRT 依赖和 ABI 测试后锁定，不跟随浮动最新版。 |
| Windows ARM64/macOS | 不阻塞 P0；协议和领域模型保持平台无关，构建产物按架构区分。 |
| P2P 功能导致杀毒误报或 Store 审核变化 | M0 提前提交实际包验证；保持签名、依赖来源、SBOM 和用途说明完整。 |
| 用户下载目录被移动、删除或离线 | 任务进入明确的 `SAVE_PATH_UNAVAILABLE` 错误，不能回退到未知默认目录。 |

## 14. 参考资料

- [libtorrent 功能列表](https://www.libtorrent.org/features.html)：DHT、Magnet/BEP 9、PEX、LSD、uTP、限速、多线程磁盘 I/O、fast-resume。
- [libtorrent Core API](https://libtorrent.org/reference-Core.html)：任务添加标志、重复任务、Magnet 解析和 resume 语义。
- [Tracker 与限量做种专项需求](tracker-and-seeding.md)：Motrix 对照、设置模型、私有种子规则、协议扩展和验收标准。
- [Motrix](https://github.com/agalwood/Motrix)：Tracker 列表源、自动同步和做种设置的交互参考。
- [aria2 BT 选项](https://aria2.github.io/manual/en/html/aria2c.html#bittorrent-specific-options)：分享率与做种时间任一条件先满足即停止的语义参考。
- [libtorrent 构建文档](https://libtorrent.org/building.html)：CMake/vcpkg、Windows 构建及编译参数/ABI 一致性要求。
- [libtorrent 许可证](https://github.com/arvidn/libtorrent/blob/RC_2_0/COPYING)：允许源码和二进制再分发，但发布材料必须保留许可证通知。
- [Dart `Process.start`](https://api.dart.dev/dart-io/Process/start.html)：普通模式提供父子进程 stdin/stdout/stderr；父进程必须持续读取输出流。
- [Microsoft：MSIX 桌面应用打包准备](https://learn.microsoft.com/en-us/windows/msix/desktop/desktop-to-uwp-prepare)：包安装目录只读、用户服务限制、包内进程 IPC 和 Store 测试要求。
- [Microsoft：MSIX 容器模型](https://learn.microsoft.com/en-us/windows/msix/msix-containerization-overview)：Full Trust/AppContainer 差异及状态文件位置约束。
