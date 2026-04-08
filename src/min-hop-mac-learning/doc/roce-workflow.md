# RoCE 工作流说明

这份文档只讲一件事：在当前版本里，一条普通 P2P 流从 Astra-Sim 下发之后，最终是怎样经过 `sendflowclient`、`RoceTransportEngine`、`FlowIo / bridge`、`RdmaPortEngine`，再从物理 port 发出去的。

## 1. 当前主链条

当前链路可以概括成：

```text
Astra-Sim
  -> my_entry / my_AstraSimNetwork
  -> sendflowclient
  -> RoceTransportEngine
  -> packet + RdmaDispatchTag
  -> FlowIo
  -> external p2p OR bridge
  -> selected outPort's RdmaPortEngine
  -> PointToPointNetDevice::SendFrom
  -> network
  -> peer bridge/local ingress
  -> sendflowclient::HandleRead
  -> ingress RdmaPortEngine HandleAck (ACK only)
  -> RoceTransportEngine::OnRxPacket
  -> 3P business callback
```

这里最重要的变化是：

- 发送语义和接收语义仍然主要在 `RoceTransportEngine`
- 但“某个包到底能不能从某个物理口发出去”，现在已经开始由 `RdmaPortEngine` 决定

## 2. Astra-Sim 到 `sendflowclient`

### 2.1 Astra-Sim 下发发送请求

1. Astra-Sim 在 workload/stream 层决定发起一个 P2P 通信。
2. 发送侧调用 `sim_send`，接收侧调用 `sim_recv`。
3. 当前分支里，网络后端实现在 `scratch/my_AstraSimNetwork.cc`。
4. 发送请求最终通过 `my_entry::SendFlow(...)` 交给源节点上的 `sendflowclient`。

### 2.2 `sendflowclient` 仍然是最上层 facade

发送侧：

1. 登记 sender 完成回调
2. 保存 `sim_request`
3. 调 `TriggerSendTo_now(...)`

接收侧：

1. `sim_recv` 预先登记回调
2. 若数据已经在本地 `recvHash` 中准备好，就直接回调

## 3. `sendflowclient` 发送侧当前怎么工作

### 3.1 先决定这是 `REQUEST`、`DATA` 还是 `ACT`

当前 3P 语义还是由 `sendflowclient` 负责：

- `REQUEST`
- `DATA`
- `ACT`

RoCE 没有替换掉 3P，只是在外层增加了一套 transport 语义和端口执行路径。

### 3.2 `RoCE=false` 与 `RoCE=true`

当前发送路径已经分成两条：

1. `RoCE=false`
   - 仍然走传统 legacy 直发路径

2. `RoCE=true`
   - 先进入 `EnqueueRoceWork_(...)`
   - 再通过 `RoceTransportEngine` 和 `RoceTxScheduler` 生成待发 slice

### 3.3 `RoCE=true` 时，先过语义层

对每个待发 work：

1. `sendflowclient` 先组织 `RoceTxWorkItem`
2. 调 `RoceTransportEngine::Enqueue(...)`
3. `RoceTransportEngine` 根据 `ThreePHeader::State` 选 lane：
   - `REQUEST -> req_ctrl`
   - `DATA -> data`
   - `ACT -> act_ctrl`
4. `RoceTransportEngine` 按 `flow + lane` 分配或复用 `QPN`
5. `RoceTxScheduler` 把 work 切成 `RoceTxSlice`
6. `sendflowclient` 再取 `TryGetNextDispatchAction(...)` 出来的 slice

这一步的作用是：

- 先把“RoCE 语义上该怎么发”决定出来
- 但还没有决定“物理端口现在是否允许发”

### 3.4 组包顺序

当前实际组包顺序是：

1. `HopCountHeader`
2. `ThreePHeader`
3. `FlowTagHeader`
   - 主要在 DATA 上出现
4. `RoceSimHeader`
   - 通过 `RoceTransportEngine::BuildTxHeader(...)` 或 `BuildAckHeader(...)`

