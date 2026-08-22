# 分步实现状态

本文把需求分析中的 P0/M0/M1 拆成可独立验证的阶段；已实现项随交付从本文移除，仅保留尚未完成的工作。

当前协议版本固定为 `1.4`，与 BangumiToday 随包严格同步；旧版本协商、回退与
按客户端版本门控的行为已移除。BitTorrent 由 libtorrent 承载，普通 HTTP(S)
文件任务由 libcurl multi 承载，两者共享任务目录、状态快照和控制协议。已交付
能力清单见 [README.md](../README.md)，调用契约见 [protocol.md](protocol.md)。

## 剩余工作

- [ ] 侧载/升级 MSIX 和 Microsoft Store 包实机验证：进程启动、父进程强制结束监管、Tracker 网络策略与状态恢复。
