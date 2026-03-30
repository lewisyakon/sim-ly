# RoCE 传输可靠性改动说明

这份文档专门记录最近几轮和 RoCE 传输层有关的改动，目标是回答三个问题：

1. 最近到底改了哪些东西。
2. 这些改动分别落在哪些文件里。
3. 为什么现在把队列从 `2000 -> 350 -> 200` 缩小后，仍然可能看不到明显丢包、PFC 和重传。

## 1. 当前已经落下来的传输层能力

当前分支里，RoCE 这条线已经不再只是“有 QP 语义的旁路头”。最近已经补上的能力包括：

1. 按可见 RoCE 头重算的分片口径
2. 传输层 ACK 报文
3. 每 QP 在途窗口 `transportWindowPackets`
4. 每 QP 一个 oldest-unacked 定时器
5. 超时后的 recovery 模式
6. 统一的待发重传路径
7. future packet 的接收侧重排缓冲与按序释放
8. bounded reorder window 与 overflow 行为
9. ACK 高优先级发送
10. ACK sideband cumulative feedback
11. 自动 ACK timeout 基线推导
12. 主程序级 `network drained / last FLOW_TX_DONE` 时间观测

这意味着当前版本已经不是“无窗口、无 ACK、无重传”的 stage 1 状态了。

## 2. 具体改了哪里

### 2.1 配置结构与默认值

文件：

- `src/min-hop-mac-learning/model/roce-sim-types.h`
- `scratch/my_common.h`
- `simconfig/config.json`

新增或开始真正生效的配置包括：

- `transport_ack_enable`
- `transport_window_packets`
- `ack_timeout_mode`
- `ack_timeout_factor`
- `ack_timeout_ns`
- `max_retransmissions`

其中：

- `transport_window_packets` 控制每个 QP 同时允许多少个 transport packet 处于 outstanding 状态。
- `ack_timeout_mode=auto` 时，会根据拓扑、链路速率、MTU 和 switch delay 先算出一个 RTT 基线，再乘 `ack_timeout_factor` 得到最终 timeout。

自动 timeout 的推导逻辑在 `scratch/my_common.h`，核心函数是：

- `ComputeAutoRoceAckTimeoutBaseNs()`
- `RefreshRoceAckTimeoutFromNetworkConfig()`

### 2.2 分片口径改动

文件：

- `src/min-hop-mac-learning/model/roce-sim-header.h`
- `src/min-hop-mac-learning/model/roce-sim-header.cc`
- `src/min-hop-mac-learning/model/my_sendflowclient.cc`

最近这一轮把 RoCE 的分片预算从“原始 MTU”改成了“`MTU - 可见 RoCE 头`”。

当前等效可见头大小是：

- `SEND -> 12B`
- `WRITE / READ_REQ -> 28B`
- `ACK / READ_RESP -> 16B`

这里的设计口径是：

- 只有可见 RoCE 头从每包预算里额外扣除。
- `FlowTagHeader / 3PHeader / HopCountHeader` 仍然视为隐藏在业务区里的仿真控制数据。

对应代码主要在：

- `BuildPacketForRoceSlice_()`
- `BuildRoceAckPacket_()`
- `RoceSimHeader::GetSerializedSize()`

### 2.3 发送窗口

文件：

- `src/min-hop-mac-learning/model/my_sendflowclient.cc`
- `src/min-hop-mac-learning/model/roce-tx-scheduler.h`
- `src/min-hop-mac-learning/model/roce-tx-scheduler.cc`

当前发送端已经有最小可用的“类滑动窗口”约束。

实现方式是：

1. 发送侧维护 `m_roceOutstandingPackets`。
2. `GetRoceOutstandingCount_()` 统计某个 QP 当前 outstanding 的 packet 数。
3. `CollectBlockedRoceQps_()` 会把达到 `transportWindowPackets` 上限的 QP 放进 blocked 集合。
4. `RoceTxScheduler::PickNextSlice(...)` 选 slice 时会跳过 blocked QP。

因此现在的发送行为不再是：

- 一个 QP 一口气把所有 slice 全部灌进网络。

而是：

- 一个 QP 最多只保留 `transportWindowPackets` 个在途 packet。
- 只有 ACK 回来释放窗口后，后续 slice 才能继续发。

这就是为什么你最近把队列长度从 `2000` 缩到 `350`、再缩到 `200` 后，仍然可能看不到明显丢包和重传的最重要原因之一：

