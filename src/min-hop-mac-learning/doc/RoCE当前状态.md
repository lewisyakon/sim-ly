# RoCE 当前状态

最近修改：2026-05-19

这份文档只回答当前工作区里的 RoCE/端口/队列实现已经做到哪一步，以及最近一次 AllToAll 排查能证明什么。早期阶段性计划和已经落地的 TODO 不再作为当前状态描述。

## 1. 当前版本的准确定位

当前版本是服务网络仿真的 `RC-like` RoCE 近似模型，不是完整 RNIC/verbs/RoCEv2/DCQCN 模型。

当前主路径已经完成解耦：

1. `sendflowclient`
   - Astra-Sim facade
   - 3P 业务语义
   - 把上层业务请求交给 RoCE transport/port 层
   - 接收侧在 transport 交付 payload 后执行 `ProcessThreePhasePayload_(...)`

2. `RoceTransportEngine`
   - 逻辑层
   - `flow/lane -> qpn`
   - `REQUEST/DATA/ACT -> opcode/txClass`
   - 维护 3P FSM
   - 不负责 packet、ACK、重传和物理端口调度

3. `RdmaPortEngine`
   - 每个 `PointToPointNetDevice` 上的 port-bound RoCE runtime
   - 维护最小 `QP/SQ/RQ/CQ`
   - 负责 packetize、ACK、retransmission、reorder
   - 做 pause-aware 发送准入和 per-qid inflight 限制

4. `PointToPointNetDevice + PppRoundRobinQueue`
   - 真实物理发送队列
   - PPP 子队列当前按目的 MAC hash 分桶
   - `Dequeue()` 跨非空、非暂停子队列 round-robin

5. `MinHopBridgeNetDevice`
   - 学表、ECMP/flood、真实 `outPort` 选择
   - ingress 是每端口一个 aggregate queue，不是 per-qid 队列
   - 每个 ingress 包独立 token，经过 `switch_delay` 后 drain/retry

也就是说，当前 RoCE 主路径已经不再依赖旧的 `sendflowclient` 内部 fallback；packet runtime 已经下沉到端口。

## 2. 当前已经完成的内容

当前代码里可以直接确认的能力包括：

1. `REQUEST/DATA/ACT` 三条逻辑 lane。
2. `flow/lane -> qpn`。
3. port-bound `QpRuntime`。
4. 最小 `SQ`。
5. 最小 `RQ`。
6. 最小 `CQ`。
7. `Control > Data` 调度。
8. send-side packetization。
9. `RoceSimHeader` 生成。
10. 按 opcode 计算可见 RoCE 头开销。
11. RX reorder buffer。
12. transport ACK。
13. ACK retire outstanding。
14. per-QP retransmission timer。
15. pending retransmission / recovery。
16. 真正重发成功时才 `retry++`。
17. pause-aware 发送准入。
18. per-qid inflight packet 限制。
19. send/recv completion 记录。
20. QP 空闲后的 runtime 状态释放。
21. `innerBridge + optional externalBridge` 端点布局。
22. 外侧 bridge 的 `TerminalLearningOnly` 行为。
23. `FlowIo` 与 bridge/PFC 的本地 pause 映射。
24. bridge aggregate ingress queue 与 token-based drain/retry。

对应的主要代码位置是：

- [rdma-port-engine.h](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/rdma-port-engine.h)
- [rdma-port-engine.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/rdma-port-engine.cc)
- [roce-transport-engine.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/roce-transport-engine.cc)
- [roce-sim-header.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/roce-sim-header.cc)
- [PppRoundRobinQueue.h](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/PppRoundRobinQueue.h)
- [min-hop-bridge-net-device.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/min-hop-bridge-net-device.cc)

## 3. 最近验证：128 GPU AllToAll

最近验证使用：

- [config.json](/home/lja/sim-ly/sim-ly/simconfig/config.json)
- [128_test.txt](/home/lja/sim-ly/sim-ly/simworkload/128_test.txt)
- [128_random_test.log](/home/lja/sim-ly/sim-ly/simlog/128_random_test.log)

当前配置中 `pfc.drop_tail_queue_num = 13`。结果：

1. `network drained = 12.108us`
2. `last FLOW_TX_DONE = 11.566us`
3. `RX_DONE` 全局范围约 `10058ns..11010ns`，spread 约 `0.952us`
4. `TX_DONE` 全局范围约 `10737ns..11566ns`，spread 约 `0.829us`
5. 未观察到 `DROP/PFC/RETRANS/TIMEOUT/NO_PATH/CALLBACK_LATE/SQ_FULL/RQ_FULL`

