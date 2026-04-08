# RoCE 类结构与调用关系说明

这份文档专门回答下面几个问题：

1. 当前 RoCE 相关类各自负责什么。
2. 它们之间怎么耦合、怎么传参。
3. 当前整体从外部是怎样调用的。
4. 这套实现当前已经能做什么，还不能做什么。

这份文档讲的是“当前代码结构”，不是目标设计。若你想看一条流具体怎样从上层走到物理 port，请同时看：

- [roce-workflow.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-workflow.md)

## 1. 当前总体分层

当前代码已经不是单核心结构，而是 5 层协作：

1. Astra-Sim 接入层
   - `scratch/my_common.h`
   - `scratch/my_entry.h`
   - `scratch/my_AstraSimNetwork.cc`
   - 负责解析配置、创建应用、把 `sim_send/sim_recv` 桥接到 `sendflowclient`

2. 应用 facade 层
   - `my_sendflowclient`
   - 负责对外接口、Astra-Sim 回调、3P 业务语义、包头拼装和 I/O 桥接

3. 语义 transport 层
   - `RoceTransportEngine`
   - 负责 lane/QPN/QP/PSN、发送调度器、header 生成、RX reorder 和部分 transport 镜像状态

4. 端口执行层
   - `RdmaPortEngine`
   - 负责绑定具体 `PointToPointNetDevice`
   - 承接 pause-aware 发送准入、端口侧 outstanding、ACK retire、retransmission timer、pending queue

5. 设备与转发层
   - `FlowIo`
   - `MinHopBridgeNetDevice`
   - `PointToPointNetDevice`
   - 负责 packet socket、bridge 选路、物理 port 发包、pause 状态与 `PhyTxEnd` trace

一句话概括当前真实结构：

- `sendflowclient` 负责“对外业务接口”
- `RoceTransportEngine` 负责“RoCE 语义与调度”
- `RdmaPortEngine` 负责“端口相关执行与重传”
- `FlowIo / bridge / p2p` 负责“把包真正送到正确的物理口”

## 2. 当前关键对象图

```text
Astra-Sim
  -> my_entry / my_AstraSimNetwork
  -> sendflowclient
       -> RoceTransportEngine
            -> RoceTxScheduler
            -> RoceQpEngine
            -> RoceThreePhaseFsm
       -> FlowIo
            -> external p2p device
            -> or MinHopBridgeNetDevice
                 -> selected outPort (PointToPointNetDevice)
                      -> RdmaPortEngine (aggregated on that port)
                      -> PointToPointChannel
```

接收方向上还多一条本地元数据回传链：

```text
PointToPointNetDevice / bridge ingress
  -> bridge local delivery writes RdmaIngressPortTag
  -> sendflowclient::HandleRead
  -> route ACK retire to the correct ingress RdmaPortEngine
  -> call RoceTransportEngine::OnRxPacket for RoCE semantic RX handling
```

## 3. 每个类当前负责什么

### 3.1 `sendflowclient`

文件：

- [my_sendflowclient.h](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/my_sendflowclient.h)
- [my_sendflowclient.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/my_sendflowclient.cc)

定位：

- 这是当前系统真正对外可见的应用类。
- Astra-Sim、`my_entry`、`my_common` 都是通过它来注册回调、发包、收包。

它当前主要负责：

1. 对外接口
   - `SetRoceConfig(...)`
   - `TriggerSendTo_now(...)`
   - `set_sendfunc(...)`
   - `set_recvfunc(...)`
   - `write_flowtag(...)`
   - `initialization(...)`

2. Astra-Sim 桥接
   - 维护 sender 完成回调表 `m_sendfunctable`
   - 维护 receiver 回调表 `m_recvfunctable`

3. 3P 业务语义
   - 区分 `REQUEST / DATA / ACT`
   - 维护 `m_request / m_recvHash / m_dataPacketsExpected / m_dataPacketsReceived`
   - 触发 `REQ_DONE / RX_DONE / TX_DONE`

