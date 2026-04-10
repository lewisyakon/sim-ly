# QP 与传输职责说明

记录时间：2026-04-10

这份文档按当前代码重新整理 `QP / SQ / RQ / CQ` 和 transport 职责。旧版文档里“当前还没有 ACK / 重传 / inflight window / SQ/RQ/CQ”的说法已经过时。

## 1. 现实中的 QP 是什么

`QP` 是 `Queue Pair`，也就是 RDMA 通信端点。应用不是直接对“整个 NIC”发 RDMA 操作，而是对某个 QP 投递 work request。

一个较完整的 RC QP 通常包含：

1. `SQ`
   - 保存本地发送工作请求。

2. `RQ`
   - 保存本地接收缓冲描述符。

3. `CQ`
   - 记录发送或接收完成。

4. QP 状态
   - 典型状态包括 `RESET / INIT / RTR / RTS / ERR`。

5. PSN 与可靠传输状态
   - 发送 PSN、期望接收 PSN、ACK、retry、timeout 等。

真实 `post_send / post_recv` 是本地 verbs 动作，不是网络中的控制报文。发送端不会通过网络“发一个 post 包”去改对端 `RQ`。

## 2. 当前项目里的职责拆分

当前项目里不要再把 QP 行为全部归到 `sendflowclient`。

当前拆分是：

1. `sendflowclient`
   - Astra-Sim facade
   - 3P 业务层
   - 把 `REQUEST / DATA / ACT` 整理成待发送业务 work

2. `RoceTransportEngine`
   - 逻辑 transport 层
   - 根据 `flow + lane` 分配稳定 synthetic `qpn`
   - 决定 opcode、`txClass`、`workId`

3. `RdmaPortEngine`
   - 每个物理 port 上的 RoCE runtime
   - 管理 port-bound `QpRuntime`
   - 负责 `SQ / RQ / CQ`、packetize、ACK、重传、reorder、pause-aware 调度

这条边界是当前版本最关键的变化：`RoceTransportEngine` 不负责真实发包，`RdmaPortEngine` 才是真正的 packet runtime。

## 3. 当前 synthetic QPN 的口径

真实 RNIC 收到包后，会根据包头中的 `QPN` 找到对应 QP 上下文。

当前项目里做了简化：

- 不是直接 `qpn = tag`
- 而是按 `flow = {tag, src, dst}` 和 lane 分配 synthetic `qpn`

这样做的原因是：

1. 不能假设 `tag` 全局唯一。
2. 当前可以把 `{tag, src, dst}` 视为业务 flow key。
3. `REQUEST / DATA / ACT` 分属不同 lane，需要避免不同语义共享同一个 QP 状态。

因此当前更准确的说法是：

- `qpn` 是当前仿真中为 flow/lane 分配的稳定局部编号。
- 它服务于网络行为建模，不等价于真实 RNIC 的完整 QP 创建与连接过程。

## 4. 当前 `QpRuntime` 已经包含什么

当前 port-bound `QpRuntime` 定义在：

- [rdma-port-engine.h](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/rdma-port-engine.h#L127)

当前包含：

1. `sqDepth`
2. `rqDepth`
3. `cqDepth`
4. `sqEntries`
5. `rqInflight`
6. `completions`
7. send/recv completion 计数
8. CQ overflow 计数

`RdmaPortEngine` 还在 QP 维度维护：

1. `m_bufferedUpperPackets`
2. `m_outstandingPackets`
3. `m_retransTimers`
4. `m_pendingRetransmissions`
5. `m_recoveryTargetPsns`

所以当前不能再说“QP 只是状态和 PSN 容器”。它已经承担了最小 transport endpoint 的职责。

## 5. 当前 `SQ / RQ / CQ` 的真实程度

### 5.1 `SQ`

发送入口是：

- `RdmaPortEngine::PostSend(...)`

它会把一个业务 work 写入对应 `QpRuntime::sqEntries`。当最后一个非重传 slice 成功发出后，`CompleteSqEntry_()` 会生成 send completion，并移除对应 `SqEntry`。

这已经是显式 `SQ` 的最小模型，但它仍然不是完整 verbs WQE 模型。

### 5.2 `RQ`

接收普通 RoCE packet 前会先走 `AcquireRqEntry_()`。

如果 `rqInflight` 达到 `rqDepth`，packet 会在协议状态推进前被拒绝，并产生 `[ROCE][RQ][FULL]` 日志。

这已经能表达“接收端资源不足会影响 packet 处理”的网络行为，但还不是完整的 `post_recv / RNR / rnr_retry` 语义。

### 5.3 `CQ`

当前 `CQ` 是 `QpRuntime::completions` 中的最小 completion 队列，支持 send/recv completion 记录和 overflow 计数。

它还没有成为上层唯一的完成驱动。当前业务完成仍主要由 3P 的 `ACT` 闭环触发。

## 6. 当前 transport 行为

当前 transport 行为已经包括：

1. 按 opcode 生成 `RoceSimHeader`
2. 按可见 RoCE 头开销调整 packetization
3. per-QP outstanding packet table
4. transport ACK
5. cumulative ACK retire
6. per-QP oldest-unacked timer
7. timeout 后 pending retransmission / recovery
8. recovery 中阻塞该 QP 发新包
9. 接收侧 bounded reorder buffer
10. stale/duplicate packet 识别
11. pause-aware port admission
12. per-qid inflight packet limit

对应主代码在：

- [rdma-port-engine.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/rdma-port-engine.cc)
- [roce-qp-engine.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/roce-qp-engine.cc)
- [roce-sim-header.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/roce-sim-header.cc)

## 7. 当前仍不能宣称什么

当前可以说它是 `RC-like` 的网络仿真模型，但不应该说它已经完整等价于现实 RNIC。

仍然不能过度宣称的点包括：

1. 不是完整 verbs 级 QP 生命周期。
2. 没有完整 `PD / MR / SRQ` 资源模型。
3. `RQ` 不是完整 posted receive + RNR 语义。
4. `CQ` 不是上层唯一完成语义。
5. 主路径不是现实常见的 `RoCEv2 over UDP/IP`。
6. 没有完整 `ECN / CNP / DCQCN` 拥塞控制闭环。
7. 多出端口 QP 绑定还没有进入主逻辑。

最稳妥的对外表述是：

- 当前版本已经把 RoCE 发送、接收、ACK、重传、reorder 和 port admission 收到 `RdmaPortEngine`。
- 当前版本已经是面向网络行为的 `RC-like` RoCE 近似模型。
- 当前版本仍不是完整 RNIC/verbs/RoCEv2/DCQCN 模型。

## 8. 与 UB 的参考关系

UB 的强项仍然在 transport behavior：ACK、inflight、RTO、重传、乱序 bitmap、流控等都更成体系。

但当前项目已经不再停留在早期“只有 QP 语义检查”的阶段。现在和 UB 的差异更准确地说是：

| 维度 | 当前项目 | UB |
|---|---|---|
| 上层模型 | Astra-Sim + 3P | WQE / Task / TP |
| transport 端点 | port-bound `QpRuntime` | `UbTransportChannel / TP / Jetty` |
| 可靠性 | ACK / timeout / retransmission / reorder 已有最小闭环 | 更完整的 transport 行为 |
| 流控 | PFC pause-aware admission + per-qid inflight | PFC / CBFC / shared credit |
| 拥塞控制 | 尚无完整 ECN/CNP/DCQCN | UB 内有更明确的拥塞控制模块 |

因此 UB 仍适合作为后续增强参考，但不能再把“补 ACK / 重传 / SQ/RQ/CQ”描述成当前项目完全没做的下一阶段任务。
