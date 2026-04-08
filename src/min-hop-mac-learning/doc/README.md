# Min-Hop Mac Learning RoCE 文档索引

本文档集记录当前分支中 RoCE 集成的实际状态，重点回答三件事：

1. 当前代码到底已经做到哪一步。
2. 现在这套实现是怎样分层、怎样互相调用的。
3. 哪些部分已经稳定，哪些部分仍处于过渡态。

## 当前状态

当前版本已经不是单纯的“`sendflowclient + RoceTransportEngine`”结构，而是进入了一个过渡期：

- `Astra-Sim + 3P` 仍然是最上层业务流程。
- `sendflowclient` 仍然是对外应用 facade，负责 Astra-Sim 桥接和 3P 业务语义。
- `RoceTransportEngine` 仍然保留 lane/QPN/QP/PSN、RX reorder、header 生成和调度器组合等语义层职责。
- `RdmaPortEngine` 已经开始下沉到每个 `PointToPointNetDevice` 上，承接端口耦合更强的逻辑：
  - per-qid 发包准入
  - pause 感知
  - 端口侧 pending queue
  - ACK retire
  - retransmission timer
  - “真正重发时才消耗 retry”
- 当前发送路径已经引入两个本地 `PacketTag`：
  - `RdmaDispatchTag`
    - 跟随 packet 穿过 `FlowIo / bridge / outPort`
    - 只携带本地调度信息，不进入线上字节流，不影响 MTU
  - `RdmaIngressPortTag`
    - 由 bridge 在本地上送应用前写入
    - 告诉 `sendflowclient` 这次收包实际来自哪个物理 ingress p2p 口
- `REQUEST / DATA / ACT` 仍然保持 3 条独立逻辑 lane：
  - `REQUEST -> req_ctrl`
  - `DATA -> data`
  - `ACT -> act_ctrl`

这意味着当前代码的真实形态是：

- `RoceTransportEngine` 不再是唯一 transport core。
- `RdmaPortEngine` 也已经成为 transport/runtime 的一部分。
- 整体仍然是“过渡态”，还没有完成最终的 `QP/SQ/RQ/CQ` 全量下沉。

## 最新验证口径

当前仓里最新保留的一次 `ROCE=true` 实测结果来自：

- [config.json](/home/lja/sim-ly/sim-ly/simconfig/config.json)
- [microAllReduce.txt](/home/lja/sim-ly/sim-ly/simworkload/microAllReduce.txt)
- [simulation.log](/home/lja/sim-ly/sim-ly/simulation.log)

可直接确认的结果是：

- `REQ_DONE = 48`
- `RX_DONE = 48`
- `TX_DONE = 48`
- `total send count = 48`
- `total events = 443645`
- `last FLOW_TX_DONE since_workload_start(us) = 244.406`
- `network drained since_workload_start(us) = 244.734`

对应日志位置：

- [simulation.log:39298](/home/lja/sim-ly/sim-ly/simulation.log#L39298)
- [simulation.log:39299](/home/lja/sim-ly/sim-ly/simulation.log#L39299)
- [simulation.log:39301](/home/lja/sim-ly/sim-ly/simulation.log#L39301)
- [simulation.log:39302](/home/lja/sim-ly/sim-ly/simulation.log#L39302)

同时，这份最新日志里没有搜到：

- `[ROCE][RTX]`
- `REORDER_OVERFLOW`
- `flow remap detected`
- `QP rejected`
- `opcode mismatch`
- `phase mismatch`

但这次更新文档时，我会明确保留一个风险提示：

- 这轮端口侧下沉后，功能闭环是通的。
- 但时间行为已经和前一阶段实现明显不同。
- 所以当前版本不能简单视为“纯重构不改行为”。

## 文档导航

1. [roce-current-status.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-current-status.md)
   - 当前版本到底停在哪
   - 最新一次真实运行验证了什么
   - 当前已确认的问题、过渡态边界和未完成项

2. [roce-class-architecture.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-class-architecture.md)
   - 当前 RoCE 相关类分别负责什么
   - `sendflowclient / RoceTransportEngine / RdmaPortEngine / bridge / FlowIo` 之间怎么耦合
   - 参数怎样在这些类之间传递
   - 对外暴露了哪些接口、当前能做到什么功能

3. [roce-workflow.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-workflow.md)
   - 一条流从 Astra-Sim 下发后，当前版本怎样经过 `sendflowclient`
   - 怎样进入 `FlowIo / bridge / RdmaPortEngine`
   - 最终怎样从物理 port 发出
   - ACK 和本地 ingress 信息怎样回到端口侧状态机

4. [roce-config-and-logs.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-config-and-logs.md)
   - `roce` JSON 配置项
   - `[ROCE][CFG] / [ACTIVE] / [TX] / [RX]` 日志解释
   - 配置在代码里如何接入

5. [roce-qp-and-transport.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-qp-and-transport.md)
   - QP、CQ、SRQ、port 的现实语义
   - 为什么现实中会出现多个 QP 争用同一物理 port
   - 这些概念在当前项目里该如何映射

6. [roce-phase2-design.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-phase2-design.md)
   - 第二阶段目标设计
   - 当前版本和目标版之间还差什么

7. [roce-transport-reliability.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-transport-reliability.md)
   - 之前几轮 `ACK / 窗口 / 重传 / 重排` 的实现历史
   - 适合回看“为什么会演化到现在这一步”

## 阅读建议

- 如果你想先判断“这版实现是不是还要修”，先看 `roce-current-status.md`。
- 如果你想从类和职责边界审视当前实现，再看 `roce-class-architecture.md`。
- 如果你想直接沿着“一条流怎么发出去、ACK 怎么回来”来审视实现，再看 `roce-workflow.md`。
- 如果你要从 RDMA 现实模型反推仿真建模，再看 `roce-qp-and-transport.md`。
- 如果你想看这套实现是怎么一步步长出来的，再看 `roce-transport-reliability.md` 和 `roce-phase2-design.md`。
