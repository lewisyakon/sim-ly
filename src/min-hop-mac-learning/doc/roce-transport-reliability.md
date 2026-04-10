# RoCE 传输可靠性说明

记录时间：2026-04-10

这份文档按当前 `RdmaPortEngine` 主路径重写。旧版文档里大量函数名和状态还停留在 `my_sendflowclient` 承担 ACK/重传/reorder 的阶段，已经不适合作为当前实现说明。

## 1. 当前可靠性能力

当前 RoCE transport 已经不是“无窗口、无 ACK、无重传”的 stage 1 状态。

当前已经具备：

1. 按可见 RoCE 头重算的分片口径。
2. transport ACK。
3. 每 QP outstanding packet table。
4. 每 QP `transportWindowPackets` 在途窗口。
5. 每 QP oldest-unacked retransmission timer。
6. timeout 后 pending retransmission / recovery。
7. recovery 期间阻塞该 QP 发新普通包。
8. ACK 到达后累计确认并 retire outstanding。
9. future packet 的接收侧重排缓冲。
10. bounded reorder window / overflow 语义。
11. ACK 使用控制类优先发送。
12. pause-aware port admission。
13. per-qid inflight packet 限制。
14. QP 空闲后的 runtime 状态释放。

主代码位置：

- [rdma-port-engine.h](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/rdma-port-engine.h)
- [rdma-port-engine.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/rdma-port-engine.cc)
- [roce-qp-engine.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/roce-qp-engine.cc)
- [roce-sim-header.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/roce-sim-header.cc)

## 2. 分片与线长口径

当前 `RoceSimHeader::GetVisibleWireBytes(...)` 按 opcode 返回等效可见 transport 头：

1. `SEND -> 12B`
2. `WRITE / READ_REQ -> 28B`
3. `ACK / READ_RESP -> 16B`

`RdmaPortEngine::BuildPacketForSlice_(...)` 会把这部分可见头开销纳入预算。`FlowTagHeader / ThreePHeader / HopCountHeader` 仍属于仿真业务控制内容，不等价于现实 RoCEv2 的 UDP/IP 外层。

## 3. 发送窗口与 blocked QP

当前发送端的在途约束在 `RdmaPortEngine` 里完成。

关键状态包括：

1. `m_outstandingPackets`
2. `m_retransTimers`
3. `m_pendingRetransmissions`
4. `m_recoveryTargetPsns`

`CollectBlockedQps_()` 会把下面这些 QP 放入 blocked 集合：

1. outstanding 数量达到 `transportWindowPackets` 的 QP。
2. 正处于 recovery 的 QP。
3. 对应 egress queue 因 pause 或 per-qid inflight limit 暂时不能准入的 QP。

这意味着当前 offered load 不再只是由上层 workload 一口气灌入决定，而是会被 transport window、recovery、PFC/pause 和端口准入共同限制。

## 4. ACK 处理

当前 ACK 是 transport 层 ACK，不是 3P 的 `ACT`。

接收侧普通包处理后会通过 `MaybeSendAck_(...)` 生成 ACK。ACK 包使用控制类发送，并通过 `RdmaDispatchTag::SetTransportAck(true)` 标记。

发送侧收到 ACK 后进入：

- `RdmaPortEngine::HandleAck(...)`

当前 ACK 处理是累计确认：

1. 读取 `ackPsn`。
2. 清除该 QP 上 `psn <= ackPsn` 的 outstanding packet。
3. 如果 outstanding 清空，取消 retransmission timer。
4. 如果仍有 outstanding，重新 arm timer。
5. 如果 `ackPsn` 已经覆盖 recovery target，则退出 recovery。
6. 尝试 drain 后续 pending/scheduler work。

## 5. timeout 与重传

当前 timer 是每 QP 一个 oldest-unacked timer，而不是每包一个独立 timer。

timeout 后的行为是：