4. 当前 RoCE 发包外壳
   - 组织 `RoceTxWorkItem`
   - 调 `RoceTransportEngine::Enqueue(...)`
   - 取 `RoceTransportEngine::TryGetNextDispatchAction(...)`
   - 组装 `ThreePHeader / FlowTagHeader / RoceSimHeader`
   - 给 packet 挂 `RdmaDispatchTag`

5. 当前 RoCE 收包外壳
   - 从 socket 收包
   - 拆 `HopCountHeader / ThreePHeader / RoceSimHeader`
   - 从 `RdmaIngressPortTag` 恢复真实 ingress device index
   - 若收到 transport ACK，先把 ACK 交给对应 `RdmaPortEngine::HandleAck(...)`
   - 再把包交给 `RoceTransportEngine::OnRxPacket(...)`
   - 把 ready payload 回送给 3P 业务层

6. 端口执行层初始化
   - `InitializeRdmaPortEngines_()`
   - 在本节点每个 `PointToPointNetDevice` 上 aggregate 一个 `RdmaPortEngine`
   - 用 `SetTxNotifyCallback(...)` 收到“这个 packet 已经真正被该端口接受发送”的通知

因此，`sendflowclient` 现在既不是完整 transport core，也不是纯业务层，而是一个 facade：

- 向上说 Astra-Sim 的语言
- 向下说 RoCE transport 和 ns-3 I/O 的语言

### 3.2 `RoceTransportEngine`

文件：

- [roce-transport-engine.h](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/roce-transport-engine.h)
- [roce-transport-engine.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/roce-transport-engine.cc)

定位：

- 它仍然是当前 RoCE 语义层的中心类。
- 但从本版本开始，它已经不再是唯一 transport core。
- 一部分 runtime 职责已经迁到 `RdmaPortEngine`。

它当前还负责：

1. lane 语义
   - `REQUEST -> req_ctrl`
   - `DATA -> data`
   - `ACT -> act_ctrl`

2. `flow + lane -> QPN` 映射
   - `m_flowToQpn`

3. `QP/QPN/PSN` 语义
   - 通过 `RoceQpEngine`
   - 通过 `BuildTxHeader(...)`
   - 通过 `BuildAckHeader(...)`

4. 发送调度
   - 通过 `RoceTxScheduler`
   - 仍然负责 `Enqueue(...)`
   - 仍然负责 `TryGetNextDispatchAction(...)`

5. 接收语义
   - `OnRxPacket(...)`
   - QP 合法性检查
   - RX reorder
   - `bufferedUpperPackets`
   - 是否要发 ACK
   - 是否把 payload 上送业务层

6. 一部分镜像态 TX runtime
   - `m_outstandingPackets`
   - `m_recoveryTargetPsns`
   - 这些状态还在，用于当前 scheduler/window 语义和日志观测

它当前已经不再实际承担：

1. 真实 ACK retire 主体
2. 真实 retransmission timer 主体
3. 真实 pending retransmission 重发执行

这一点可以直接从代码看出来：

- `HasPendingRetransmissions()` 现在固定返回 `false`
- `OnPacketWireTx(...)` 当前是空实现
- `OnRetransmissionSent(...)` 当前也是空实现

所以对它更准确的描述是：

- 它仍然是“RoCE 语义协调器”
- 但“端口侧可靠性执行器”已经开始变成 `RdmaPortEngine`

### 3.3 `RoceTxScheduler`

文件：

- [roce-tx-scheduler.h](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/roce-tx-scheduler.h)
- [roce-tx-scheduler.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/roce-tx-scheduler.cc)

定位：

- 第一阶段的发送调度器。
- 它只管“从 work queue 里挑下一片 slice”，不管真正的端口执行。

它负责：

1. 维护 per-QP work queue
2. 维护 `RoceTxClass::Control` 与 `RoceTxClass::Data`
3. 控制类优先
4. 数据类按活跃 QP 轮询
5. 把 `RoceTxWorkItem` 切成多个 `RoceTxSlice`

