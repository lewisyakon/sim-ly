# RoCE 集成说明索引

最近修改：2026-04-23

原先的单文件说明已经按主题拆分，新的文档位于 `src/min-hop-mac-learning/doc/`。

建议阅读顺序：

1. `src/min-hop-mac-learning/doc/README.md`
   - 总览、当前状态、文档导航
2. `src/min-hop-mac-learning/doc/topology-endpoint-bridge-model.md`
   - GPU 端点双 bridge、外部 bridge、`TerminalLearningOnly`、FlowIo/PFC 边界
3. `src/min-hop-mac-learning/doc/roce-config-and-logs.md`
   - JSON 配置项、日志含义、配置入口
4. `src/min-hop-mac-learning/doc/roce-workflow.md`
   - 从 Astra-Sim 下放到 `sendflowclient`，再到网络收发的完整 workflow
5. `src/min-hop-mac-learning/doc/roce-qp-and-transport.md`
   - QP 基础、一个 NIC 上为什么会有多个 QP、当前项目与 UB 的职责差异、阶段划分和对外表述
6. `src/min-hop-mac-learning/doc/roce-transport-reliability.md`
   - 最近几轮 `ACK / 窗口 / 重传 / 重排 / ACK 优先级 / 自动 timeout` 的落地实现
7. `src/min-hop-mac-learning/doc/archive/roce-phase2-design.md`
   - 历史 Phase 2 设计口径，只作为演化脉络参考

本文件保留为兼容入口，不再继续扩写。
