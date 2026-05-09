# RoCE 现实匹配性审查与差距分级

最近修改：2026-04-23

本文件用于记录一次冻结在 `2026-04-09` 的判断。

这次审查只回答 4 个问题：

1. 当前实现和现实中的 RoCE 到底有多像。
2. 现在哪些点已经足以支撑“网络行为近似”。
3. 现在哪些点仍然偏离现实，且会影响大规模网络仿真。
4. 这些差距里，哪些是必须补、建议补、可以暂缓。

本轮只做判断记录，不推动代码修改。

## 1. 审查边界

这次审查采用下面这个标准：

1. 目标优先级是“网络行为相似”，不是“RNIC 内部实现逐条复刻”。
2. 如果某个内部细节不太真实，但不会明显扭曲队列、回压、重传、拥塞反馈、乱序恢复、ECMP 行为，可以容忍一定简化。
3. 如果某个缺口会明显影响大规模 AI 集群里的拥塞扩散、尾延迟、PFC 活跃度、吞吐公平性或路径负载分布，就应当视为高优先级差距。
4. 结论只针对当前工作区中的实现，不把“文档里计划未来要做”的能力算作已完成。

## 2. 一句话结论

当前实现已经不是“只有 RoCE 名字的业务旁路”，而是一个带有 `QP / PSN / ACK / timeout / retransmission / reorder / PFC pause-aware scheduling` 的 `RC-like` 仿真传输层。

但如果对标对象是现实中 AI 集群常见的 `RoCEv2 RC`，那么当前版本仍然只能算“部分匹配”，还不能直接说成“已经和真实 RoCE 在网络行为上基本等价”。

更准确的定位应当是：

- **当前版本更像：`PFC-aware` 的自定义二层 `RC-like` RoCE 近似模型**
- **而不是：完整意义上的现实 `RoCEv2 + ECN/CNP/DCQCN` 大规模部署模型**

## 3. 当前已经匹配现实的部分

下面这些点，我认为已经进入“不是口号式 RoCE，而是真的在建模传输行为”的范围。

### 3.1 QP / PSN / 有序接收这条主骨架已经在

当前代码里已经有：

1. QP 状态和 `PSN`
2. 发送侧每 QP 的 packet 编号
3. 接收侧按 `expectedRxPsn` 推进
4. 有界乱序缓存
5. 旧包/重复包识别

对应代码：

