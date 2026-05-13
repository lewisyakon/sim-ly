# RoCE 类结构与调用关系说明

最近修改：2026-04-23

这份文档回答 5 个问题：

1. 现在有哪些 RoCE 相关类。
2. 每个类到底负责什么。
3. 它们之间怎样耦合、怎样传参。
4. 从外部怎样调用这套实现。
5. 当前整体已经能做到什么，哪些地方仍然是简化模型。

## 1. 当前总体结构

当前代码可以看成 4 层：

1. Astra-Sim 接入层
   - `scratch/my_entry.h`
   - `scratch/my_AstraSimNetwork.cc`
   - 负责把 `sim_send/sim_recv` 接到 `sendflowclient`

2. 应用 facade 与 3P 业务层
   - `sendflowclient`
   - 负责 Astra-Sim 回调、`REQUEST/DATA/ACT` 业务语义、`REQ_DONE/RX_DONE/TX_DONE`

3. RoCE 逻辑层
   - `RoceTransportEngine`
   - 负责 `flow/lane -> qpn`、work 准备、3P FSM

4. RoCE 端口运行时
   - `RdmaPortEngine`
   - aggregate 在每个 `PointToPointNetDevice` 上
   - 负责端口绑定后的 `QP/SQ/RQ/CQ`、packetize、ACK、重传、reorder、pause-aware 调度

一句话概括：

- `sendflowclient` 说“我要发什么业务动作”
- `RoceTransportEngine` 说“这个业务动作应该落到哪个 lane/QPN、是什么 WR/work”
- `RdmaPortEngine` 说“这个 work 在这个物理 port 上怎样以 QP 的形式真正跑起来”

## 2. 当前对象图

```text
Astra-Sim
  -> my_entry / my_AstraSimNetwork
  -> sendflowclient
       -> FlowIo
            -> bind innerBridge socket
            -> optionally bind externalBridge socket
       -> RoceTransportEngine
            -> RoceThreePhaseFsm
       -> innerBridge (m_bridgeDevice)
            -> ResolveLocalInjectionEgressPort(...)
       -> or externalBridge (when m_outDeviceIndex is a bridge)
            -> ResolveLocalInjectionEgressPort(...)
                 -> selected outPort (PointToPointNetDevice)
                      -> RdmaPortEngine
                           -> RoceTxScheduler
                           -> RoceQpEngine
                           -> QpRuntime(SQ/RQ/CQ)
                      -> PointToPointChannel
```

接收方向：

```text
wire packet
  -> PointToPointNetDevice / bridge ingress
  -> sendflowclient::HandleRead
  -> ingress RdmaPortEngine::HandleIncomingPacket
  -> ready upper payload
  -> sendflowclient::ProcessThreePhasePayload_
```

这里最关键的边界是：

- `sendflowclient` 不再保留旧的 RoCE RX/TX fallback 主路径
- `RdmaPortEngine` 已经是唯一的 RoCE packet runtime

## 3. 各个类分别负责什么

### 3.1 `sendflowclient`

文件：

- [my_sendflowclient.h](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/my_sendflowclient.h)
- [my_sendflowclient.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/my_sendflowclient.cc)

它现在负责：

1. 对外接口
   - `TriggerSendTo_now(...)`
   - `set_sendfunc(...)`
   - `set_recvfunc(...)`
   - `write_flowtag(...)`
   - `SetRoceConfig(...)`

2. Astra-Sim 桥接
   - 维护 sender/receiver 回调表
   - 保存 `sim_request`

3. 3P 业务语义
   - `REQUEST`
   - `DATA`
   - `ACT`
   - 以及 `REQ_DONE / RX_DONE / TX_DONE`

4. RoCE 逻辑入口
   - 组装 `RoceTxWorkItem`
   - 调 `RoceTransportEngine::PrepareTxWork(...)`
   - 解析 bridge/port，找到具体 `RdmaPortEngine`
   - 调 `RdmaPortEngine::PostSend(...)`

5. RoCE 接收上层入口
   - 在 `HandleRead(...)` 中拆外层头
   - 把 RoCE 包交给 ingress `RdmaPortEngine::HandleIncomingPacket(...)`
   - 把 ready payload 回送到 `ProcessThreePhasePayload_(...)`

6. 发送完成桥接
   - `HandleRdmaPortPacketSent_(...)`
   - 只负责在 packet 真正被 port 接受发送后，收尾 `REQ_DONE` 等上层语义

它现在不再负责：