然后 `sendflowclient` 会给 packet 额外挂一个本地 `RdmaDispatchTag`。

这个 tag 里带：

1. `flowKey(tag/src/dst)`
2. `state`
3. `qpn`
4. `psn`
5. `opcode`
6. `txClass`
7. `sliceBytes / packetIndex / totalPackets`
8. `isLastSlice`
9. `isRetransmission`
10. `isTransportAck`

这一步非常关键：

- `RdmaDispatchTag` 是 `PacketTag`
- 它不会进入线上字节流
- 所以不会改变当前 MTU 分片逻辑

## 4. 从 `sendflowclient` 到物理 port

### 4.1 外部直发路径

如果当前发送模式是 `External`：

1. `sendflowclient` 通过 `FlowIo` 选择外部设备
2. `FlowIo` 检查 packet 是否带 `RdmaDispatchTag`
3. 如果带 tag，且目标 `PointToPointNetDevice` 上 aggregate 了 `RdmaPortEngine`
4. 则调用 `RdmaPortEngine::SubmitFromFlowIo(...)`
5. 否则回退到原来的 `sock->SendTo(...)`

### 4.2 内部经 bridge 的路径

如果当前发送模式是 `Internal`：

1. `sendflowclient` 仍然把包交给 `FlowIo`
2. `FlowIo` 把包送到本节点的 bridge device
3. `MinHopBridgeNetDevice` 仍然按原逻辑做：
   - 学表
   - 转发
   - 选真实 `outPort`
4. 当 bridge 知道具体 `outPort` 后：
   - 如果这是本地注入流量
   - 且 packet 带 `RdmaDispatchTag`
   - 且该 `outPort` 上有 `RdmaPortEngine`
   - 则调用 `RdmaPortEngine::SubmitFromBridgeEgress(...)`
5. 否则还是直接 `outPort->SendFrom(...)`

因此，当前实现保证了两件事：

1. bridge 仍然保留原来的选路职责
2. `QP/重传/端口 credit` 不需要在 bridge 之前就猜出最终 port

## 5. `RdmaPortEngine` 在发送时实际做什么

一旦 packet 进入具体 `outPort` 上的 `RdmaPortEngine`，当前会发生下面这些事：

1. 从 `RdmaDispatchTag` 判断这是不是 control packet
2. 根据目标地址分类 `qid`
3. 检查这个 `qid` 当前是否 paused
4. 检查这个 `qid` 的 inflight packet 数是否已经到上限
5. 如果当前不能发：
   - 把 packet 放进端口本地 pending queue
6. 如果当前能发：
   - 补写 `queueId` 到 `RdmaDispatchTag`
   - 真正调用 `PointToPointNetDevice::SendFrom(...)`
7. 发出成功后：
   - 记录端口侧 outstanding packet
   - 必要时 arm retransmission timer
   - 通过 `TxNotifyCallback` 通知 `sendflowclient`
8. 设备 `PhyTxEnd` 发生后：
   - 归还端口 inflight credit
   - 再尝试 drain pending queue

### 5.1 当前 retransmission 的新语义

当前版本最重要的变化之一是：

- timeout 不再等于“已经消耗了一次 retry”

更准确的语义变成了：

1. timeout 到来后，先标记“这个最老 outstanding 需要重传”
2. 把它放回端口 pending queue
3. 只有当它真的再次通过 `SendFrom(...)` 发出去时
4. 才 `retry++`

这样做的目的，是避免 packet 在 PFC/pause 期间尚未真正重新上链路，却在模拟器里已经把 retry 次数烧光。

## 6. 接收侧当前怎么工作

### 6.1 包先到 socket，再到 `sendflowclient::HandleRead`

接收时：

1. packet 先从设备/bridge 上送到应用 socket
2. `sendflowclient::HandleRead(...)` 收到 packet
3. 先拆：
   - `HopCountHeader`
   - `ThreePHeader`
4. 再探测是否存在 `RoceSimHeader`

