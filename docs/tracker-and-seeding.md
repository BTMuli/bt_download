# Tracker 与限量做种需求

> 状态：已实现（待发布环境验证）
> 目标协议：`1.2`（引擎与 BangumiToday 主应用均已严格同步）
> 适用范围：BangumiToday 设置页、`BtEngineClient` 与 `bt_download` 伴随进程

## 1. 结论

下一阶段应把 Tracker 补充和限量做种提升为替代 Motrix 的必备能力，并采用以下产品基线：

- 设置页可选择多个 Tracker 列表源、手动刷新、查看合并后的 Tracker，并默认每 24 小时自动刷新一次；
- BangumiToday 负责下载远程列表，`bt_download` 只接收已经校验的 Tracker URL，不在引擎内访问列表源；
- 公共补充 Tracker 只应用于公共种子；不得向私有种子注入，也不得在 Magnet 元数据尚未确认公开属性前注入；
- 新安装默认在下载完成后继续做种，达到分享率 `2.0` 或做种 `60` 分钟中的任一条件后停止；
- 升级安装保持既有“完成即停止”行为，直到用户确认新的做种设置，避免升级后静默增加上传流量；
- 下载完成与做种结束是两个事件：文件校验完成即可通知用户，随后任务可进入 `seeding`，最终才进入 `completed`。

本文定义完整的产品与协议语义；引擎以及 BangumiToday 设置、同步、迁移与提示均已实现。发布环境验证状态见 [implementation.md](implementation.md)。

## 2. Motrix 参考与取舍

Motrix 的设置模型提供了有价值的交互参考：

- Tracker 列表源支持多选和自定义 URL，可手动同步，并展示最后同步时间；
- 默认推荐 `ngosang/trackerslist` 与 `XIU2/TrackersListCollection`，同步结果合并后写入 aria2 的 `bt-tracker`；
- 自动同步默认开启。界面文案为“每天”，当前源码中的启动检查间隔实际为 12 小时；
- 做种设置提供“持续做种”、分享率和分钟数。aria2 在分享率与时间同时配置时，任一条件满足即结束做种。

BangumiToday 参考其交互，但不照搬以下实现细节：

| Motrix/aria2 做法 | BangumiToday 取舍 |
| --- | --- |
| 列表源、解析结果和引擎参数混在一份配置中 | 拆为应用侧的列表源/缓存和引擎侧的 `additionalTrackers` |
| 同步成功后直接替换 `bt-tracker` 字符串 | 手工 Tracker 永久保留；远程列表使用最后一次成功快照，失败不清空 |
| 以字符串长度限制 Tracker 集合 | 同时限制源数量、响应大小、单 URL 长度和最终条目数 |
| “持续做种”通过特殊数值组合表达 | 使用显式 `seedingEnabled`，不依赖魔法值 |
| UI“每天”与 12 小时检查间隔不一致 | 产品语义固定为距上次成功更新满 24 小时 |

截图中的分享率 `2`、做种时间 `60` 分钟作为本项目的新安装默认值；它们不是 Motrix 当前源码中的全部默认值复刻。

## 3. 责任边界

| 组件 | 责任 |
| --- | --- |
| BangumiToday 设置页 | 编辑列表源、手工 Tracker、自动更新开关和做种策略；展示更新时间、错误和生效数量 |
| BangumiToday 同步服务 | 获取远程文本、限制响应、解析/校验/去重、维护最后成功快照，并调用 `engine.configure` |
| `BtEngineClient` | 协议 `1.2` 严格匹配、配置下发、错误映射和任务快照兼容 |
| `bt_download` | 二次校验 Tracker、按公开/私有属性应用、跟踪做种指标、执行停止条件并持久化状态 |
| libtorrent | Tracker announce、Peer 连接、上传、累计计数与 fast-resume 基础能力 |

远程列表同步不是 BT 引擎的任务。这样可以复用 BangumiToday 的 HTTP、代理和证书策略，也避免伴随进程同时承担任意 URL 获取器的安全边界。

## 4. Tracker 设置

### 4.1 设置模型

应用侧持久化以下字段：

