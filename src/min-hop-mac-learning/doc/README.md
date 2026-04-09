# Min-Hop Mac Learning RoCE 文档索引

这组文档现在以“当前代码已经实现成什么样”为中心，不再按早期阶段性原型来描述。

## 当前一句话结论

当前版本里：

- `sendflowclient` 是上层 facade 和 3P 业务层。
- `RoceTransportEngine` 是逻辑层，只负责 `flow/lane -> qpn`、`WR/work` 准备、3P FSM。
- `RdmaPortEngine` 是真正的 RoCE 运行时核心，挂在每个 `PointToPointNetDevice` 上，负责端口绑定后的 `QP/SQ/RQ/CQ`、packetize、ACK、重传、reorder、pause-aware 调度。

也就是说，RoCE 主路径已经不再依赖旧的 `sendflowclient` 内部 fallback。

## 最新验证

当前仓里最新一次 `ROCE=true` 实测来自：

- [config.json](/home/lja/sim-ly/sim-ly/simconfig/config.json)
- [microAllReduce.txt](/home/lja/sim-ly/sim-ly/simworkload/microAllReduce.txt)
- [simulation.log](/home/lja/sim-ly/sim-ly/simulation.log)

本次实测可直接确认：

- `REQ_DONE = 48`
- `RX_DONE = 48`
- `TX_DONE = 48`
- `last FLOW_TX_DONE since_workload_start(us) = 291.923`
- `network drained since_workload_start(us) = 292.251`

对应日志：

- [simulation.log:418](/home/lja/sim-ly/sim-ly/simulation.log#L418)
- [simulation.log:419](/home/lja/sim-ly/sim-ly/simulation.log#L419)

同时没有搜到这些异常项：

- `[ROCE][RQ][FULL]`
- `[ROCE][CQ][OVERFLOW]`
- `[ROCE][RTX]`
- `REORDER_OVERFLOW`
- `flow remap detected`
- `QP rejected`
- `opcode mismatch`
- `phase mismatch`
- `Callback not found`
- `without expected count setup`

## 阅读顺序

1. [roce-current-status.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-current-status.md)
   - 当前版本已经完成了什么
   - 最新运行验证了什么
   - 现在还保留了哪些简化

2. [roce-class-architecture.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-class-architecture.md)
   - 每个 RoCE 类分别负责什么
   - 类之间怎样耦合、怎样传参
   - 从外部调用时暴露什么接口
   - 整体已经支持什么功能

3. [roce-workflow.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-workflow.md)
   - 一条流从 Astra-Sim 下发后，怎样经过 transport、bridge、port、QP，最终从物理口发出去
   - 接收时怎样回到 ingress `RdmaPortEngine`
   - ACK、reorder、completion 分别在哪里发生

4. [roce-config-and-logs.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-config-and-logs.md)
   - 配置项解释
   - 日志口径解释

5. [roce-qp-and-transport.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-qp-and-transport.md)
   - 现实中的 `QP / port / CQ / SRQ`
   - 它们在当前仿真里的映射

## 历史文档

下面这些文档还保留，但应主要按“演化过程参考”来看，而不是“当前实现说明”：

- [roce-phase2-design.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-phase2-design.md)
- [roce-transport-reliability.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-transport-reliability.md)
- [roce-scheduler-stage1.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-scheduler-stage1.md)

