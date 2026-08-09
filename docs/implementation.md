# 分步实现状态

本文把需求分析中的 P0/M0/M1 拆成可独立验证的阶段。勾选项表示本仓库当前已有实现和对应的基础验证，不代表尚未执行的 MSIX、公开网络或 BangumiToday 主仓库验收已经通过。

## 第 1 步：工程与协议闭环（已完成）

- [x] C++20、CMake Preset、vcpkg 固定依赖和 Windows x64 构建；
- [x] libtorrent 2.0.11 封装在 `Engine` 内，上层不暴露 libtorrent 类型；
- [x] stdin/stdout NDJSON + JSON-RPC 2.0，stdout 与 stderr 分离；
- [x] `event.ready`、协议协商、1 MiB 帧上限和稳定错误结构；
- [x] `engine.initialize/status/configure/shutdown`；
- [x] `task.add/list/get/pause/resume/retry/recheck/remove`。

## 第 2 步：P0 任务语义（已完成基础实现）

- [x] `.torrent` 绝对路径和 Magnet URI；
- [x] UUID 任务 ID、同一 info-hash 会话级去重（避免 libtorrent 将不同保存路径静默别名到同一 handle）；
- [x] metadata/checking/queued/downloading/seeding/paused/completed/error 状态；
- [x] 500 ms 聚合进度事件（普通进度最多 2 次/秒）和单调序号；
- [x] 协议 `1.0` 或未显式启用做种时完成后立即暂停；协议 `1.1` 可执行限量做种；
- [x] 删除任务默认保留数据，删除数据必须显式传入 `deleteData`；
- [x] 保存目录绝对路径/存在性/可写性检查；
- [x] 种子内部绝对路径、盘符和 `..` 穿越拦截；
- [x] 已有 `.torrent` 元数据任务的磁盘空间预检；
- [x] 并发、上下行速度和连接数运行时配置。

## 第 3 步：恢复与健壮性（部分完成）

- [x] 版本化任务目录与配置恢复；
- [x] Windows 原子替换 catalog；
- [x] 单条损坏任务隔离，损坏 catalog 隔离为 `.corrupt`；
- [x] EOF、显式 shutdown 和异常析构路径停止监控线程并保存；
- [x] libtorrent fast-resume 数据的关键状态/30 秒周期保存、最终保存和恢复加载，损坏文件按任务隔离；
- [x] Magnet 元数据取得后的路径/空间二次检查与明确的元数据超时；
- [x] libtorrent alert 到业务错误码的细粒度映射；
- [x] Windows Job Object 父子进程监管（由 BangumiToday 启动端持有
  `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` 句柄）。

## 第 4 步：发布级验证（待实现）

- [x] 本地 Tracker/Seeder 的单文件、多文件和 Magnet 集成测试；
- [x] 强制终止、断点恢复和安全删除 E2E；
- [x] 磁盘不足与私有种子 E2E；
- [x] CPU、工作集、磁盘吞吐和事件频率基准；
- [x] Release 依赖复制、第三方许可证/SBOM；
- [x] BangumiToday `BtEngineClient`、任务 Store 和 UI 接入；
- [x] Flutter Release/MSIX 构建接入与伴随进程产物完整性校验；
- [ ] 侧载/升级 MSIX 和 Microsoft Store 包实机验证。

## 第 5 步：Tracker 与限量做种（已完成）

- [x] 协议升级到 `1.1`，配置支持 `additionalTrackers`、`seedingEnabled`、`seedRatioLimit` 和 `seedTimeLimitMinutes`；
- [x] Tracker URL 二次校验、去重、来源标记和运行时动态应用；
- [x] 公共 `.torrent`/Magnet 补充 Tracker，私有种子与属性未知 Magnet 的防泄露测试；
- [x] `seeding` 状态、累计上传/做种时间、停止条件与停止原因；
- [x] 做种计数的 fast-resume 持久化以及暂停、崩溃、重启恢复测试；
- [x] 文件可用与做种结束状态分离，做种任务不占活动下载槽；
- [x] BangumiToday 完成列表源同步、最后成功快照、自动更新和设置页接入；
- [x] 协议 `1.0`/catalog schema 1 迁移保持完成即停止；新安装默认值由 BangumiToday `1.1` 客户端显式下发。

专项需求、边界和验收用例见 [tracker-and-seeding.md](tracker-and-seeding.md)。下一阶段还应在侧载、升级安装和 Store 包实机环境下验证进程启动、父进程强制结束监管、Tracker 网络策略与状态恢复。

## 第 6 步：文件选择与优先级（已完成基础实现）

- [x] `task.setFilePriorities` 部分索引更新，优先级 `0`（跳过）/`1`/`4`/`7` 与索引范围校验；
- [x] 元数据不可用返回 `METADATA_UNAVAILABLE`，已完成/做种任务返回 `TASK_UNAVAILABLE`；
- [x] `task.details` 文件项返回 `priority`；修改等待磁盘线程异步生效并触发 fast-resume 保存，跨重启恢复；
- [x] 多文件种子只下载选中文件、跳过文件不落盘，以及完成态拒绝修改的集成测试；
- [x] BangumiToday 文件标签页按文件勾选“跳过/下载”并即时应用。

## 第 7 步：下载详情按 Tab 拆分（协议 1.2）

- [x] `task.details` 对 `1.2` 客户端只返回概览（任务、分片状态、`totalFiles`/`totalPeers`），不再携带大列表；
- [x] 新增 `task.files` / `task.peers`，`offset`/`limit` 窗口、`*Truncated` 与 `nextOffset` 分页语义和 `INVALID_PAGINATION` 校验；
- [x] `1.0`/`1.1` 客户端继续收到 `task.details` 全量列表，行为不回归；
- [x] `event.ready`、`engine.initialize` 与 `engine.status` 的 `features` 增加 `tabbedDetails`；
- [x] 引擎侧覆盖概览拆分、分页窗口、越界与非法分页参数的协议测试。