| 字段 | 类型与默认值 | 说明 |
| --- | --- | --- |
| `trackerSources` | URL 数组，最多 8 个 | 默认选择 `ngosang/trackerslist` 的 `trackers_best_ip.txt` 与 `trackers_best.txt` HTTPS CDN 地址 |
| `manualTrackers` | URL 数组，默认空 | 用户手工维护，不会被远程同步覆盖 |
| `autoUpdateTrackers` | `true` | 是否在应用启动后按 24 小时周期检查 |
| `lastTrackerUpdateAttemptAt` | 可空时间 | 最近一次尝试时间，用于诊断，不作为周期基准 |
| `lastTrackerUpdateSuccessAt` | 可空时间 | 最近一次至少一个源成功且快照可用的时间 |
| `trackerUpdateError` | 可空摘要 | 仅用于 UI；不得包含 URL 查询参数或私有 passkey |
| `resolvedTrackers` | URL 数组 | 最后一次成功的远程快照，原子替换并可离线复用 |

首期内置候选列表源可包含 Motrix 推荐的 `ngosang/trackerslist` 与 `XIU2/TrackersListCollection`，但来源 URL 必须集中配置，不散落在 Widget 或引擎代码中。第三方列表内容不受本项目控制，UI 应标明来源、说明补充 Tracker 会获知任务 info-hash 与用户 IP，并允许完全关闭。

UI 可沿用截图中的“多选来源 + 刷新按钮 + Tracker 文本区 + 自动更新 + 最后更新时间”布局，但数据模型必须区分手工项与同步项。即使界面把两者合并展示，再次同步也不能覆盖用户手工输入。

### 4.2 刷新行为

1. 用户点击刷新时立即执行；自动刷新只在距 `lastTrackerUpdateSuccessAt` 已满 24 小时时执行。
2. 每个源独立请求，单源连接与读取总超时 30 秒，响应正文上限 1 MiB，最多跟随 5 次 HTTP 重定向。
3. 接受 `http`/`https` 列表源，不接受 `file`、`data` 或带用户名密码的 URL；内置源只使用 HTTPS。
4. 多源并发获取，允许部分成功。成功源的结果合并；全部失败时保留上一份 `resolvedTrackers`。
5. 只有解析后至少得到一个合法 Tracker，才更新成功时间并原子替换远程快照。
6. 刷新不会删除 `manualTrackers`。传给引擎的集合为 `manualTrackers + resolvedTrackers` 去重后的结果。
7. 应用关闭或切换页面不取消已经进入提交阶段的快照写入；同一时间最多存在一个同步作业。

支持 ETag 或 `Last-Modified` 时应发送条件请求。`304 Not Modified` 视为成功并更新成功时间，但不改写快照内容。

### 4.3 解析与校验

- 文本按换行解析；手工输入额外接受逗号分隔；忽略空行、UTF-8 BOM 和以 `#` 开头的整行注释；
- Tracker URL 只接受 `udp`、`http`、`https`，必须包含主机，端口必须在 `1..65535`，禁止 userinfo 和 fragment；
- 单条 URL 最长 2048 字符，最终最多保留 512 条；超出时按“手工优先、源选择顺序、文件内顺序”截断并提示；
- 去重时统一 scheme/host 大小写并消除默认端口，其余路径和查询参数原样保留；
- URL 查询参数可能包含私有 Tracker passkey，日志和遥测只记录 scheme、host、端口及脱敏计数。

任何一条非法记录只淘汰该记录，不使整个源失败；响应不是 UTF-8、超限或没有合法记录时，该源失败。

### 4.4 应用到任务

- `.torrent` 自带的 announce tiers 和 Magnet 的 `tr` 参数始终保留，补充 Tracker 作为最低优先级的新 tier 追加；
- `.torrent` 在创建任务时即可判断 private 标志。私有种子不得注入任何全局补充 Tracker；
- Magnet 在取得元数据前只使用 URI 自带的 `tr`、DHT 和已连接 Peer。仅当元数据明确 `private=false` 后才注入补充 Tracker；
- 配置更新应用到新任务以及当前 `metadata`、`queued`、`downloading`、`seeding` 的公共任务；引擎必须记录 Tracker 来源，只移除旧的“全局补充”项，不能改写种子自带项；
- 单个 Tracker 失败只影响该 announce endpoint，不把任务直接置为 `error`。所有发现方式长期不可用时，沿用任务级网络/元数据超时语义。

## 5. 限量做种设置