- 当前真正主导 offered load 的，已经不是“应用一口气打满网络”，而是每 QP 的在途窗口。

### 2.4 ACK 发送与处理

文件：

- `src/min-hop-mac-learning/model/my_sendflowclient.cc`

当前 ACK 机制的要点是：

1. 仍然采用每包 ACK 作为基线。
2. ACK 报文使用 `RoceOpcode::Ack`。
3. 业务 `ACT` 仍然只是 3P 控制报文，不和 transport ACK 混用。
4. 发送侧对 ACK 使用累计确认：`ackPsn` 及之前的 outstanding 都会被清掉。
5. ACK 现在按 sideband cumulative feedback 处理，不再参与反向普通控制流的 `expectedRxPsn / reorder window` 状态机。
6. ACK 包自己的 `psn` 只锚定当前累计确认点，不再消耗反向普通控制流的 `nextPsn`。

关键函数：

- `MaybeSendRoceAck_()`
- `HandleRoceAck_()`
- `TrackRoceOutstandingPacket_()`
- `HandleRoceRxHeader_()`

这一步是最近一次最关键的结构修复。

之前的问题是：

1. ACK 一边承担累计确认语义
2. 一边又被当成“反向可靠有序 packet”
3. 于是只要反向 `expectedRxPsn` 卡住，后面的 ACK 即使 `ackPsn` 已经很新，也会被挡在 reorder/overflow 逻辑前面

现在改成：

1. ACK 到达接收入口后，直接走 `HandleRoceAck_()`
2. 不再调用 `OnRxHeader()` 去走反向普通 packet 的有序检查
3. stale ACK 只会自然失效，不会再把 reverse transport 状态机带偏
4. 真正需要按序交付的反向业务控制报文仍然是 `ACT`

### 2.5 ACK 高优先级

文件：

- `src/min-hop-mac-learning/model/PppRoundRobinQueue.h`
- `src/min-hop-mac-learning/model/flow-io.h`
- `src/min-hop-mac-learning/model/flow-io.cc`

这个改动非常关键，因为前一版 ACK 和 DATA 混在一起竞争队列。

当前做法是：

1. transport ACK 使用 `ThreePHeader::IDLE`。
2. `PppRoundRobinQueue` 会把 `REQUEST / ACT / IDLE` 都放进高优先队列。
3. `FlowIo` 的软件缓冲里，`REQUEST / ACT / IDLE` 也会被识别为高优先级控制报文，插到前面。

对应辅助函数：

- `FlowIo::IsHighPriorityControlPacket(...)`

因此现在的 ACK 不再和普通 DATA 完全同级排队。

### 2.5A external path 与下游 RRQ 阈值对齐

文件：

- `src/min-hop-mac-learning/model/flow-io.h`
- `src/min-hop-mac-learning/model/flow-io.cc`

这一步不是 ACK 逻辑本身，但它直接影响最近大窗口实验为什么还能保持无丢包。

当前 external path 的 backpressure 现在看的是：

1. 下游真实 `RRQ` 子队列长度
2. 与 `drop_tail_queue_size` 相同的 `high/low watermark`

而不是只看上游 `FlowIo` 自己的 ingress 长度。

这一步修正以后，行为从：

- “下游 `RRQ/P2P` 已经过阈值甚至开始 drop，但上游还没 pause”

变成了：

- “下游 `RRQ` 到阈值后，external path 立刻 pause，直到子队列回落到 low watermark 再恢复”

这也是最近把 sender window 拉到很大时，系统还能保持：

- `PFC` 很活跃
- 但没有真实 `DROP`

的重要原因。

### 2.5B 主程序收尾时间观测

文件：

- `src/min-hop-mac-learning/model/my_sendflowclient.h`
- `src/min-hop-mac-learning/model/my_sendflowclient.cc`
- `scratch/my_AstraSimNetwork.cc`

最近加了一组显式时间戳，用来替代之前那个不可靠的“从 `Simulator::Run()` 结束时刻反推网络排空时间”的做法。

当前做法是：

1. `sendflowclient::realsend()` 更新最后一次网络发送活动时间。
2. `sendflowclient::HandleRead()` 更新最后一次网络接收活动时间。
3. `[FLOW][TX_DONE]` 额外更新最后一次业务完成时间。
4. `my_AstraSimNetwork.cc` 在 `Run()` 返回后直接读取这两个显式时间戳，而不是再去看 LP / simulator 的尾时间。