1. 旧的 RoCE 调度事件链
2. 旧的 ACK 构包与 fallback ACK 路径
3. 旧的 `OnRxPacket(...)` fallback
4. `QPN/PSN/ACK/retrans/reorder` 的 packet 级 runtime

### 3.2 `RoceTransportEngine`

文件：

- [roce-transport-engine.h](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/roce-transport-engine.h)
- [roce-transport-engine.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/roce-transport-engine.cc)

它现在是一个明显收缩后的逻辑层。

它负责：

1. `flow + lane -> qpn`
   - `REQUEST -> req_ctrl`
   - `DATA -> data`
   - `ACT -> act_ctrl`

2. `RoceTxWorkItem` 的逻辑准备
   - 分配 `qpn`
   - 分配 `workId`
   - 决定 `opcode`
   - 决定 `userPhase`
   - 决定 `txClass`

3. 3P FSM
   - `OnRequestIssued(...)`
   - `OnDataIssued(...)`
   - `OnActReceived(...)`

它不再负责：

1. ACK 生成
2. packet header 生成
3. 发送调度主循环
4. outstanding/retrans timer/recovery
5. RX reorder 主逻辑

也就是说，`RoceTransportEngine` 现在不再是 packet runtime，而是“逻辑 QP 和业务阶段语义”的协调器。

### 3.3 `RdmaPortEngine`

文件：

- [rdma-port-engine.h](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/rdma-port-engine.h)
- [rdma-port-engine.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/rdma-port-engine.cc)

这是当前实现的核心。

它负责：

1. 绑定具体 `PointToPointNetDevice`
   - `AttachToDevice(...)`
   - 监听 `PhyTxEnd`
   - 监听 `QueuePauseState`

2. 发送入口
   - `PostSend(...)`
   - 将 `RoceTxWorkItem` 写入 port-bound `QP runtime`

3. 端口级调度和准入
   - `qid` 分类
   - pause-aware admission
   - per-qid inflight packet 限制
   - pending queue

4. packetize 和真正发包
   - `RoceTxScheduler`
   - `BuildPacketForSlice_(...)`
   - `PointToPointNetDevice::SendFrom(...)`

5. 发送侧可靠性
   - outstanding packet
   - retransmission timer
   - pending retransmission
   - recovery target
   - “真正重发时才 `retry++`”

6. 接收侧 runtime
   - `HandleIncomingPacket(...)`
   - ACK 处理
   - RQ 资源检查
   - reorder buffer
   - ACK 回发

7. completion
   - `GetCompletionCount(...)`
   - `PopCompletion(...)`
   - 维护 send/recv completion

### 3.4 `RdmaPortEngine::QpRuntime`

不是独立文件，而是 `RdmaPortEngine` 内部结构。

它现在代表“绑定到某个物理 port 上的一个 QP 运行时”。

它包含：

1. `sqDepth`
2. `rqDepth`
3. `cqDepth`
4. `dest`
5. `txClass`
6. `sqEntries`
7. `rqInflight`
8. `completions`
9. `sendCompletionCount`
10. `recvCompletionCount`

当前这套 `QP` 不是为了完全复刻 verbs 全部接口，而是为了把：

- `SQ`
- `RQ`
- `CQ`

这三类资源的所有权明确落到 port 侧。

### 3.5 `SqEntry`

`SqEntry` 是 `SQ` 的最小单位。

它代表的是一个业务层 post 到这个 QP 的发送 work，而不是一个 MTU 分片。

当前存的核心字段有：

1. `workId`
2. `state`
3. `flowKey`
4. `totalBytes`
5. `totalPackets`
6. `txClass`

packetization 发生在它之后，而不是在它之前。

### 3.6 `RqEntry`

`RqEntry` 是接收侧在这个 QP 上占用的 receive slot。

当前是最小模型：

1. `psn`
2. `wrId`

它的作用是：

- 在 packet 进入 reorder/upper delivery 之前，先占住一个 RQ 资源
- packet 真正完成交付或释放时，再归还这个 slot

### 3.7 `CQ`

当前 `CQ` 还不是一个独立类，而是 `QpRuntime::completions`。

但语义已经明确下来：

1. 支持 `send completion`
2. 支持 `recv completion`
3. `RoceCompletion` 增加了 `kind`
   - `Send`
   - `Recv`
4. 可按 `kind` 统计/弹出 completion

当前这就是“最小但真实存在的 CQ”。

### 3.8 `RoceTxScheduler`

文件：

- [roce-tx-scheduler.h](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/roce-tx-scheduler.h)
- [roce-tx-scheduler.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/roce-tx-scheduler.cc)