和之前 16 队列临时结果相比：

| 指标 | 16 队列旧结果 | 13 队列当前结果 |
|---|---:|---:|
| `network drained` | 18.491us | 12.108us |
| `last FLOW_TX_DONE` | 17.949us | 11.566us |
| `RX_DONE` 全局 spread | 14.364us | 0.952us |
| `TX_DONE` 全局 spread | 13.596us | 0.829us |
| 每个 dst 的 `RX_DONE` spread 平均值 | 4.150us | 0.256us |
| 每个 dst 的 `TX_DONE` spread 平均值 | 4.067us | 0.224us |

重要解释：

1. 上层 DATA 触发节奏没有变，仍是 `565ns..613ns` 范围内触发。
2. 单源相邻 DATA 触发仍主要是 `4ns` 间隔，这是 REQUEST 解锁节奏，不是 PPP 队列导致的。
3. 性能下降的主因在网络侧排队形态，不在上层触发时序。
4. 16 队列下，目的 MAC hash 与 `%16` 形成结构性碰撞，单源多个目的流落入同一个 PPP 子队列，RR 退化成该子队列内 FIFO。
5. 13 队列打破了这类别名碰撞，因此显著降低尾部时延。

这个验证说明：当前 PPP RR 机制本身能轮询，但分类键过窄。它只按目的 MAC 分桶，不是 flow-aware 调度，因此队列数和地址分配规律会直接影响公平性。

## 4. 当前保留的简化

当前仍保留这些简化：

1. `CQ` 仍是 `QpRuntime` 内的 completion 队列，不是完整独立对象体系。
2. completion 还不是上层完成语义的唯一驱动，`TX_DONE` 仍由 3P 的 `ACT` 闭环触发。
3. `RQ` 是最小 receive slot 模型，不是完整 `post_recv/RNR` 语义。
4. `SRQ` 没有实现。
5. QP bringup 被自动压缩为 `Reset -> Init -> RTR -> RTS`。
6. 每个 work 仍只绑定一个物理 egress port，真正的多 rail striping 还没有进入主逻辑。
7. 主路径不是现实常见的 `RoCEv2 over UDP/IP`，而是自定义二层 RoCE-like transport。
8. 没有完整 `ECN/CNP/DCQCN` 拥塞控制闭环。
9. PPP 子队列分类不是 flow-aware，遇到规律 MAC 和特定队列数会退化。
10. bridge ingress 是 aggregate queue，不能用旧的 per-qid ingress queue 口径解释。

## 5. 当前最重要的风险提示

功能闭环和“现实 RoCE 网络行为足够像”不是一回事。当前版本已经具备 ACK、重传、reorder、PFC、端口准入等关键网络行为，但大规模 AI 集群里的尾延迟、公平性、PFC 活跃度、ECMP/ECN/DCQCN 行为，仍需要按具体 workload 和拓扑验证。

近期最直接的风险是 PPP 队列分类：

- `drop_tail_queue_num=13` 是对当前 128 AllToAll 有效的 workaround。
- 它不是长期机制本身。
- 更稳健的方向是让 DATA 的 qid 分类至少包含 `ThreePHeader(src,dst,tag)` 这类 flow key，或进一步做 per-flow DRR。
- 如果只继续调队列数，换规模、换拓扑或换地址分配后仍可能再次碰撞。

## 6. 现在最值得继续审的点

后续如果继续审实现，优先级建议是：

1. PPP/RDMA qid classifier 是否要从目的 MAC hash 改成 flow-aware hash。
2. `RdmaPortEngine::ClassifyQueueId_()` 与 `PppRoundRobinQueue::CalculateQueueIndex()` 是否必须共用同一套分类逻辑，避免 pause/admission 和实际入队 qid 不一致。
3. `drop_tail_queue_num` 是否应拆成“调度 bucket 数”和“PFC qid 数”，避免一个配置同时改变多个语义。
4. `transportWindowPackets`、per-qid inflight 和 PFC backpressure 的耦合是否会在大规模拓扑中制造过强限速。
5. `RQ/CQ` 是否需要进一步接近真实 posted receive / completion 语义。
6. 如果要对标现实 AI 集群，是否需要补 `RoCEv2/UDP/IP/ECN/CNP/DCQCN` 这一层。