因此现在主程序会输出两条更可信的时间：

- `[main] network drained`
- `[main] last FLOW_TX_DONE`

并且统一只给出：

- `ns`
- `us`
- `ms`

以及相对 workload `fire()` 起点的 `since_workload_start`。

这组输出的目的不是替代 Astra-Sim 自己的：

- `all passes finished at time: ...`

而是把：

1. workload/stream 完成
2. 3P 事务完成
3. 网络活动结束

这三种时间口径拆开。

### 2.6 定时器与重传

文件：

- `src/min-hop-mac-learning/model/my_sendflowclient.h`
- `src/min-hop-mac-learning/model/my_sendflowclient.cc`

当前已经从“每包一个 timer”改成了“每 QP 一个 oldest-unacked timer”。

实现方式：

1. `m_roceRetransTimers` 以 `RoceQpKey` 为键。
2. `ArmRoceRetransTimer_()` 只盯当前 oldest outstanding PSN。
3. `CancelRoceRetransTimer_()` 在该 QP 没有 outstanding 时清掉 timer。
4. `HandleRoceRetransTimeout_(RoceQpKey qpKey)` 触发 recovery，不再直接在每个 packet 上挂独立 timeout。

这比早期版本稳很多，因为它避免了“整批 packet 在同一时间一起 timeout”的风暴。

### 2.7 recovery 模式

文件：

- `src/min-hop-mac-learning/model/my_sendflowclient.cc`

当前 timeout 以后，QP 不会继续像没事一样发新包，而是进入 recovery。

核心状态：

- `m_rocePendingRetransmissions`
- `m_roceRecoveryTargetPsns`

行为是：

1. timeout 后，把 oldest outstanding packet 挂进 `m_rocePendingRetransmissions`。
2. 记录 recovery 目标 PSN。
3. `CollectBlockedRoceQps_()` 会把处于 recovery 的 QP 也视为 blocked。
4. 只有当 `HandleRoceAck_()` 看到 `ackPsn >= recoveryTargetPsn`，该 QP 才退出 recovery。

这一步保证了：

- timeout 后先补洞
- 不再一边超时，一边继续无节制推进新的普通包

### 2.8 重传路径

文件：

- `src/min-hop-mac-learning/model/my_sendflowclient.cc`

当前重传已经不再在 timeout 回调里直接立刻 `realsend()`。

新路径是：

1. timeout 先把包放进 `m_rocePendingRetransmissions`
2. `DispatchOneRoceSlice_()` 检查到有待发重传时，会先走：
   - `DispatchPendingRoceRetransmissions_()`
3. 重传报文以高优先的统一 dispatch 路径发出

注意：

- 当前这仍然不是“完整 scheduler 一体化 retransmission”。
- 但它已经比“timeout 里直接裸冲网络”好得多。

### 2.9 接收侧重排缓冲

文件：

- `src/min-hop-mac-learning/model/roce-qp-engine.h`
- `src/min-hop-mac-learning/model/roce-qp-engine.cc`
- `src/min-hop-mac-learning/model/my_sendflowclient.h`
- `src/min-hop-mac-learning/model/my_sendflowclient.cc`

当前 future packet 不再直接上送业务层。

流程是：

1. `RoceQpEngine::OnRxHeader(...)` 遇到 `psn > expectedRxPsn` 时：
   - 标记 `bufferedForReorder=true`
   - 不让它立刻上送上层
2. `sendflowclient::HandleRead()` 收到这个标记后：
   - 把 payload 放进 `m_roceBufferedUpperPackets[qpKey][psn]`
3. `MaybeReleaseBufferedRocePackets_(qpKey)` 会在 `expectedRxPsn` 前移后，按序把连续的缓冲 packet 释放给业务层
4. `ProcessThreePhasePayload_(...)` 承接原先 REQUEST/DATA/ACT 的业务处理逻辑

这一步已经把“QP 只记账，但 future packet 仍直接上送”的问题修掉了。

### 2.10 关于 transport window 和乱序缓冲的高层判断

这部分不是“最近几轮新增了什么功能”，而是最近一次重新梳理后已经收敛下来的设计判断。之所以写在这里，是因为它会直接影响后续 RoCE 应该继续朝哪个方向补。

#### A. RoCE v1 依赖无损以太网，不等于 transport 层没有窗口