1. 找到该 QP 最老 outstanding packet。
2. 如果 retry 已达到 `maxRetransmissions`，丢弃该 outstanding 并清理 recovery 状态。
3. 否则把该 packet 标记为 retransmission。
4. 写入 `m_pendingRetransmissions` 和 `m_recoveryTargetPsns`。
5. 放回 port pending queue。
6. 由正常 `TrySendNow_()` 路径重新发送。

注意一个关键点：

- retry 计数在真正重发成功时增加，而不是 timeout 回调一触发就增加。

这避免了 PFC/pause 期间 timer 空转把 retry 次数瞬间烧光。

## 6. 接收侧 reorder

普通包到达 `RdmaPortEngine::HandleIncomingPacket(...)` 后，会先检查 `RQ`，再进入 `RoceQpEngine::OnRxHeader(...)`。

当前接收侧行为是：

1. `psn < expectedRxPsn`
   - 视为旧包/重复包，不上送业务层。

2. `psn == expectedRxPsn`
   - 推进接收状态，产生 recv completion，释放对应 `RqEntry`。

3. `expectedRxPsn < psn < expectedRxPsn + reorderWindowPackets`
   - 缓存在 `m_bufferedUpperPackets`，暂不上送业务层。

4. `psn >= expectedRxPsn + reorderWindowPackets`
   - 触发 reorder overflow 语义，不进入无限缓存。

连续缺口补齐后，`CollectReadyBufferedPackets_()` 会把已经连续的 buffered packet 释放给 `sendflowclient::ProcessThreePhasePayload_(...)`。

## 7. completion 与 QP 清理

当前 completion 是 `QpRuntime::completions` 内部队列：

1. `CompleteSqEntry_()` 产生 send completion。
2. 接收侧成功上送产生 recv completion。
3. CQ 满时会丢弃最老 completion，并记录 `[ROCE][CQ][OVERFLOW]`。

QP 空闲后，`MaybeReleaseIdleTxQp_(...)` 会检查：

1. `sqEntries`
2. `rqInflight`
3. `m_bufferedUpperPackets`
4. `m_outstandingPackets`
5. `m_pendingRetransmissions`
6. `m_recoveryTargetPsns`
7. `m_retransTimers`

全部为空后才释放该 QP runtime。这一点和前面内存泄漏调查有关：长期运行时，服务对象结束后应尽可能清掉对应状态。

## 8. 当前仍未完成的地方

虽然当前可靠性闭环已经明显强于早期版本，但仍不能宣称完整等价真实 RC/RNIC。

还保留的简化包括：

1. `RQ` 不是完整 `post_recv / RNR / rnr_retry` 语义。
2. `CQ` 仍未成为上层唯一完成驱动。
3. retransmission 策略仍是较简化的 timeout/recovery。
4. 没有动态 RTO，例如 SRTT/RTTVAR。
5. 没有完整 `ECN / CNP / DCQCN` 拥塞控制。
6. 主路径不是 `RoCEv2 over UDP/IP`。
7. 多出端口场景还没有进入主逻辑。

## 9. 与内存问题的关系

当前这份文档只讨论 RoCE transport 可靠性。最近 50 层模型中确认的主要内存峰值不是由 RoCE outstanding/reorder 长期堆积造成，而是 AstraSim 收尾阶段的 `dimension_utilization` CSV 统计路径。

对应记录见：

- [memory-leak-investigation.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/memory-leak-investigation.md)

## 10. 外部和本地参考边界

前期判断参考过 NVIDIA RoCE 文档、`ibv_modify_qp(3)`、`ibv_post_recv(3)`、NVIDIA OOO data placement 文档，以及本地 UB transport/flow-control 代码。

这些资料支持的高层结论是：

1. 无损网络/PFC 与 RC QP 的 ACK、timeout、retry、outstanding 状态不是互斥关系。
2. 接收侧 bounded out-of-order 处理在工程上是合理方向。
3. 当前项目可以做 `RC-like` 网络行为近似，但不能把简化模型说成完整 RNIC。
