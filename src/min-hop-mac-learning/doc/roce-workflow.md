# RoCE 工作流说明

最近修改：2026-04-23

这份文档讲的是“当前最终实现里，一条流怎样从上层下来，经过逻辑层和端口层，最后从物理 port 发出去”。

## 1. 总览

当前 `ROCE=true` 的主链路是：

```text
Astra-Sim
  -> sendflowclient
  -> RoceTransportEngine::PrepareTxWork
  -> resolve egress port
  -> RdmaPortEngine::PostSend
  -> port-side SQ
  -> RoceTxScheduler
  -> packetize + RoceSimHeader
  -> PointToPointNetDevice::SendFrom
  -> network
  -> peer ingress port
  -> sendflowclient::HandleRead
  -> ingress RdmaPortEngine::HandleIncomingPacket
  -> ready upper payload
  -> sendflowclient::ProcessThreePhasePayload_
```

接收侧 transport ACK 也完全在 ingress `RdmaPortEngine` 里处理。

## 2. 发送方向

### 2.1 Astra-Sim 把请求交给 `sendflowclient`

上层发送请求最终还是通过：

- `TriggerSendTo_now(...)`

进入 `sendflowclient`。

这里先决定业务相位：

1. `REQUEST`
2. `DATA`
3. `ACT`

如果 `ROCE=false`：

- 仍然走旧的 `SendPacket(...)` 传统 3P 路径。

如果 `ROCE=true`：

- 进入 `EnqueueRoceWork_(...)`

### 2.2 `sendflowclient` 组织 `RoceTxWorkItem`

`sendflowclient` 在这一层只负责准备业务语义字段：

1. `flowKey`
2. `dest`
3. `sendMode`
4. `totalBytes`
5. `remainingBytes`
6. `totalPackets`
7. `FlowTagHeader`
8. `REQUEST/DATA/ACT`

这一刻还没有真正 packetize。

### 2.3 `RoceTransportEngine` 把业务 work 变成逻辑 work

`RoceTransportEngine::PrepareTxWork(...)` 会写入：

1. `qpn`
2. `workId`
3. `opcode`
4. `userPhase`
5. `txClass`

它做的是逻辑决策：

1. 这条 flow 属于哪个 lane
2. 这个 lane 用哪个 `qpn`
3. 这个业务动作在 RoCE 上是什么 opcode

它不负责 packet，不负责 ACK，不负责重传。

### 2.4 `sendflowclient` 解析实际 egress port

当前虽然还没有做单 work 多口并发，但实现里已经把最终发送落点放在具体 port 上，而且端点布局已经支持 `innerBridge + optional externalBridge`。

发送时：

1. `External`
   - 先尝试把 `m_outDeviceIndex` 直接当物理口
   - 如果 `m_outDeviceIndex` 实际上是 `externalBridge`，则由 `externalBridge` 的 `ResolveLocalInjectionEgressPort(...)` 找真实 `outPort`
2. `Loopback` 且 `mindex == m_outDeviceIndex`
   - 走与 `External` 相同的外侧逻辑
3. 其他 `Internal/Loopback`
   - 通过 `m_bridgeDevice`，也就是 `innerBridge`，解析真实 `outPort`

然后 `sendflowclient` 找到这个 `outPort` 上 aggregate 的 `RdmaPortEngine`。

### 2.5 `RdmaPortEngine::PostSend`

真正的发送入口是：

- `RdmaPortEngine::PostSend(const RoceTxWorkItem& item)`

这一步会发生两件事：

1. 保证这个 `qpKey` 在本 port 上有 `QpRuntime`
2. 把该 work 写入这个 QP 的 `SQ`

如果 `SQ` 已满，会直接拒绝。

## 3. 端口侧 `QP/SQ/RQ/CQ`

### 3.1 `QpRuntime`

当前每个 port 上，每个 `qpKey` 都有自己的 `QpRuntime`。

里边至少有：

1. `sqDepth`
2. `rqDepth`
3. `cqDepth`
4. `sqEntries`
5. `rqInflight`
6. `completions`

### 3.2 `SQ`

`SQ` 的最小单位是 `SqEntry`，不是一个 MTU packet。

也就是说：

- 上层先 post 一个 work
- port engine 之后才把它切成多个 packet

### 3.3 `RQ`

接收时，每个 RoCE packet 要先占用一个 `RqEntry`。

如果 `RQ` 满：

- packet 会在进入协议推进前就被拒绝

这样可以避免“QP 状态已经推进，但接收资源其实已经不够”的不一致。

### 3.4 `CQ`

当前 `CQ` 是最小模型：

1. send completion
2. recv completion
3. 可按 `kind` 统计和弹出

## 4. 发送 packetize 与真正发包

### 4.1 `RoceTxScheduler`

port engine 内部用 `RoceTxScheduler`：

1. 维护 per-QP work queue
2. `Control > Data`
3. 从 work 中切出 `RoceTxSlice`

### 4.2 `BuildPacketForSlice_()`

真正的 packet 组装在 port engine 内完成：

1. `FlowTagHeader`
   - 只在 `DATA` 包里进入 packet payload
2. `RoceSimHeader`
3. `ThreePHeader`
4. `HopCountHeader`
5. `RdmaDispatchTag`

关键点：