当前更合理的理解是：

1. `PFC / ECN / lossless Ethernet` 负责减少网络内部的丢包与拥塞扩散。
2. `RC QP` 仍然负责自己的可靠传输状态：
   - `PSN`
   - `ACK`
   - `timeout`
   - `retry`
   - `outstanding / inflight`
3. 因此当前引入 `transportWindowPackets` 这类“类滑动窗口”控制，是合理的，不是和无损以太网重复建模。

也就是说，RoCE v1 的“依赖无损以太网”与 QP 上的“类滑动窗口发送控制”并不冲突，它们本来就是两层职责。

#### B. “RoCE 面对乱序就是 go-back-N”这个说法太粗

更准确的说法应该是：

1. 发送端 recovery 常常是 go-back-N-like
   - 例如 timeout 以后，从 oldest-unacked 开始补洞
2. 接收端处理未必是“未来包一律丢掉”
   - 现实实现里可以有 bounded reorder buffer
   - 现代 RNIC 也存在对 out-of-order data placement 的支持

因此，当前我们把 future packet 先缓冲、等连续后再按序释放给上层，这个方向本身并不违背现实。真正需要后续继续补的是：

1. reorder window 的容量上限
2. overflow 行为
3. 与 ACK / retransmission 的更紧耦合

#### C. 如果一定要谈 “N”，要区分两个不同的量

1. 发送端的 `N`
   - 更接近 outstanding / inflight window
   - 当前项目里对应的是 `transportWindowPackets`
2. 接收端的 `N`
   - 更接近 reorder / OOO 容忍窗口
   - 当前项目里对应的是 `reorder_window_packets`
   - overflow 行为已收敛成 `drop future packet + cumulative ACK + timeout recovery`

所以“RoCE 的 go-back-N 里的 N 是多少”并没有一个简单的标准答案。更合理的说法是：

- sender side 的 `N` 来自 transport 发送窗口
- receiver side 的 `N` 来自接收侧的乱序容忍窗口

## 3. 这部分判断参考了哪些代码和资料

### 3.1 UB 本地代码

本次最关键的 UB 参考文件在下面这个工程里：

- `/home/lja/桌面/ns-3-ub-main./ns-3-ub-main`

重点读的是：

1. 传输可靠性
   - `/home/lja/桌面/ns-3-ub-main./ns-3-ub-main/src/unified-bus/model/protocol/ub-transport.h`
   - `/home/lja/桌面/ns-3-ub-main./ns-3-ub-main/src/unified-bus/model/protocol/ub-transport.cc`
2. Jetty/上层事务级 inflight 与 ACK bitmap
   - `/home/lja/桌面/ns-3-ub-main./ns-3-ub-main/src/unified-bus/model/protocol/ub-function.h`
   - `/home/lja/桌面/ns-3-ub-main./ns-3-ub-main/src/unified-bus/model/protocol/ub-function.cc`
3. 无损流控
   - `/home/lja/桌面/ns-3-ub-main./ns-3-ub-main/src/unified-bus/model/protocol/ub-flow-control.h`
   - `/home/lja/桌面/ns-3-ub-main./ns-3-ub-main/src/unified-bus/model/protocol/ub-flow-control.cc`

从 UB 里可以直接确认三件事：

1. `PFC/CBFC` 和 transport ACK/重传/window 是分开的两层
2. UB 明确有发送窗口 / inflight limit
3. UB 的接收端不是“纯 future packet 全丢”，而是有 bitmap + 有界乱序缓冲

一个最直接的证据是：

- `UB_TP_PSN_OOO_THRESHOLD = 2048`
  - 定义在 `ub-transport.h`

但要注意：

- `2048` 是 UB 的实现选择
- 不是 “RoCE 标准规定乱序窗口必须是 2048”

### 3.2 当前工程里能直接检查到的 SIMAI / 继承代码

当前工程里直接可检查的 RDMA 相关文件主要是：

1. 真实 verbs 包装层
   - `/home/lja/sim-ly/sim-ly/src/applications/astra-sim/system/SimAiFlowModelRdma.hh`
   - `/home/lja/sim-ly/sim-ly/src/applications/astra-sim/system/SimAiFlowModelRdma.cc`
2. 历史 ns3-rdma 路径的接口引用
   - `/home/lja/sim-ly/sim-ly/src/applications/astra-sim/network_frontend/ns3/entry.h`
   - `/home/lja/sim-ly/sim-ly/src/applications/astra-sim/network_frontend/ns3/common.h`