### 6.2 bridge 现在会告诉应用：真实 ingress 口是谁

如果 packet 是经 bridge 本地上送应用的：

1. bridge 会在 copy 后的 packet 上加 `RdmaIngressPortTag`
2. 这个 tag 里只存一个 `deviceIndex`
3. `sendflowclient::HandleRead(...)` 用它恢复真实 ingress p2p 口

这样做的目的，是让 ACK 能回到正确的端口侧状态机。

### 6.3 ACK 当前怎样处理

如果收到的 RoCE packet 是 `Ack`：

1. `sendflowclient::HandleRead(...)` 先根据 `RdmaIngressPortTag` 找到 ingress `RdmaPortEngine`
2. 先调用 `RdmaPortEngine::HandleAck(...)`
   - retire 端口侧 outstanding
   - cancel/re-arm retransmission timer
   - 清理 recovery target
3. 然后再调用 `RoceTransportEngine::OnRxPacket(...)`
   - 让语义侧镜像状态也同步更新

所以当前 ACK 路径是“双层更新”：

1. 端口执行层先处理
2. 语义 transport 层再镜像处理

这也是当前版本仍然处于过渡态的一个重要标志。

### 6.4 非 ACK 包怎样处理

如果收到的不是 `Ack`：

1. `RoceTransportEngine::OnRxPacket(...)` 负责：
   - QP 合法性检查
   - RX reorder
   - `expectedRxPsn`
   - 是否需要发 transport ACK
   - 哪些 upper payload 已经 ready
2. 如果 `needAck=true`
   - `sendflowclient` 当前仍会构造 ACK packet
   - 给 ACK 也挂 `RdmaDispatchTag`
   - 再经 `FlowIo / bridge / RdmaPortEngine` 发出去
3. 如果有 ready payload
   - 才继续交给 `ProcessThreePhasePayload_(...)`

## 7. 3P 业务语义在接收侧怎样收敛

### 7.1 `REQUEST`

- 收到后按当前 3P 逻辑触发 receiver 回调

### 7.2 `DATA`

1. 每到一个 DATA 分片就增加计数
2. 收齐后：
   - 反向发送 ACT
   - 写入 `recvHash`
   - 触发 receiver 回调

### 7.3 `ACT`

1. sender 收到 ACT
2. 业务层触发最初登记的发送完成回调
3. 打出 `TX_DONE`

这说明当前 `ACT` 仍然只是业务控制报文，不是 transport ACK。

## 8. 当前工作流里最值得注意的“边界点”

### 8.1 当前有两套“谁来控制发送”的逻辑

1. `RoceTransportEngine`
   - 决定语义上“下一片该发什么”
2. `RdmaPortEngine`
   - 决定物理上“这一片现在能不能从这个 port 发出去”

这两个层次现在还没有完全合并。

### 8.2 当前有两套 reliability state 视角

1. `RoceTransportEngine`
   - 保留镜像 outstanding / reorder / recovery 视角
2. `RdmaPortEngine`
   - 开始持有真实端口侧 outstanding / ack retire / retrans timer

这就是当前实现最需要继续收敛的地方。

### 8.3 当前本地元数据不会影响 MTU

这一点已经是当前实现的重要前提：

- `RdmaDispatchTag`
- `RdmaIngressPortTag`

都是 `PacketTag`，不是线上 header。

因此在当前版本里：

- bridge/port 需要的本地控制信息已经能随 packet 走
- 同时不会改变当前 `RoCESimHeader + 3P header` 的线上长度口径

## 9. 用一句话概括当前 workflow

当前版本里，一条流的真实流程是：

- `sendflowclient` 决定业务语义，
- `RoceTransportEngine` 决定 RoCE 语义和 slice，
- `RdmaPortEngine` 决定这个 slice 何时能从某个物理 port 真正发出去，
- 接收时再由 bridge 把真实 ingress port 信息带回应用层，使 ACK 能回到正确的端口状态机。
