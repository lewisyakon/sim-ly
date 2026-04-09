# RoCE 当前状态

这份文档只回答 4 个问题：

1. 当前版本准确做到哪一步了。
2. 最新一次真实运行验证了什么。
3. 现在还保留哪些简化。
4. 当前最值得注意的风险是什么。

## 1. 当前版本的准确定位

当前版本已经不再是“解耦进行中”的早期过渡态，而是完成了本轮目标结构：

1. `sendflowclient`
   - Astra-Sim facade
   - 3P 业务语义

2. `RoceTransportEngine`
   - 逻辑层
   - `flow/lane -> qpn`
   - work 准备
   - FSM

3. `RdmaPortEngine`
   - port-bound RoCE runtime
   - `QP/SQ/RQ/CQ`
   - packetize
   - ACK / reorder / retransmission
   - pause-aware 调度

也就是说：

- 旧的 `sendflowclient` 内部 RoCE fallback 已经收掉
- `RdmaPortEngine` 已经成为唯一的 packet runtime

## 2. 当前已经完成的内容

当前版本已经具备：

1. `REQUEST / DATA / ACT` 三条逻辑 lane
2. `flow + lane -> qpn`
3. port-bound `QpRuntime`
4. `SQ` 最小模型
5. `RQ` 最小模型
6. `CQ` 最小模型
7. `Control > Data` 调度
8. send-side packetization
9. `RoceSimHeader` 生成
10. RX reorder
11. transport ACK
12. ACK retire
13. retransmission timer
14. pending retransmission / recovery
15. “真正重发时才 retry++”
16. pause-aware 发送准入
17. per-qid inflight packet 限制
18. send/recv completion 记录

## 3. 最新真实验证

当前仓里最新一次 `ROCE=true` 真实运行使用的是：

- [config.json](/home/lja/sim-ly/sim-ly/simconfig/config.json)
- [microAllReduce.txt](/home/lja/sim-ly/sim-ly/simworkload/microAllReduce.txt)
- [simulation.log](/home/lja/sim-ly/sim-ly/simulation.log)

可以直接确认：

1. `REQ_DONE = 48`
2. `RX_DONE = 48`
3. `TX_DONE = 48`
4. `last FLOW_TX_DONE since_workload_start(us) = 291.923`
5. `network drained since_workload_start(us) = 292.251`

对应日志：

- [simulation.log:418](/home/lja/sim-ly/sim-ly/simulation.log#L418)
- [simulation.log:419](/home/lja/sim-ly/sim-ly/simulation.log#L419)

同时没有搜到：

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

因此至少可以确认：

1. 当前 workload 上功能闭环是通的
2. 这轮 `QP/SQ/RQ/CQ` 和兼容壳收尾没有把主路径打断

## 4. 当前保留的简化

虽然主结构已经到位，但这并不意味着它是完整 verbs 级 NIC 模型。

当前仍保留这些简化：

1. `CQ` 不是独立对象类，而是 `QpRuntime` 内的 completion 队列
2. completion 目前还没有成为上层完成语义的主驱动
   - `TX_DONE` 仍然靠 3P 的 `ACT`
3. `RQ` 目前是最小 receive slot 模型
4. `SRQ` 还没有实现
5. 当前主实现针对“出端口唯一”场景
6. 多出端口绑定策略还只是结构上留有空间，没有进入主逻辑

所以更准确地说：

- 这版已经完成“当前项目所需的 RoCE 主结构”
- 但它仍是一个带有简化资源模型的仿真实现

## 5. 当前最重要的风险提示

当前最需要正视的一点是：

- 功能闭环虽然稳定
- 但时间行为和早期中间版本已经发生了明显变化

这次最新日志里：

- `last FLOW_TX_DONE = 291.923 us`
- `network drained = 292.251 us`

所以当前版本不能简单理解为“纯重构无行为变化”。

更准确的说法是：

- 结构已经稳定下来
- 但时序表现已经随着更严格的 port/QP 运行时约束而变化

这不一定是坏事，但如果你后面要继续调优，就应该把它当成真实模型变化来分析，而不是当成文档或类名层面的整理。

## 6. 现在最值得继续审的点

如果你后面还要继续抠实现，我最建议优先看：

1. `RQ` 更严格之后，时间为什么回到了更大的量级
2. 当前 `CQ` 是否需要进一步变成上层可消费的主语义接口
3. 当前 `transportWindowPackets` 和 per-qid inflight 之间的关系是否还要继续细化
4. 多出端口场景未来如何把 `LogicalQp` 绑定到多个 `PortQpRuntime`

但这些已经属于“下一轮增强/调优”了，不再是这轮“把 RoCE 架构做完整”的收尾问题。