- ns-3 `AddHeader(...)` 是后加的头先被接收侧 `RemoveHeader(...)` 读到，所以线上解析顺序是 `HopCountHeader -> ThreePHeader -> RoceSimHeader -> FlowTagHeader`
- `REQUEST/ACT` 没有 `FlowTagHeader`
- `RdmaDispatchTag` 是 `PacketTag`
- 它不进入线上字节流
- 所以不影响 MTU

### 4.3 端口准入

packet 在真的 `SendFrom(...)` 前，还要经过端口准入：

1. 分类 `qid`
2. 检查 pause
3. 检查 per-qid inflight packet 上限
4. 不满足就进入 pending queue
5. 满足才真正 `SendFrom(...)`

### 4.4 真正发出后

发出成功后：

1. 如果 `transport_ack_enable=true`，且该包不是 transport ACK
   - 记录 outstanding packet
   - arm retransmission timer
   - 如果是某个 work 的最后一个非重传 slice，则触发 send completion 并 retire 对应 `SqEntry`
2. 如果是 transport ACK
   - 不进入 outstanding
   - 不触发 transport completion
3. 通过 `TxNotifyCallback` 通知 `sendflowclient`
   - `sendflowclient` 会忽略 transport ACK 和重传包
   - 上层主要用最后一个非重传 `REQUEST` slice 收尾 `REQ_DONE`

### 4.5 `PhyTxEnd`

设备 `PhyTxEnd` 到来后：

1. 归还 per-qid inflight credit
2. 再次尝试 drain pending queue

## 5. 重传与 pause

当前的重传语义是：

1. timeout 到来
2. 找到最老 outstanding
3. 把它标成 pending retransmission
4. 放回端口待发队列
5. 只有当它真的再次 `SendFrom(...)` 发出去时
   - 才 `retry++`

这保证了：

- pause/PFC 期间不会因为 timer 空转而把 retry 次数瞬间烧光

## 6. 接收方向

### 6.1 `sendflowclient::HandleRead`

接收时先拆：

1. `HopCountHeader`
2. `ThreePHeader`
3. 尝试探测 `RoceSimHeader`

如果不是 RoCE 包：

- 直接走原来的 3P 业务路径

如果是 RoCE 包：

1. 先用 `RdmaIngressPortTag` 恢复真实 ingress device index
2. 找到这个 ingress 口上的 `RdmaPortEngine`
3. 调 `HandleIncomingPacket(...)`

当前已经没有旧的 `RoceTransportEngine::OnRxPacket(...)` fallback。

### 6.2 `RdmaPortEngine::HandleIncomingPacket`

这一步会做：

1. 如果是 ACK
   - 直接走 `HandleAck(...)`
   - retire outstanding
   - cancel/re-arm retrans timer
   - 清理 recovery target

2. 如果是普通包
   - 先检查 `RQ`
   - 再走 `RoceQpEngine::OnRxHeader(...)`
   - 做顺序检查/reorder
   - 必要时缓存到 reorder buffer
   - 需要时直接回发 ACK
   - 若可以上送，则返回 `readyPackets`

### 6.3 reorder

reorder 现在在 port engine 里：

1. future packet 先缓存到 `m_bufferedUpperPackets`
2. 当前 packet 顺利推进 `expectedRxPsn` 后
3. `CollectReadyBufferedPackets_()` 再把连续的 buffered packet 一起释放

### 6.4 recv completion

每个成功交付到 upper 的接收 packet：

1. 会产生 recv completion
2. 会释放对应的 `RqEntry`

## 7. 返回到 3P 业务层

`HandleIncomingPacket(...)` 返回的 `readyPackets` 再交给：

- `sendflowclient::ProcessThreePhasePayload_(...)`

这一步仍然只做业务层语义：

1. `REQUEST`
   - 触发 receiver 回调

2. `DATA`
   - 统计数据分片
   - 收齐后反向发 `ACT`

3. `ACT`
   - sender 完成闭环
   - 触发 `TX_DONE`

所以：

- transport ACK 不是 `ACT`
- `ACT` 仍然是 3P 的业务控制报文

## 8. 一个完整 send/recv 闭环

把整条链串起来，就是：

1. Astra-Sim 下发一个 flow
2. `sendflowclient` 把它转成 `REQUEST` 或 `DATA`
3. `RoceTransportEngine` 给它分 lane/QPN/workId
4. 找到真实 egress port 的 `RdmaPortEngine`
5. port engine 把它写进 `SQ`
6. scheduler 选出 slice
7. port engine packetize 并真正从物理口发出
8. 对端 ingress port engine 先接住 packet
9. 做 `RQ/reorder/ACK`
10. ready payload 回到 `sendflowclient`
11. `ProcessThreePhasePayload_()` 执行 3P 业务语义
12. sender 侧收到 `ACT` 后 `TX_DONE`

## 9. 当前最关键的边界

现在最重要的结构边界已经比较清楚了：

1. `sendflowclient`
   - 业务层

2. `FlowIo`
   - socket 绑定
   - legacy/fallback buffer
   - 本地软件 pause 适配

3. `RoceTransportEngine`
   - 逻辑层

4. `RdmaPortEngine`
   - packet runtime + port runtime

5. `bridge`
   - 选择 `outPort`
   - 学表 / flood / forwarding
   - 外侧终端 bridge 上的 learning-only drop

6. `PointToPointNetDevice`
   - 真正上链路

如果你想判断“这次解耦是不是已经成型”，最值得看的就是这一条边界链。
