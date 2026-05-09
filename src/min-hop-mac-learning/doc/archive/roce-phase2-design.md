# RoCE Phase 2 设计归档

最近修改：2026-04-23

记录时间：2026-04-10

这份文档是历史设计归档，不再描述“下一阶段尚未实现的计划”。旧版 Phase 2 里列出的多项能力已经在当前代码中落到 `RdmaPortEngine`，因此继续保留旧的 TODO 口径会误导后续读者。

当前实现状态请优先看：

- [roce-current-status.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-current-status.md)
- [roce-workflow.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-workflow.md)
- [roce-qp-and-transport.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-qp-and-transport.md)

## 1. 旧 Phase 2 的原始目标

旧 Phase 2 的核心目标是把 RoCE 从“语义旁路”推进到“更像 RC transport 的网络仿真”。

当时确定的方向包括：

1. `SEND / WRITE` 作为主要对外语义。
2. `ACT` 继续是 3P 业务控制报文，不混同 transport ACK。
3. QP 显式包含最小 `SQ / RQ / CQ`。
4. 按标准可见 RoCE 头开销修正 packetization。
5. ACK、timeout、retransmission 回到 RoCE 层。
6. 接收侧从“简单记录 future PSN”升级为 bounded reorder buffer。

这些目标在当时是设计计划；现在不能再当成未实现的路线图来读。

## 2. 当前已经落地的部分

当前代码已经落地：

1. `REQUEST / ACT` 默认走控制类 `SEND` 口径。
2. `DATA` 默认走数据类 `WRITE` 口径。
3. `RoceTransportEngine` 负责 `flow/lane -> qpn` 和 opcode/txClass 决策。
4. `RdmaPortEngine::PostSend(...)` 负责显式写入 port-side `SQ`。
5. `QpRuntime` 包含最小 `sqEntries / rqInflight / completions`。
6. `RoceSimHeader::GetVisibleWireBytes(...)` 已经按 opcode 返回可见 RoCE transport 头大小。
7. `RdmaPortEngine::BuildPacketForSlice_(...)` 已经把可见 RoCE 头开销纳入分片预算。
8. `RdmaPortEngine` 维护 `m_outstandingPackets`、`m_retransTimers`、`m_pendingRetransmissions` 和 `m_recoveryTargetPsns`。
9. ACK 进入 `RdmaPortEngine::HandleAck(...)`，按累计确认 retire outstanding。
10. future packet 进入 `m_bufferedUpperPackets`，连续后再释放给 3P 业务层。

这说明旧 Phase 2 的主干能力已经不是“下一阶段计划”，而是当前实现的一部分。

## 3. 当前仍未落地或仍简化的部分

还有一些旧 Phase 2 中没有完全解决、或者只做了最小模型的内容：

1. `RQ` 仍是最小 receive slot 模型，不是完整 posted receive/RNR。
2. `CQ` 仍是内部 completion 队列，不是上层唯一完成语义。
3. QP bringup 仍由代码自动推进，不是显式 verbs 参数配置。
4. `SRQ / MR / PD` 没有建模。
5. 主路径仍不是 `RoCEv2 over UDP/IP`。
6. `ECN / CNP / DCQCN` 没有形成完整闭环。
7. 多出端口绑定与多 port QP 运行时还没有成为主路径。

这些才是后续如果继续追现实 RoCE 行为时更值得讨论的内容。

## 4. 旧实施顺序的当前状态

旧文档里曾经把 Phase 2 拆成：

1. 修字节模型。
2. 补最小 `SQ/RQ/CQ`。
3. 补 ACK / 重传 / 有限乱序窗口。

现在对应状态是：

1. 字节模型：已落地最小版本。
2. `SQ/RQ/CQ`：已落地最小版本。
3. ACK / 重传 / reorder：已落地最小闭环。

因此这个文件只保留设计脉络，不再作为当前开发 TODO 使用。

## 5. 后续更准确的方向

如果后面继续增强 RoCE，建议不要再重复旧 Phase 2 的 TODO，而是围绕这些新问题讨论：

1. 是否需要补更真实的 `RQ/RNR` 行为。
2. `CQ` 是否要变成上层完成语义的主接口。
3. 是否引入 `RoCEv2 / UDP/IP / ECN / CNP / DCQCN`。
4. 多出端口场景下 `LogicalQp` 和 `PortQpRuntime` 怎么绑定。
5. 大规模仿真里 `transportWindowPackets`、PFC、per-qid inflight 是否会共同造成过强 admission control。
