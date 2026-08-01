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
- [x] metadata/checking/queued/downloading/paused/completed/error 状态；
- [x] 500 ms 聚合进度事件（普通进度最多 2 次/秒）和单调序号；
- [x] 完成后立即暂停，默认不继续做种；
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
- [ ] libtorrent alert 到业务错误码的细粒度映射；
- [ ] Windows Job Object 父子进程监管（由 BangumiToday 启动端配合）。

## 第 4 步：发布级验证（待实现）

- [x] 本地 Tracker/Seeder 的单文件、多文件和 Magnet 集成测试；
- [x] 强制终止、断点恢复和安全删除 E2E；
- [ ] 磁盘不足与私有种子 E2E；
- [ ] CPU、工作集、磁盘吞吐和事件频率基准；
- [ ] Release 依赖复制、第三方许可证/SBOM；
- [ ] Flutter Release、侧载/升级 MSIX 和 Microsoft Store 包验证；
- [ ] BangumiToday `BtEngineClient`、任务 Store 和 UI 接入。

下一阶段应补齐磁盘不足与私有种子 E2E，再开始 Flutter/MSIX 接入；这样可在 UI 改造前验证错误与隐私边界。