### 5.1 配置与默认值

协议 `1.2` 包含以下三个全局字段：

| 字段 | 范围 | 新安装默认值 | 语义 |
| --- | --- | --- | --- |
| `seedingEnabled` | 布尔值 | `true` | `false` 表示文件校验完成后立即停止上传 |
| `seedRatioLimit` | `0` 或 `0.1..100.0` | `2.0` | `0` 禁用分享率条件 |
| `seedTimeLimitMinutes` | `0..525600` 整数 | `60` | `0` 禁用时间条件 |

`seedingEnabled=true` 时至少一个停止条件必须大于 0；首期不支持无限做种，避免静默无限上传。两个条件都启用时按逻辑“或”判断，任一先满足即停止。

表中的新安装默认值属于 BangumiToday 产品设置。字段缺省时引擎采用安全默认值
`seedingEnabled=false`；客户端在用户确认提示后显式下发产品默认值。

升级既有持久配置时，迁移值为 `seedingEnabled=false`，设置页预填 `2.0` 和 `60`，由用户保存或明确启用后生效。新安装也必须先展示 BT 会暴露 IP 并产生上传流量的说明。

### 5.2 指标定义

- `uploadedBytes`：任务创建以来累计上传的有效载荷字节，包含下载阶段的上传，并通过 fast-resume 跨重启累计；
- `shareRatio`：`uploadedBytes / totalWantedBytes`。分母使用任务选择下载的内容总字节，避免已有完整文件或计数恢复时除以零；
- `seedingSeconds`：任务数据完整后处于未暂停 `seeding` 状态的累计秒数；没有 Peer 或没有实际上行时仍计时；用户暂停、引擎未运行和任务错误期间不计时；
- 分享率比较不得依赖 UI 四舍五入后的值，内部至少保留双精度或等价的整数交叉相乘精度。

达到分享率或时间条件后，引擎暂停该 torrent 的网络活动、保存 resume，并进入 `completed`。停止原因分别记录为 `ratio` 或 `time`；同一轮检查同时满足时优先记录 `ratio`，但不影响行为。

### 5.3 状态与交互

```text
downloading -> seeding -> completed
           \-> completed  （seedingEnabled=false）
```

- `seeding` 表示所选文件已完整校验、仍在对外上传；它不占 `activeDownloads` 下载槽；
- 首次进入 `seeding`，或禁用做种时直接进入 `completed`，都触发一次“文件可用”通知；进入最终 `completed` 不重复通知；
- 用户在 `seeding` 时暂停，任务进入 `paused` 并保留暂停前阶段；继续后回到 `seeding`；
- 引擎重启后恢复累计上传量、累计做种时间和停止策略，不从零开始计算；
- 运行时降低限制后，如果活动做种任务已满足新条件，应在下一次状态检查（目标 1 秒内）停止；提高限制不会自动重启已经 `completed` 的任务；
- `seeding` 任务按约 1 秒间隔检查 payload 文件的存在和大小。发现文件被删除或截断时自动进入 `checking`，清除旧的做种状态并恢复下载；用户暂停和已停止做种的 `completed` 任务不会被自动唤醒；
- 全局 `uploadRateLimit` 同时约束下载阶段上传和做种阶段上传。首期不增加单任务上传限速或做种队列调度。

任务快照新增：

```json
{
  "state": "seeding",
  "uploadedBytes": 734003200,
  "shareRatio": 1.37,
  "seedingSeconds": 1240,
  "seedRatioLimit": 2.0,
  "seedTimeLimitMinutes": 60,
  "seedStopReason": null
}
```

`seedStopReason` 为 `null`、`disabled`、`ratio` 或 `time`。展示层应将分享率限制显示为无量纲小数，将时间显示为分钟，不把 `2.0` 格式化为 `2%`。

## 6. 协议与持久化

`engine.initialize` 和 `engine.configure` 在协议 `1.2` 接受以下字段：

```json
{
  "additionalTrackers": [
    "udp://tracker.example:6969/announce",
    "https://tracker.example/announce"
  ],
  "seedingEnabled": true,
  "seedRatioLimit": 2.0,
  "seedTimeLimitMinutes": 60
}
```

要求如下：

