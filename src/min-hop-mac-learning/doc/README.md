# Min-Hop Mac Learning 文档索引

最近修改：2026-05-07

这组文档现在以“当前代码已经实现成什么样”为中心，不再按早期阶段性原型来描述。当前文档分成两部分：

- RoCE runtime 主路径、可靠性和日志
- GPU 端点的 bridge / 外部口 / PFC 边界

## 当前一句话结论

当前版本里：

- `sendflowclient` 是上层 facade 和 3P 业务层。
- `RoceTransportEngine` 是逻辑层，只负责 `flow/lane -> qpn`、`WR/work` 准备、3P FSM。
- `RdmaPortEngine` 是真正的 RoCE 运行时核心，挂在每个 `PointToPointNetDevice` 上，负责端口绑定后的 `QP/SQ/RQ/CQ`、packetize、ACK、重传、reorder、pause-aware 调度。
- GPU 端点现在统一成“内侧 bridge + 可选外侧 bridge”的模型；外侧 bridge 可以只学表、不承担 transit 转发。

也就是说，RoCE 主路径已经不再依赖旧的 `sendflowclient` 内部 fallback。

## 最新验证

当前仓里最近一次小模型 `ROCE=true` 实测来自：

- [config.json](/home/lja/sim-ly/sim-ly/simconfig/config.json)
- [microAllReduce.txt](/home/lja/sim-ly/sim-ly/simworkload/microAllReduce.txt)
- [simulation.log](/home/lja/sim-ly/sim-ly/simulation.log)

本次日志降级后，不再打印完整 `FLOW_TX_DONE / FLOW_RX_DONE` 明细；因此这里不再沿用旧文档里的 `TX_DONE=48 / RX_DONE=48` 口径。

本次实测可直接确认：

- `REQ_DONE = 144`
- `total send count = 144`
- `total events = 1171473`
- `last FLOW_TX_DONE since_workload_start(us) = 15562.8`
- `network drained since_workload_start(us) = 15563.1`

对应日志：

- [simulation.log:1186](/home/lja/sim-ly/sim-ly/simulation.log#L1186)
- [simulation.log:1187](/home/lja/sim-ly/sim-ly/simulation.log#L1187)
- [simulation.log:1189](/home/lja/sim-ly/sim-ly/simulation.log#L1189)
- [simulation.log:1190](/home/lja/sim-ly/sim-ly/simulation.log#L1190)

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

2. [topology-endpoint-bridge-model.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/topology-endpoint-bridge-model.md)
   - 当前 GPU 端点为什么拆成 inner/external bridge
   - `TerminalLearningOnly` 到底做什么
   - FlowIo、bridge、PFC 现在分别落在哪一层

3. [external-topology-v1.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/external-topology-v1.md)
   - 外部拓扑文本文件 v1 的格式和合法性规则
   - GPU rank / 交换机编号 / 外网和内网链路的语义
   - external topology 构建器必须保留哪些 GPU 标准组件

4. [roce-reality-gap-review.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-reality-gap-review.md)
   - 当前实现和现实 RoCE 的差距到底在哪里
   - 哪些差距会影响大规模网络行为
   - 哪些是必须补、建议补、可以暂缓

5. [memory-leak-investigation.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/memory-leak-investigation.md)
   - 本轮内存增长问题的根因
   - CSV dimension utilization 为什么会制造超大中间结构
   - 当前关闭 CSV 和清理临时内存探针后的验证口径

6. [roce-class-architecture.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-class-architecture.md)
   - 每个 RoCE 类分别负责什么
   - 类之间怎样耦合、怎样传参
   - 从外部调用时暴露什么接口
   - 整体已经支持什么功能

7. [roce-workflow.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-workflow.md)
   - 一条流从 Astra-Sim 下发后，怎样经过 transport、bridge、port、QP，最终从物理口发出去
   - 接收时怎样回到 ingress `RdmaPortEngine`
   - ACK、reorder、completion 分别在哪里发生

8. [roce-transport-reliability.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-transport-reliability.md)
   - ACK、重传、窗口、reorder 当前落在哪里
   - 哪些可靠性能力已经从 `sendflowclient` 下沉到 `RdmaPortEngine`

9. [roce-config-and-logs.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-config-and-logs.md)
   - 配置项解释
   - 日志口径解释

10. [roce-qp-and-transport.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-qp-and-transport.md)
   - 现实中的 `QP / port / CQ / SRQ`
   - 它们在当前仿真里的映射

11. [roce-future-scheduling-options.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-future-scheduling-options.md)
   - 如果后续继续提高 port 调度真实性，可以考虑哪些方案
   - 为什么当前暂时不继续改调度层

## 历史文档

下面这些文档已经移到 `archive/`，应主要按“演化过程参考”来看，而不是“当前实现说明”：

- [archive/README.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/archive/README.md)
- [archive/roce-phase2-design.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/archive/roce-phase2-design.md)
- [archive/roce-scheduler-stage1.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/archive/roce-scheduler-stage1.md)