它现在已经不在 `sendflowclient` 里用，而是完全由 `RdmaPortEngine` 使用。

它负责：

1. 维护 per-QP work queue
2. `Control > Data`
3. 从 `RoceTxWorkItem` 切出 `RoceTxSlice`

它不负责：

1. 设备 pause
2. inflight credit
3. retrans timer
4. ACK retire

### 3.9 `RoceQpEngine`

文件：

- [roce-qp-engine.h](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/roce-qp-engine.h)
- [roce-qp-engine.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/roce-qp-engine.cc)

它是 QP 协议辅助器。

它负责：

1. QP state machine
   - `Reset / Init / Rtr / Rts / Err`

2. PSN
   - `BuildSendHeader(...)`

3. RX 有序性
   - `expectedRxPsn`
   - `pendingRxPsns`
   - duplicate/out-of-order/overflow

它现在被 `RdmaPortEngine` 组合使用，而不再被 `sendflowclient` 直接驱动。

### 3.10 `RoceThreePhaseFsm`

它是最轻的一层，只记录 3P 业务阶段。

当前用途主要有两个：

1. `DATA` 该用什么 opcode
2. `ACT` 到达后逻辑流的完成态

### 3.11 `RdmaDispatchTag`

这是本地 `PacketTag`。

它现在只承担本地 packet runtime metadata，不承担核心状态源头。

它里边主要有：

1. `workId`
2. `flowKey`
3. `state`
4. `qpn`
5. `psn`
6. `opcode`
7. `txClass`
8. `totalBytes`
9. `sliceBytes`
10. `packetIndex`
11. `totalPackets`
12. `queueId`
13. `isLastSlice`
14. `isRetransmission`
15. `isTransportAck`

关键点：

- 它是 `PacketTag`
- 不影响线上长度
- 不影响 MTU 分片

### 3.12 `RdmaIngressPortTag`

这是 bridge 本地上送应用时写的 `PacketTag`。

它只有一个作用：

- 告诉 `sendflowclient` 这次包是从哪个物理 ingress p2p 口进来的

这样接收时才能把 RoCE 包交给正确的 ingress `RdmaPortEngine`。

### 3.13 `FlowIo`

`FlowIo` 现在主要是 socket/device 桥，加上一层本地软件 pause/buffer 适配。

RoCE 打开时，它的作用是：

1. `BridgeAndExternal` 模式下，同时绑定 `innerBridge` 和 `externalBridge`
2. 非 RoCE legacy 路径保持原样
3. 本地注入路径上保留 software buffer / retry
4. 如果目标设备本身就是带 `RdmaPortEngine` 的物理口，先尝试直接提交到 port engine

RoCE 主路径现在已经不再依赖 `FlowIo` 去实现 transport runtime；它只是 I/O 入口和 fallback/local-pause 适配层。

### 3.14 `MinHopBridgeNetDevice`

bridge 仍然负责：

1. 学表
2. 在真正 forwarding bridge 上做转发
3. ECMP / flood
4. 选择真实 `outPort`
5. 在外侧终端 bridge 上执行“学习后丢弃 transit 包”
6. 用每 ingress port 一个 aggregate queue 模拟 bridge ingress 有限缓冲和 `switch_delay`
7. 根据 aggregate ingress queue / egress queue 水位传播 PFC pause/resume

它和 RoCE 的耦合现在主要只有两点：

1. 发送时提供 egress 解析
2. 接收时写入 `RdmaIngressPortTag`

bridge 不维护 QP，不维护 ACK，不维护重传。`TerminalLearningOnly` 也只应该给外侧 bridge 用，而不是给所有 bridge 统一开启。

当前 bridge ingress 不是一个按 qid 分裂的 `PppRoundRobinQueue`。它只做 aggregate 缓冲：

1. 容量取对应物理 egress queue 的 `NumQueues * MaxPacketsPerQueue`。
2. 包入队后生成独立 token。
3. 每个 token 按自己的 `switch_delay` drain。
4. qid 优先级、ACK 优先级和 per-qid pause-aware admission 由物理 egress queue / `RdmaPortEngine` 承担。

旧的 `NotHeadRetryDelay` 和 `BridgeDiagEnable` 属性仍保留为配置兼容项，但当前不再影响 bridge ingress 调度，也不输出旧 residence 诊断。

## 4. 当前类之间怎样耦合

### 4.1 发送方向

发送方向的耦合链是：