它不负责：

1. 真实端口 pause
2. `qid` 准入
3. `PhyTxEnd` 释放 credit
4. retransmission timer
5. ACK retire

所以它更像“语义调度器”，不是“物理 NIC scheduler”。

### 3.4 `RoceQpEngine`

文件：

- [roce-qp-engine.h](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/roce-qp-engine.h)
- [roce-qp-engine.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/roce-qp-engine.cc)

定位：

- 当前轻量 RoCE QP 引擎。
- 负责 QP 状态机、PSN、RX 有序性和 header 级语义。

它负责：

1. 创建和维护 `RoceQpContext`
2. `Reset / Init / Rtr / Rts / Err`
3. `BuildSendHeader(...)`
4. `OnRxHeader(...)`
5. `expectedRxPsn / pendingRxPsns / duplicate / out-of-order / overflow`

它不负责：

1. 选择哪个 port 发
2. 端口 pause
3. 设备级 pending queue
4. ACK timer
5. `FlowIo / bridge`

### 3.5 `RoceThreePhaseFsm`

文件：

- [roce-three-phase-fsm.h](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/roce-three-phase-fsm.h)
- `roce-three-phase-fsm.cc`

定位：

- 很轻的 3P 阶段状态记录器。

它主要负责：

1. `OnRequestIssued(...)`
2. `OnDataIssued(...)`
3. `OnActReceived(...)`
4. `GetState(...)`
5. `IsCompleted(...)`
6. `DataOpcode()`

它不负责可靠传输和端口调度。

### 3.6 `RdmaDispatchTag`

文件：

- [rdma-dispatch-tag.h](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/rdma-dispatch-tag.h)
- [rdma-dispatch-tag.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/rdma-dispatch-tag.cc)

定位：

- 一个本地 `PacketTag`。
- 用来把 RoCE 的本地调度元数据跟着 packet 一起穿过 `FlowIo / bridge / outPort`。

它当前承载的关键信息包括：

1. `workId`
2. `flowKey(tag/src/dst)`
3. `state`
4. `qpn`
5. `psn`
6. `opcode`
7. `txClass`
8. `totalBytes / sliceBytes / packetIndex / totalPackets`
9. `queueId`
10. `isLastSlice`
11. `isRetransmission`
12. `isTransportAck`

这个 tag 的两个关键性质：

1. 它是 `PacketTag`，不是线上 header。
2. 它不进入 packet byte buffer，所以不影响 MTU 和线上长度。

### 3.7 `RdmaIngressPortTag`

文件：

- [rdma-ingress-port-tag.h](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/rdma-ingress-port-tag.h)
- [rdma-ingress-port-tag.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/rdma-ingress-port-tag.cc)

定位：

- 另一个本地 `PacketTag`。
- 只用于“桥接后本地上送应用”的路径。

它当前只做一件事：

- 告诉 `sendflowclient`：这次收到的包实际上是从哪个物理 ingress p2p device 进来的。

这个信息的主要用途是：

- 把 transport ACK retire 路由到正确的 `RdmaPortEngine`

### 3.8 `RdmaPortEngine`

文件：

- [rdma-port-engine.h](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/rdma-port-engine.h)
- [rdma-port-engine.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/rdma-port-engine.cc)

定位：

- 当前版本最重要的新对象。
- 它 aggregate 在每个 `PointToPointNetDevice` 上。
- 它是“端口执行层”，不是应用层。

它当前对外暴露的核心接口是：

1. 生命周期
   - `AttachToDevice(...)`
   - `Configure(...)`
   - `Reset()`
   - `SetTxNotifyCallback(...)`

2. 发送入口
   - `SubmitFromFlowIo(...)`
   - `SubmitFromBridgeEgress(...)`

3. 接收 ACK 入口
   - `HandleAck(...)`

它当前已经负责：

1. 绑定具体 p2p 设备
2. 监听设备 trace
   - `PhyTxEnd`
   - `QueuePauseState`