- [roce-qp-engine.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/roce-qp-engine.cc#L101)
- [roce-qp-engine.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/roce-qp-engine.cc#L154)
- [rdma-port-engine.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/rdma-port-engine.cc#L192)

这和真实 `RC` 至少在“每连接维护可靠有序状态”这件事上是同方向的。

### 3.2 已经有发送窗口、累计 ACK 和超时重传

当前并不是“无限灌包然后全靠底层以太网”。

当前代码里已经有：

1. 每 QP 在途 packet 上限 `transportWindowPackets`
2. ACK 到来后按累计确认释放 outstanding
3. 每 QP 一个 oldest-unacked 风格的 timer
4. timeout 后进入 recovery，并阻塞该 QP 继续发新包

对应代码：

- [rdma-port-engine.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/rdma-port-engine.cc#L333)
- [rdma-port-engine.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/rdma-port-engine.cc#L579)
- [rdma-port-engine.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/rdma-port-engine.cc#L1132)
- [rdma-port-engine.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/rdma-port-engine.cc#L1173)
- [scratch/my_common.h](/home/lja/sim-ly/sim-ly/scratch/my_common.h#L1010)

对于网络仿真来说，这一步很关键，因为它已经不再是“应用侧无限注入、链路侧被动排队”的模型。

### 3.3 PFC 路径是真实接通的

当前项目不是只在文档里提 PFC。

我看到的事实是：

1. bridge / ingress queue 会根据高低水位触发 pause / resume
2. `PointToPointNetDevice` 会识别 `PauseHeader`
3. `RdmaPortEngine` 监听 `QueuePauseState`
4. pause 会直接阻止对应队列继续发送

对应代码：

- [min-hop-bridge-net-device.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/min-hop-bridge-net-device.cc#L437)
- [min-hop-bridge-net-device.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/min-hop-bridge-net-device.cc#L1851)
- [point-to-point-net-device.cc](/home/lja/sim-ly/sim-ly/src/point-to-point/model/point-to-point-net-device.cc#L498)
- [rdma-port-engine.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/rdma-port-engine.cc#L1275)
- [flow-io.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/flow-io.cc#L660)

所以当前版本对“无损网络中的回压”和“pause 对发送推进的影响”已经不是空的。

### 3.4 线长口径已经开始向真实可见头靠齐

当前 `RoceSimHeader` 已经把可见 transport 头大小纳入 packetization：

1. `SEND -> 12B`
2. `WRITE / READ_REQ -> 28B`
3. `ACK / READ_RESP -> 16B`

对应代码：

- [roce-sim-header.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/roce-sim-header.cc#L20)
- [roce-tx-scheduler.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/roce-tx-scheduler.cc#L118)
- [rdma-port-engine.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/rdma-port-engine.cc#L683)

这对大规模仿真是加分项，因为它直接影响链路占用、排队长度和 serialization delay。

## 4. 当前与现实 RoCE 的主要偏离

下面这些偏离里，有些只是“内部过程更简化”，有些则会直接影响网络行为。

### 4.1 当前主路径不是现实中常见的 RoCEv2 外层

当前代码默认使用自定义二层 `etherType=0x9999`：

- [roce-sim-types.h](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/roce-sim-types.h#L62)
- [rdma-port-engine.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/rdma-port-engine.cc#L801)
- [rdma-port-engine.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/rdma-port-engine.cc#L923)

而现实中大规模数据中心更常见的是 `RoCEv2`，它和 `UDP/IP`、优先级、QoS、ECMP 哈希、ECN 标记密切耦合。

因此当前实现更像：

- 在以太网上直接承载一个“仿真版 RoCE transport 头”

而不是：

- 现实里常见的 `RoCEv2 over UDP/IP`

这会影响：

1. 路由和 ECMP 哈希口径
2. QoS 分类与优先级映射
3. ECN 标记与回传路径

### 4.2 当前主路径里没有看到 `ECN/CNP/DCQCN` 的实际闭环

我在当前主路径代码中搜索了 `ECN / CNP / DCQCN / LDCP / QCN`，没有看到落到 `src/min-hop-mac-learning/model` 与 `scratch` 主配置路径中的实现。

当前 `scratch` 里显式配置的是：

1. `transport_ack_enable`
2. `transport_window_packets`
3. `reorder_window_packets`
4. `ack_timeout_*`
5. `max_retransmissions`

对应代码：

- [scratch/my_common.h](/home/lja/sim-ly/sim-ly/scratch/my_common.h#L1010)
- [scratch/my_common.h](/home/lja/sim-ly/sim-ly/scratch/my_common.h#L1218)

这说明当前主实现的拥塞控制重心仍然在：

- `PFC`
- 发送侧在途窗口
- ACK / timeout recovery

而不是现实大规模部署里常见的：

- `ECN mark -> CNP -> per-QP rate adaptation / DCQCN`

这一点会直接影响大规模网络中的：

1. incast 下的队列增长方式
2. PFC 活跃度
3. 流间公平性
4. 长流与短流在拥塞下的时延表现

### 4.3 当前恢复机制还是简化版 `RC-like`

当前接收侧对 future packet 的处理是：

1. 在有界窗口内缓存
2. 超窗则不上送
3. 继续只靠累计 ACK + timeout recovery 补洞

对应代码：

- [roce-qp-engine.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/roce-qp-engine.cc#L200)
- [rdma-port-engine.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/rdma-port-engine.cc#L253)
- [rdma-port-engine.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/rdma-port-engine.cc#L1173)

这并不等于错误，但它还是比真实 RC transport 粗。

当前缺少或未显式建模的真实要素包括：

1. `RNR` 相关接收端资源不足语义
2. 更明确的 `NAK-Sequence Error / Implied NAK` 风格恢复来源
3. 更接近硬件实现的 retry/backoff 语义

### 4.4 QP bringup 过程被压缩成自动推进

当前发送前会自动把 QP 从 `Reset -> Init -> Rtr -> Rts` 推到可发：

- [rdma-port-engine.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/rdma-port-engine.cc#L560)

但真实 RC QP bringup 需要显式设置的参数包括：

1. `dest_qp_num`
2. `rq_psn`
3. `sq_psn`
4. `timeout`
5. `retry_cnt`
6. `rnr_retry`
7. `min_rnr_timer`

这一点对“内部过程真实性”影响更大，对纯网络行为的直接影响相对次一层，但它确实意味着当前模型还没有把一些重要的 RC 约束显式化。

### 4.5 3P 业务控制流仍然在参与线上报文组成

当前 `REQUEST / DATA / ACT` 仍然通过 `RoceTransportEngine` 映射到不同 opcode：

- [roce-transport-engine.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/roce-transport-engine.cc#L31)
- [my_sendflowclient.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/my_sendflowclient.cc#L441)

这意味着当前线上流量并不完全等同于真实 verbs 应用产生的报文谱系。

对大消息主导的 AI 训练 workload，这个偏离通常不是第一主矛盾。
但对小消息密集或控制包占比高的 workload，这会影响流量组成。

## 5. 差距分级

这里的分级不是“功能有没有实现”，而是“对万卡 / 十万卡网络行为拟真的重要性”。

## 5.1 A 级：必须补齐

这些点如果长期缺席，就不适合把模型直接表述成“现实 AI 集群 RoCEv2”。

### A1. 明确并落实目标协议层次

要先明确：

1. 目标到底是 `RoCEv1-like` 还是 `RoCEv2 RC`
2. 如果目标是现实 AI 集群主流，应优先对齐 `RoCEv2`

若目标确定为 `RoCEv2`，则至少需要把下面这些行为在模型里站稳：

1. `UDP/IP` 外层
2. QoS / 优先级映射
3. 与 ECMP / 哈希 / spray 的一致口径

否则很多“网络行为”对齐只能停留在局部。

### A2. 建立 `ECN -> CNP -> rate control` 的拥塞反馈闭环

在大规模数据中心里，`PFC` 往往不是唯一主角。

如果没有这条闭环，模型会系统性低估或误判：

1. 拥塞传播方式
2. PFC 依赖程度
3. 流间公平性
4. 尾部时延

这一项是当前离现实大规模部署最明显的差距。

## 5.2 B 级：建议补齐

这些点不一定是第一轮必须补，但如果后续要做“定量对齐现实行为”的结果，就应尽快收进来。

### B1. 把当前简化恢复机制升级成更接近 RC 的恢复模型

优先关注：

1. `RNR` 及接收资源压力
2. `NAK / implied NAK / sequence error` 的抽象恢复来源
3. retry/backoff 语义

当前实现已有 recovery 骨架，但还比较像“够用的统一 timeout 补洞器”。

### B2. 显式化关键 QP transport 参数

当前自动 bringup 在工程上很方便，但如果后续要做现实校准，建议把这些量显式进入配置或上下文：

1. `timeout`
2. `retry_cnt`
3. `rnr_retry`
4. `min_rnr_timer`
5. `rq_psn / sq_psn`
6. `dest_qpn`

### B3. 校准 ACK / 反向流量的建模粒度

当前 ACK 路径已经可用，但仍然是相当工程化、简化的实现。

如果后续发现 reverse path 对时延或公平性影响变大，就需要单独复核：

1. ACK 发送策略
2. ACK 排队优先级
3. ACK 对控制流和反向流的占用程度

### B4. 复核多 QP / 多端口 / NIC 内部仲裁

万卡和十万卡仿真里，NIC 端内部仲裁会越来越重要。

当前调度器已经有主骨架：

- [roce-tx-scheduler.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/roce-tx-scheduler.cc#L76)

但如果后续要更逼近现实，还需要继续看：

1. 多 QP 同活跃时的仲裁
2. 多出端口绑定
3. control/data/retransmission 的更细粒度竞争

## 5.3 C 级：可以暂缓

这些点对“网络行为相似”重要性较低，可以在当前阶段先不优先投入。

### C1. 完整 verbs / CQ / SRQ / MR 内部对象化

只要上层不依赖精细的 verbs 行为，这些对象级完整度可以后移。

### C2. 更完整的 memory placement / rkey / atomic 语义

当前网络仿真的主矛盾不在这里。

### C3. 更完整的 host software stack 细节

如果当前目标是网络行为而不是驱动或用户态 verbs 语义，这部分可以继续简化。

## 6. 当前冻结判断

在不改实现的前提下，我建议把当前模型对外描述为：

- **一个带有 `PFC`、`per-QP window`、`ACK/timeout/retransmission`、有界乱序恢复的 `RoCE RC-like` 仿真模型**

不建议直接表述成：

- **已经与现实 AI 集群的 `RoCEv2` 在网络行为上基本等价**

更保守但准确的说法是：

- 当前已经覆盖了“无损 / 高回压网络中的 RC-like 数据面主骨架”
- 但尚未覆盖“现实大规模部署中的 `RoCEv2 + ECN/CNP/DCQCN` 关键拥塞反馈闭环”

## 7. 外部信源

这次外部资料只优先采用了厂商技术文档和高可信论文。

### 7.1 NVIDIA: Transport Modes

链接：

- <https://docs.nvidia.com/networking/display/rdmaawareprogrammingv17/transport%2Bmodes>

本次采用的判断依据：

1. `RC` 是可靠连接
2. `RC` 保证可靠投递
3. `RC` 保证按序交付

### 7.2 NVIDIA: Queue Pair Bringup (ibv_modify_qp)

链接：

- <https://docs.nvidia.com/networking/display/rdmaawareprogrammingv17/queue%2Bpair%2Bbringup%2B%28ibv_modify_qp%29>

本次采用的判断依据：

1. 真实 QP 需要 `RESET -> INIT -> RTR -> RTS`
2. `RTR` 需要 `dest_qp_num / rq_psn / min_rnr_timer`
3. `RTS` 需要 `timeout / retry_cnt / rnr_retry / sq_psn`

### 7.3 NVIDIA: RoCE on Onyx

链接：

- <https://docs.nvidia.com/networking/display/onyxv3104006/rdma%2Bover%2Bconverged%2Bethernet%2B%28roce%29>

本次采用的判断依据：

1. `RoCE Congestion Management` 依赖 `ECN`
2. congestion notification 通过 `CNP` 回送到 source，并约束相关 QP 的注入速率
3. `RoCEv2` 背景下底层网络通常按 lossless 配置
4. 典型 QoS 映射里 `RoCE` 与 `CNP` 使用不同优先级
5. 文档页面明确给出 `UDP 4791`

### 7.4 NVIDIA: Cumulus Linux RoCE

链接：

- <https://docs.nvidia.com/networking-ethernet-software/cumulus-linux-52/Layer-1-and-Switch-Ports/Quality-of-Service/RDMA-over-Converged-Ethernet-RoCE/>

本次采用的判断依据：

1. RoCE 依赖 `ECN` 和 `PFC`
2. `lossless` 模式下优先使用 `PFC`
3. 这再次说明现实数据中心 RoCE 不是只看传输层重传

### 7.5 NVIDIA: RDMA Capabilities

链接：

- <https://docs.nvidia.com/networking/display/winof2v244/rdma+capabilities>

本次采用的判断依据：

1. RoCE 上存在“excessive retransmissions”这类 driver / adapter 侧能力
2. 文档明确提到 `Local Ack Timeout`
3. 文档明确提到 `NAK-Sequence Error`
4. 文档明确提到 `Implied NAK`

这说明真实恢复路径比当前“单一 timeout 补洞”要更丰富。

### 7.6 NVIDIA: DOCA RDMA Programming Guide

链接：

- <https://docs.nvidia.com/doca/archive/doca-v2.2.0/rdma-programming-guide/index.html>

本次采用的判断依据：

1. 基于以太网的 RDMA 可以是 `RoCE`
2. 当前 DOCA RDMA 明确只支持 `RC`
3. `Receive / Send / Write / Read / Atomic` 都建立在这个 `RC` 连接上

### 7.7 SIGCOMM 2015: DCQCN

链接：

- <https://haggaie.github.io/publications/2015-dcqcn>

本次采用的判断依据：

1. 大规模数据中心的 `RoCEv2` 部署依赖 `PFC`
2. 仅有 `PFC` 会带来性能问题
3. `DCQCN` 用于改善 `RoCEv2` 的吞吐与公平性

## 8. 本地代码依据

本次判断主要基于下面这些文件的现状：

1. [rdma-port-engine.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/rdma-port-engine.cc)
2. [rdma-port-engine.h](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/rdma-port-engine.h)
3. [roce-qp-engine.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/roce-qp-engine.cc)
4. [roce-transport-engine.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/roce-transport-engine.cc)
5. [roce-tx-scheduler.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/roce-tx-scheduler.cc)
6. [roce-sim-header.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/roce-sim-header.cc)
7. [my_sendflowclient.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/my_sendflowclient.cc)
8. [min-hop-bridge-net-device.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/min-hop-bridge-net-device.cc)
9. [flow-io.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/flow-io.cc)
10. [point-to-point-net-device.cc](/home/lja/sim-ly/sim-ly/src/point-to-point/model/point-to-point-net-device.cc)
11. [scratch/my_common.h](/home/lja/sim-ly/sim-ly/scratch/my_common.h)

另外，这次也参考了 `UB` 侧 transport 骨架，用于辅助判断“更完整的 transport realism 可以长什么样”：

1. [/home/lja/桌面/ns-3-ub-main./ns-3-ub-main/src/unified-bus/model/protocol/ub-congestion-control.h](/home/lja/桌面/ns-3-ub-main./ns-3-ub-main/src/unified-bus/model/protocol/ub-congestion-control.h)
2. [/home/lja/桌面/ns-3-ub-main./ns-3-ub-main/src/unified-bus/model/protocol/ub-transport.cc](/home/lja/桌面/ns-3-ub-main./ns-3-ub-main/src/unified-bus/model/protocol/ub-transport.cc)

## 9. 当前结论的使用方式

如果后续要汇报或写实验说明，建议按下面口径使用：

1. 可以说“当前模型已具备 `PFC-aware RC-like` 传输骨架”
2. 可以说“当前模型已经能研究无损网络中的回压、窗口、超时恢复和有界乱序”
3. 暂时不要直接说“当前模型已经等价于现实大规模 `RoCEv2`”
4. 如果后续论文或报告要对标现实集群，应明确注明当前未纳入的 `ECN/CNP/DCQCN` 部分

## 10. 本次记录的验证状态

本次判断记录完成时，工作区构建状态为：

- `./ns3 build` 通过

这说明本文件描述的是当前可构建版本的实现状态，而不是脱离代码的设想。
