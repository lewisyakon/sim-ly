# RoCE Scheduler Stage 1 归档

记录时间：2026-04-10

这份文档是历史归档。旧版 `Stage 1` 文档描述的是 RoCE 调度还主要在 `sendflowclient + RoceTxScheduler` 内部完成的阶段；当前主路径已经迁移到 port-bound `RdmaPortEngine`。

当前调度和发包路径请优先看：

- [roce-workflow.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-workflow.md)
- [roce-qp-and-transport.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-qp-and-transport.md)
- [roce-current-status.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-current-status.md)

## 1. 旧 Stage 1 当时解决了什么

旧 Stage 1 的目标是：

1. `ROCE=true` 时不再 legacy 直发。
2. 把发送入口迁移到 QP/scheduler 层。
3. 支持控制类优先。
4. 支持数据类按活跃 QP 轮询。
5. 支持 DATA 逐 slice 出网。

这些内容当时是重要中间态，但现在已经不是当前实现的完整描述。

## 2. 当前主路径已经变化

当前发送路径是：

```text
sendflowclient
  -> RoceTransportEngine::PrepareTxWork
  -> resolve egress port
  -> RdmaPortEngine::PostSend
  -> QpRuntime::sqEntries
  -> RoceTxScheduler
  -> RdmaPortEngine::BuildPacketForSlice_
  -> RdmaPortEngine::TrySendNow_
  -> PointToPointNetDevice::SendFrom
```

也就是说：

1. `sendflowclient` 仍是业务入口，但不是 packet runtime。
2. `RoceTxScheduler` 仍在使用，但由 `RdmaPortEngine` 持有和调度。
3. ACK、重传、reorder、pause-aware admission 都已经进入 `RdmaPortEngine`。

## 3. 旧文档中过时的说法

旧文档里这些判断已经过时：

1. “没有 transport ACK 驱动推进。”
2. “没有 inflight/send window。”
3. “没有 timeout/retransmission。”
4. “当前只停在 stage 1。”
5. “ACK/重传属于后续 Phase 2。”

当前代码里已经能直接看到：

1. `m_outstandingPackets`
2. `m_retransTimers`
3. `m_pendingRetransmissions`
4. `m_recoveryTargetPsns`
5. `CollectBlockedQps_()`
6. `HandleAck(...)`
7. `MaybeSendAck_(...)`

因此这份文件不再保留旧的逐段实现说明，避免后续误读。

## 4. 仍然有价值的历史结论

旧 Stage 1 里仍然有两点历史结论值得保留：

1. 发送调度从 legacy 直发迁到 QP/scheduler 层，是 RoCE 主路径成型的关键一步。
2. 当时“先收敛发送入口，再补 ACK/重传”的切分是合理的，因为它避免了调度问题和可靠传输问题同时失控。

但从 2026-04-10 当前代码看，这一阶段已经结束。

## 5. 后续如果继续研究调度

后续调度问题不应再叫 `Stage 1`。更准确的问题是：

1. 当前 `RdmaPortEngine::DispatchFromScheduler_()` 会尽量 drain scheduler，直到窗口、recovery、pause 或 per-qid inflight 阻塞。
2. 这仍不完全等价于真实 NIC 基于 wire-time completion 的 dequeue。
3. 如果要进一步贴近真实设备节拍，应重新讨论“设备发送完成事件驱动的 pull 调度”或“按 slice wire time pacing”。

这两个候选方案记录在：

- [roce-future-scheduling-options.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-future-scheduling-options.md)