3. 按目标地址分类 `qid`
4. 按 `qid` 检查 pause 状态
5. 维护 per-qid pending queue
   - `m_controlPending`
   - `m_dataPending`
6. 维护 per-qid inflight packet 数
   - `m_inflightPackets`
7. 维护端口侧 outstanding packet 集合
   - `m_outstandingPackets`
8. 维护端口侧 retrans timer
   - `m_retransTimers`
9. 维护端口侧 pending retransmission 与 recovery target
   - `m_pendingRetransmissions`
   - `m_recoveryTargetPsns`
10. 保证“真正重发时才 retry++”
11. `PhyTxEnd` 后归还端口 inflight credit

它当前还没有做到：

1. 真正的 per-QP `SQ/RQ/CQ`
2. 让 QP runtime 全部落到端口侧
3. 完整接管 RX reorder
4. 让 ACK 报文本身完全由端口侧直接构造

因此，`RdmaPortEngine` 当前不是完整 NIC 模型，而是“端口相关逻辑的第一阶段承接者”。

### 3.9 `FlowIo`

文件：

- [flow-io.h](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/flow-io.h)
- [flow-io.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/flow-io.cc)

定位：

- 仍然是 app 层和 ns-3 packet socket/device 之间的桥。

它当前与 RoCE 的耦合点只有一个：

- 若 packet 带 `RdmaDispatchTag`
- 且目标 device 上有 `RdmaPortEngine`
- 则优先把包交给 `RdmaPortEngine::SubmitFromFlowIo(...)`
- 否则仍走原来的 `sock->SendTo(...)`

所以 `FlowIo` 仍然不是 transport core，只是“进入端口执行层的入口之一”。

### 3.10 `MinHopBridgeNetDevice`

文件：

- [min-hop-bridge-net-device.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/min-hop-bridge-net-device.cc)

定位：

- 仍然负责桥接、学表、转发、选真实 egress port。

它当前和 RoCE 的两个耦合点是：

1. 对本地注入的包：
   - 若 packet 带 `RdmaDispatchTag`
   - 且最终选中的 `outPort` 上有 `RdmaPortEngine`
   - 则把 packet 交给 `RdmaPortEngine::SubmitFromBridgeEgress(...)`

2. 对本地上送应用的包：
   - 在发给 `m_rxCallback` 前，给 copy 后的 packet 加 `RdmaIngressPortTag`

它不负责：

1. QP 状态机
2. ACK/retry
3. MTU 分片
4. reorder

bridge 的职责没有变：

- 还是 bridge 自己选 outPort
- `RdmaPortEngine` 只是在“outPort 已知之后”接手端口执行

## 4. 当前主要耦合关系

### 4.1 `sendflowclient -> RoceTransportEngine`

这条链负责：

1. `RoceTxWorkItem` 入队
2. `RoceTxSlice` 出队
3. `RoceSimHeader` 生成
4. `OnRxPacket(...)` 语义处理

这是一条“语义 transport 链”。

### 4.2 `sendflowclient -> RdmaPortEngine`

这条链负责：

1. 初始化本节点所有端口上的 `RdmaPortEngine`
2. 接收 `TxNotifyCallback`
3. 在 ACK 收包时，根据 `RdmaIngressPortTag` 找到正确的 ingress port engine

这是一条“应用 facade 到端口执行层”的桥接链。

### 4.3 `FlowIo / bridge -> RdmaPortEngine`

这条链负责：

1. 把 packet 真正提交给具体端口
2. 把端口 pause/credit 约束纳入发送执行路径

这条链的关键点是：

- bridge 仍然负责选路
- `RdmaPortEngine` 只在具体 `outPort` 已知后工作

### 4.4 `RdmaPortEngine -> PointToPointNetDevice`

这条链负责：

1. `SendFrom(...)`
2. `PhyTxEnd`
3. `QueuePauseState`

这是当前所有端口执行语义最终落到设备上的地方。

## 5. 当前参数是怎样在类之间传递的