这部分需要注意一个边界：

1. `entry.h/common.h` 里仍然引用了：
   - `ns3/qbb-helper.h`
   - `ns3/rdma-driver.h`
   - `ns3/rdma.h`
2. 但这份当前 checkout 里没有找到对应源码文件。

因此，本次关于 SIMAI 的直接本地证据，主要还是来自 `SimAiFlowModelRdma.cc`。

这份文件能直接说明：

1. 它创建的是 `RC` QP
2. `ibv_modify_qp(... RTS ...)` 时明确设置了：
   - `timeout`
   - `retry_cnt`
   - `rnr_retry`
3. 接收端依赖 `ibv_post_recv(...)`
4. 数据面主要使用 `RDMA_WRITE / RDMA_WRITE_WITH_IMM`

因此它支持的判断是：

- 在真实 RC verbs 语义里，QP 本来就有 timeout/retry 等 transport 状态
- 这再次说明“无损网络”并不等于“transport 自己不需要可靠发送状态”

### 3.3 外部公开资料

这次高层判断还参考了下面这些公开资料：

1. NVIDIA RoCE 文档
   - <https://docs.nvidia.com/networking/display/rdmaawareprogrammingv17/rdma%2Bover%2Bconverged%2Bethernet%2B%28roce%29>
2. NVIDIA/Cumulus RoCE 文档
   - <https://docs.nvidia.com/networking-ethernet-software/cumulus-linux-42/Network-Solutions/RDMA-over-Converged-Ethernet-RoCE/>
3. `ibv_modify_qp(3)`
   - <https://man7.org/linux/man-pages/man3/ibv_modify_qp.3.html>
4. `ibv_post_recv(3)`
   - <https://man7.org/linux/man-pages/man3/ibv_post_recv.3.html>
5. NVIDIA OOO data placement 文档
   - <https://docs.nvidia.com/doca/archive/2-9-4/out-of-order-data-placement/index.html>
6. NVIDIA QP bringup
   - <https://docs.nvidia.com/networking/display/RDMAAwareProgrammingv17/Queue%2BPair%2BBringup%2B%28ibv_modify_qp%29>

这些资料共同支持下面几点：

1. RoCE v1 对 PFC / lossless Ethernet 的依赖是事实
2. 但 RC QP 仍然有 timeout/retry/rnr 等 transport 语义
3. 现代实现里也存在对 out-of-order data placement 的支持

## 4. 现在为什么缩队列也不容易复现丢包/重传

这不是单一原因，更像下面几件事叠加：

1. `transportWindowPackets` 已经变成主导性的发送 admission control
2. ACK 变成高优先后，窗口释放更快，不容易虚假 timeout
3. ACK sideband 化后，不再把反向有序控制流状态机带偏
4. oldest-unacked 单 timer 避免了定时器风暴
5. future packet 不再直接扰乱业务层，累计 ACK 行为更稳定
6. external path 现在会在下游 RRQ 子队列达到阈值时提前 pause
7. 当前这轮 workload 和 queue size 组合，已经更像“无损但高回压”的区间，而不是“自然进入 drop/retransmission”的区间

因此现在看到“把 sender window 提得很大，甚至接近一次性发送极限，依然没有明显丢包/重传，但总时间反而变长”，一个很合理的解释就是：

- 当前 transport 层自己的窗口、ACK sideband 化，以及 external path 基于 RRQ 阈值的 backpressure，已经把系统从“易错区”推到了“无丢包但高排队/高回压”的区间。

这不说明网络队列配置完全不重要，而是说明：

- 在你当前这组 workload 和参数下，**发送端注入强度与回压耦合更主导**。

## 5. bounded reorder window 设计收敛

这部分是下一阶段准备优先落的内容，因此先把设计和参数口径定死。

### 5.1 窗口语义

每个 QP 的接收侧维护一个以 `expectedRxPsn` 为左边界的有界乱序窗口。

窗口定义为：

```text
[ expectedRxPsn, expectedRxPsn + reorderWindowPackets )
```

处理规则固定为：

1. `psn < expectedRxPsn`
   - 视为旧包/重复包
   - 不再进入 reorder buffer
   - 不上送业务层
2. `psn == expectedRxPsn`
   - 正常按序接收
   - 推进 `expectedRxPsn`
   - 并尝试释放后面已经连续的 buffered packet
