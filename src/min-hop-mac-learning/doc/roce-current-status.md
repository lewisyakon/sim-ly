# RoCE 当前状态

记录时间：2026-04-10

这份文档只回答当前工作区里 RoCE 实现已经做到哪一步，以及最近一次本地日志能证明什么。过往阶段性计划请不要从这里反推当前状态。

## 1. 当前版本的准确定位

当前版本已经完成本轮 RoCE 主路径解耦：

1. `sendflowclient`
   - Astra-Sim facade
   - 3P 业务语义
   - 负责把上层业务请求交给 RoCE transport/port 层

2. `RoceTransportEngine`
   - 逻辑层
   - `flow/lane -> qpn`
   - `REQUEST / DATA / ACT -> opcode / txClass`
   - 不负责 packet、ACK、重传和 port 调度

3. `RdmaPortEngine`
   - port-bound RoCE runtime
   - `QP / SQ / RQ / CQ` 的最小运行时模型
   - packetize、ACK、retransmission、reorder
   - pause-aware 端口准入和 per-qid inflight 限制

也就是说，当前 RoCE 主路径已经不再依赖旧的 `sendflowclient` 内部 fallback。真正的 packet runtime 已经下沉到挂在 `PointToPointNetDevice` 上的 `RdmaPortEngine`。

## 2. 当前已经完成的内容

当前代码里可以直接确认的能力包括：

1. `REQUEST / DATA / ACT` 三条逻辑 lane
2. `flow + lane -> qpn`
3. port-bound `QpRuntime`
4. 最小 `SQ`
5. 最小 `RQ`
6. 最小 `CQ`
7. `Control > Data` 调度
8. send-side packetization
9. `RoceSimHeader` 生成
10. 按 opcode 计算可见 RoCE 头开销
11. RX reorder buffer
12. transport ACK
13. ACK retire outstanding
14. per-QP retransmission timer
15. pending retransmission / recovery
16. 真正重发成功时才 `retry++`
17. pause-aware 发送准入
18. per-qid inflight packet 限制
19. send/recv completion 记录
20. QP 空闲后的 runtime 状态释放

对应的主要代码位置是：

- [rdma-port-engine.h](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/rdma-port-engine.h)
- [rdma-port-engine.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/rdma-port-engine.cc)
- [roce-transport-engine.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/roce-transport-engine.cc)
- [roce-sim-header.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/roce-sim-header.cc)

## 3. 最新真实验证

当前仓里最近一次小模型 `ROCE=true` 运行使用的是：

- [config.json](/home/lja/sim-ly/sim-ly/simconfig/config.json)
- [microAllReduce.txt](/home/lja/sim-ly/sim-ly/simworkload/microAllReduce.txt)
- [simulation.log](/home/lja/sim-ly/sim-ly/simulation.log)

这次日志降级后，不再打印完整 `FLOW_TX_DONE / FLOW_RX_DONE` 明细；因此这里不再沿用旧文档里的 `TX_DONE=48 / RX_DONE=48` 口径。

当前日志能直接确认：

1. `REQ_DONE = 144`
2. `total send count = 144`
3. `total events = 1171473`
4. `last FLOW_TX_DONE since_workload_start(us) = 15562.8`
5. `network drained since_workload_start(us) = 15563.1`

对应日志：

- [simulation.log:1186](/home/lja/sim-ly/sim-ly/simulation.log#L1186)
- [simulation.log:1187](/home/lja/sim-ly/sim-ly/simulation.log#L1187)
- [simulation.log:1189](/home/lja/sim-ly/sim-ly/simulation.log#L1189)
- [simulation.log:1190](/home/lja/sim-ly/sim-ly/simulation.log#L1190)

同时，当前日志没有搜到这些异常项：

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

1. 当前小模型上功能闭环是通的。
2. RoCE 主路径没有在最近的内存探针清理和 CSV 关闭后被打断。
3. 由于当前日志级别降低，不能再用旧的 TX/RX 逐条明细结论做精确收发计数。

## 4. 当前保留的简化

当前仍不是完整 verbs 级 RNIC 模型。更准确地说，它是服务网络仿真的 `RC-like` RoCE 近似模型。

当前仍保留这些简化：

1. `CQ` 仍是 `QpRuntime` 内的 completion 队列，不是独立对象体系。
2. completion 目前还没有成为上层完成语义的唯一驱动，`TX_DONE` 仍由 3P 的 `ACT` 闭环触发。
3. `RQ` 是最小 receive slot 模型，不是完整 `post_recv`/RNR 语义。
4. `SRQ` 没有实现。
5. QP bringup 被自动压缩为 `Reset -> Init -> RTR -> RTS`。
6. 当前主实现仍针对“出端口唯一”的路径，多出端口绑定策略还没有成为主逻辑。
7. 主路径不是现实中常见的 `RoCEv2 over UDP/IP`，而是自定义二层 RoCE-like transport。
8. 还没有完整 `ECN/CNP/DCQCN` 拥塞控制闭环。

## 5. 当前最重要的风险提示

功能闭环和“现实 RoCE 网络行为足够像”不是一回事。当前版本已经比早期 stage 1 更接近 transport 行为，但大规模 AI 集群里的尾延迟、PFC 活跃度、ECMP/ECN/DCQCN 行为，仍然不能只靠这一次小模型日志下结论。

这次最新日志里的时间口径是：

- `last FLOW_TX_DONE = 15562.8 us`
- `network drained = 15563.1 us`

这和旧文档中的早期小模型时间不是同一次验证。后续比较时应使用 workload、配置和日志级别都一致的一组实验，不要把不同阶段的小模型结果直接混在一起。

## 6. 现在最值得继续审的点

如果后面继续审实现，我建议优先看：

1. 当前 `RQ` 模型是否需要进一步接近真实 posted receive / RNR。
2. `CQ` 是否需要从内部统计队列升级成上层可消费的完成语义接口。
3. `transportWindowPackets`、per-qid inflight 和 PFC backpressure 的耦合是否会在大规模拓扑中制造过强限速。
4. 多出端口场景未来如何把 `LogicalQp` 映射到多个 `PortQpRuntime`。
5. 是否需要补 `RoCEv2 / UDP/IP / ECN / CNP / DCQCN` 这一层，以提高与现实 AI 集群网络行为的匹配度。