当前实现最重要的几类参数/对象如下：

1. `RoceTxWorkItem`
   - `sendflowclient` 生成
   - 交给 `RoceTransportEngine::Enqueue(...)`

2. `RoceTxSlice`
   - `RoceTxScheduler` 产出
   - `sendflowclient` 用它建包

3. `RoceSimHeader`
   - `RoceTransportEngine::BuildTxHeader(...)`
   - `RoceTransportEngine::BuildAckHeader(...)`
   - 是线上可见的 RoCE 仿真头

4. `RdmaDispatchTag`
   - `sendflowclient` 挂到 packet 上
   - `FlowIo / bridge / RdmaPortEngine / sendflowclient::HandleRdmaPortPacketSent_()` 都会用
   - 是本地侧带元数据，不上链路

5. `RdmaIngressPortTag`
   - bridge 本地上送时写入
   - `sendflowclient::HandleRead(...)` 消费

6. `RoceQpKey`
   - 在 `RoceTransportEngine` 和 `RdmaPortEngine` 两边都出现
   - 当前意味着“语义 QP”和“端口侧可靠性状态”还没有彻底统一

## 6. 外部是怎样调用这套结构的

### 6.1 从 Astra-Sim 看

当前对外仍然只暴露 `sendflowclient` 这一层：

1. `sim_send`
   - 最终到 `TriggerSendTo_now(...)`
2. `sim_recv`
   - 最终到 `set_recvfunc(...)`
3. 配置
   - 最终到 `SetRoceConfig(...)`

所以从 Astra-Sim 看不到 `RoceTransportEngine` 和 `RdmaPortEngine`。

### 6.2 从 ns-3 设备层看

当前设备层只需要知道：

1. 这个 packet 是否带 `RdmaDispatchTag`
2. 当前 `outPort` 上是否 aggregate 了 `RdmaPortEngine`

因此外部设备层也不需要理解全部 QP 语义。

## 7. 当前整体已经能做到什么

当前版本已经可以做到：

1. `REQUEST / DATA / ACT` 三 lane 分离
2. `flow + lane -> QPN` 稳定映射
3. per-QP 语义调度与 slice 发送
4. RoCE header 生成
5. RX reorder 与有界缓冲
6. ACK 生成与 ACK 收包处理
7. 端口侧 pause-aware 准入
8. 端口侧 inflight packet 限制
9. 端口侧 ACK retire
10. 端口侧 retransmission timer
11. “真正重发时才 retry++”
12. `ROCE=true` 下当前 workload 的 `REQ_DONE / RX_DONE / TX_DONE` 闭环

## 8. 当前结构还缺什么

当前还没有完成的部分主要是：

1. `QP/SQ/RQ/CQ` 的完整端口侧落地
2. `RoceTransportEngine` 与 `RdmaPortEngine` 之间的运行态统一
3. ACK 报文完全从端口侧直接构造和发送
4. RX 路径完全下沉到端口侧
5. `FlowIo / bridge` 与端口侧模型更深的 credit/backpressure 一致性

## 9. 当前最需要警惕的实现风险

当前版本最值得继续审视的不是“有没有类”，而是“职责是不是已经完全对齐”：

1. `RoceTransportEngine` 和 `RdmaPortEngine` 现在同时持有一部分 reliability state
   - 这是一种过渡态，不是最终状态

2. ACK 的路径现在是“端口侧先 retire，语义侧再镜像 retire”
   - 这能工作，但意味着有双份状态

3. 最新 workload 虽然已经功能闭环
   - 但完成时间和前一阶段实现相比已经明显变化
   - 所以不能把当前版本直接视为“纯重构不改行为”

如果你要判断“这版实现是不是还要修”，最应该盯这三件事：

1. 端口侧和 transport 侧是否仍有重复状态
2. 发送准入到底是按 QP 还是按 port/qid 真正起作用
3. 最新时间变化到底来自更合理的执行模型，还是来自某个窗口/节流语义被削弱