```text
sendflowclient
  -> RoceTransportEngine::PrepareTxWork
  -> ResolveRoceTxPortEngine_
  -> RdmaPortEngine::PostSend
  -> RoceTxScheduler
  -> RoceQpEngine::BuildSendHeader
  -> PointToPointNetDevice::SendFrom
```

参数如何传：

1. `sendflowclient` 组织 `RoceTxWorkItem`
2. `RoceTransportEngine` 写入：
   - `qpn`
   - `workId`
   - `opcode`
   - `userPhase`
   - `txClass`
3. `RdmaPortEngine` 把它写进 `QpRuntime::sqEntries`
4. `RoceTxScheduler` 再切成 `RoceTxSlice`
5. `BuildPacketForSlice_()` 才真正生成 packet 和 `RoceSimHeader`

当前还有一个端点布局相关的关键点：

1. `Internal / 普通 Loopback` 通过 `m_bridgeDevice`，也就是 `innerBridge`，解析 `outPort`
2. `External` 优先把 `m_outDeviceIndex` 当成物理口
3. 如果 `m_outDeviceIndex` 实际上是 `externalBridge`，再由 `externalBridge` 解析真实 `outPort`

### 4.2 接收方向

接收方向的耦合链是：

```text
sendflowclient::HandleRead
  -> ResolveRoceIngressDeviceIndex_
  -> ingress RdmaPortEngine::HandleIncomingPacket
  -> readyPackets
  -> sendflowclient::ProcessThreePhasePayload_
```

这里已经没有旧的 transport fallback。

### 4.3 ACK 与重传

现在 ACK 与重传都在 `RdmaPortEngine`：

1. ACK RX
   - `HandleAck(...)`

2. ACK TX
   - `MaybeSendAck_(...)`
   - `BuildAckPacket_(...)`

3. retransmission
   - `HandleRetransTimeout_(...)`
   - `ArmRetransTimer_(...)`
   - `CancelRetransTimer_(...)`

4. retry 计数
   - 只在真正再次发出时增加

### 4.4 completion

completion 现在也是 port 侧：

1. send completion
   - `CompleteSqEntry_(...)`

2. recv completion
   - `PushCompletion_(..., Recv)`

3. 对外接口
   - `GetCompletionCount(...)`
   - `PopCompletion(...)`

## 5. 从外部怎样调用

从 Astra-Sim 看，外部还是只看见 `sendflowclient`：

1. `sim_send`
   - 最终到 `TriggerSendTo_now(...)`

2. `sim_recv`
   - 最终到 `set_recvfunc(...)`

3. RoCE 配置
   - `SetRoceConfig(...)`

从调用方视角，并不需要直接知道：

- `RoceTransportEngine`
- `RdmaPortEngine`
- `RoceQpEngine`
- `RoceTxScheduler`

这些类的存在。

## 6. 当前整体已经支持什么

当前版本已经支持：

1. `REQUEST / DATA / ACT` 三 lane
2. `flow + lane -> qpn`
3. per-port `QP runtime`
4. `SQ / RQ / CQ` 最小模型
5. send-side packetization
6. receive-side reorder
7. transport ACK
8. ACK/retrans/recovery
9. pause-aware 发送准入
10. per-qid inflight packet 限制
11. send/recv completion 记录
12. `ROCE=true` workload 闭环

## 7. 当前仍然是简化模型的地方

虽然主结构已经到位，但还有几处是“简化实现”，不是完整 verbs 级建模：

1. `CQ` 还不是独立对象类，而是 `QpRuntime` 内部 completion 队列
2. completion 目前还没有成为上层主驱动语义，`TX_DONE` 仍然由 3P 的 `ACT` 完成闭环
3. `RQ` 是最小 receive slot 模型，还没有做 `SRQ`
4. 当前每个具体 work 仍只会落到一个物理 egress port，不做单 work 多口并发
5. 拓扑侧已经落了 `innerBridge/externalBridge` 布局和 external rail 清单，但 transport side 的多 rail striping / per-logical-QP 多端口绑定还没有展开

## 8. 最值得你 review 的地方

如果你现在要审代码，我建议优先看这几条：

1. `sendflowclient` 是否真的已经退回 facade
2. `RoceTransportEngine` 是否真的只剩逻辑层
3. `RdmaPortEngine` 是否已经成为唯一 packet runtime
4. `SQ/RQ/CQ` 的所有权是否已经清楚
5. `RdmaDispatchTag` 是否只剩 metadata 作用，而不是状态源头

如果这几条你看下来都成立，那么这轮解耦就算真正站住了。