- 整次配置更新必须先完整校验再原子生效；任一字段非法时返回 `INVALID_CONFIG`，旧配置保持不变；
- `additionalTrackers` 仍由引擎执行同等的 URL、长度、数量和去重校验，不能信任应用侧结果；
- `engine.status` 返回最终生效配置，但不得把含查询参数的 Tracker URL 写入普通诊断日志；
- catalog schema 升级时保存做种配置；每个任务的 fast-resume 保存累计上传量和做种时间；
- 引擎与客户端严格使用协议 `1.2`，不提供旧协议协商或回退；字段缺省时仍按安全默认值处理。

远程列表源、更新时间和同步错误属于 BangumiToday 配置，不进入引擎 catalog。

## 7. 验收标准

| ID | 场景 | 预期 |
| --- | --- | --- |
| TS-001 | 两个列表源一成一败 | 合并成功源并更新时间；UI 提示部分失败；旧手工项仍存在 |
| TS-002 | 所有源失败或响应超限 | 保留最后成功快照，不把空数组下发到引擎 |
| TS-003 | 列表含重复、注释、非法 scheme 和超长 URL | 合法项按顺序去重；非法项被丢弃且不泄露完整 URL 到日志 |
| TS-004 | 公共 `.torrent` 和公共 Magnet | 元数据确认公开后均含补充 Tracker，原 announce tier 未被覆盖 |
| TS-005 | 私有 `.torrent` 和最终为私有的 Magnet | 全程不向全局补充 Tracker 泄露 info-hash，DHT/PEX/LSD 仍遵守 private 规则 |
| TS-006 | 分享率先达到 2.0 | 任务在 1 秒内停止做种，进入 `completed`，原因为 `ratio` |
| TS-007 | 60 分钟先达到 | 即使无 Peer/无上传也停止做种，原因为 `time` |
| TS-008 | 做种中暂停、退出并恢复 | 暂停/停机时间不累计，累计上传和做种秒数不丢失 |
| TS-009 | 下载期间已上传达到目标 | 数据校验完成后不进入持续做种，直接完成且文件可用通知只发一次 |
| TS-010 | 运行时收紧/放宽限制 | 活动任务立即重新评估；已完成任务不会被自动重启 |
| TS-011 | 从旧目录升级配置 | 保持完成即停止，直到用户明确启用新策略 |
| TS-012 | 全局上传限速下同时下载和做种 | 聚合上行不持续超过配置限速，做种任务不占下载并发槽 |
| TS-013 | 删除做种/已完成任务的负载后强制重校验 | 任务进入 `checking`；发现缺失数据后进入 `downloading`，不被旧停止原因卡在 0%，并从 Peer 恢复完整文件 |
| TS-014 | 做种中直接删除或截断 payload | 任务约 1 秒内自动进入 `checking`，重新发现 Peer 并恢复完整文件；用户暂停或已停止做种的任务不自动唤醒 |

测试必须使用本地 Tracker、可自由分发的测试数据和可控时钟，不依赖公共 Tracker 列表的在线状态。

## 8. 非目标

- 按单任务、Tracker、分类或 RSS 源覆盖做种策略；
- 无限做种、最小做种时间、空闲停止、做种任务轮转或上传槽调度；
- Tracker 健康评分、自动剔除、黑名单订阅或可用性承诺；
- 编辑种子自带 announce tiers；
- 应用退出后继续后台做种；
- 通过自动同步下载可执行代码、脚本或非文本配置。

## 9. 参考资料

- [Motrix Tracker 设置界面](https://github.com/agalwood/Motrix/blob/master/src/renderer/components/Preference/Advanced.vue)
- [Motrix 做种设置界面](https://github.com/agalwood/Motrix/blob/master/src/renderer/components/Preference/Basic.vue)
- [Motrix Tracker 同步实现](https://github.com/agalwood/Motrix/blob/master/src/shared/utils/tracker.js)
- [Motrix Tracker 来源与同步间隔](https://github.com/agalwood/Motrix/blob/master/src/shared/constants.js)
- [aria2 `seed-ratio` / `seed-time` 语义](https://aria2.github.io/manual/en/html/aria2c.html#bittorrent-specific-options)
- [libtorrent `torrent_status`](https://www.libtorrent.org/reference-Torrent_Status.html)
- [libtorrent Settings](https://libtorrent.org/reference-Settings.html)