3. `expectedRxPsn < psn < expectedRxPsn + reorderWindowPackets`
   - 允许进入 reorder buffer
   - 不立刻上送业务层
4. `psn >= expectedRxPsn + reorderWindowPackets`
   - 视为 reorder overflow
   - 不进入 reorder buffer
   - 不上送业务层
   - 交由发送端的 timeout / retransmission 去恢复

### 5.2 参数先定成什么

第一版先只引入一个窗口参数：

- `reorder_window_packets`

默认值先定死为：

```text
reorder_window_packets = transport_window_packets
```

按当前默认配置，就是：

```text
reorder_window_packets = 32
```

这样定的原因很简单：

1. sender side 最多只允许 `transportWindowPackets` 个 packet outstanding
2. receiver side 没必要允许一个比 sender outstanding 还更大的 future-packet gap
3. 这样 sender/receiver 对“同一时刻最多能离散到多大”有一致上界

当前 JSON 已支持 `reorder_window_packets`，推荐约束也直接定死：

```text
1 <= reorder_window_packets <= transport_window_packets
```

第一版不再单独引入 `bytes` 维度，也不搞多套窗口，避免模型复杂度先失控。

### 5.3 overflow 行为先定成什么

overflow 的行为这次也先定死，不再留成开放项。

当接收侧发现：

```text
psn >= expectedRxPsn + reorderWindowPackets
```

第一版行为固定为：

1. 丢弃这个 packet 的业务上送资格
2. 不把它塞进 reorder buffer
3. 记录 `rxReorderOverflowPackets`
4. ACK 仍然只回当前累计点：
   - `ackPsn = expectedRxPsn - 1`
5. 不额外引入 NAK/SACK
6. 依赖现有 timeout/recovery 机制补回缺失 packet

也就是说，overflow 不是“QP 直接进 Err”，也不是“无限缓存”，而是：

- **drop future packet, keep cumulative ACK, let sender recover**

### 5.4 这和 go-back-N 的关系

按这个设计，receiver side 不是“纯粹的无缓冲 go-back-N”：

1. 在窗口内，允许 bounded reorder buffering
2. 超出窗口，才退回到更保守的“丢 future packet，等待 sender recovery”

所以它更接近：

- **bounded OOO receiver + go-back-N-like sender recovery**

而不是：

- **纯 receiver 端无缓冲的经典 go-back-N**

### 5.5 这一步落完以后，ACK/重传还剩什么

这一步做完以后，乱序处理这条线就会比较清楚：

1. receiver 能缓多少：由 `reorder_window_packets` 定死
2. 超窗怎么办：固定成 `drop + cumulative ACK + timeout recovery`
3. sender 怎么补洞：继续用现有 recovery / retransmission

这样后面 ACK 这条线就可以单独讨论，不会再和“receiver 到底能缓多少 future packet”搅在一起。

## 6. 当前还没完成的地方

虽然最近改动很多，但当前 RoCE 可靠传输还没有完全做完。

还缺的主要是：

1. `SQ / RQ / CQ` 的完整语义
2. 更真实的 receiver-side `RQ` 资源模型
3. retransmission 更深地并入统一 scheduler
4. 动态 RTO（例如 SRTT/RTTVAR）
5. 事务完成统计与 transport 完整性的更严格绑定
6. 更完整的累计 ACK / recovery 策略细化

## 7. 建议怎么读代码

如果你现在要顺着代码看这一轮改动，建议顺序是：

1. `src/min-hop-mac-learning/model/roce-sim-types.h`
   - 先看配置和 QP context 里增加了哪些字段
2. `src/min-hop-mac-learning/model/my_sendflowclient.h`
   - 再看发送端和接收端新增了哪些 transport 状态
3. `src/min-hop-mac-learning/model/my_sendflowclient.cc`
   - 再看 ACK、窗口、recovery、reorder 的具体实现
4. `src/min-hop-mac-learning/model/roce-qp-engine.cc`
   - 再看 future packet 在 QP 层是如何被标记为 reorder buffered 的
5. `src/min-hop-mac-learning/model/flow-io.cc`
   - 最后看 ACK 高优先在软件缓冲里是如何落地的
6. `src/min-hop-mac-learning/model/PppRoundRobinQueue.h`
   - 再看 ACK 高优先在设备 RRQ 中是如何落地的
