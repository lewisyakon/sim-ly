# QP 与传输职责说明

## 1. QP 是什么

`QP` 是 `Queue Pair`，即“队列对”。在真实 RoCE/IB 里，它是一个通信端点。应用不是直接对“整个 NIC”发 RDMA 操作，而是对某个 QP 投递 `SEND / WRITE / READ` 请求。

从功能上看，一个 QP 可以理解成：

- 一条逻辑连接的本地状态
- 一组发送/接收队列
- 一套可靠传输上下文

## 2. QP 的核心组成

一个较完整的 QP 通常包含：

1. `SQ`，Send Queue
   - 保存发送工作请求 `WR/WQE`

2. `RQ`，Receive Queue
   - 保存接收缓冲描述符

3. `QP State`
   - 典型状态：`RESET / INIT / RTR / RTS / ERR`

4. `PSN` 相关状态
   - 下一个发送 PSN
   - 期望接收的下一个 PSN

5. 队列深度与资源限制
   - `SQ depth`
   - `RQ depth`

6. 连接属性
   - `QPN`
   - 对端 `QPN`
   - 传输类型 `RC / UC / UD`
   - MTU、重试、超时、权限

## 3. 与 QP 强相关但不完全属于 QP 本体的对象

1. `CQ`
   - Completion Queue

2. `MR`
   - 已注册内存区域

3. `PD`
   - Protection Domain

## 3.1 `post_send / post_recv` 的正确口径

`post_send / post_recv` 是本地 verbs 动作，不是网络中的控制报文。

更准确地说：

1. `post_send`
   - 往本地 `SQ` 投递发送工作请求
2. `post_recv`
   - 往本地 `RQ` 投递接收工作请求

因此：

- 发送端不会通过网络“发一个 post 包”去改对端状态
- 但 `SEND` 语义成立的前提，仍然是对端本地已经准备好了可消费的 `RQ` entry

这也是为什么：

- 控制类通道适合长期预贴 `RQ`
- 大数据通道更适合走 `WRITE`

## 4. QPN 是什么

`QPN` 是 `Queue Pair Number`，即 QP 编号。真实 RNIC 收到包后，会根据包头里的 QPN 找到对应的 QP 上下文。

当前项目里做了一个简化：

- 不是直接 `qpn = tag`
- 而是按 `flow = {tag, src, dst}` 三元组稳定分配 synthetic `qpn`

这样做的原因是：

1. 不能保证 `tag` 全局不重复
2. 但当前可以把 `tag/src/dst` 视为 flow 唯一键
3. 所以更稳妥的做法是：
   - 先用 flow 三元组定位一条逻辑流
   - 再在本地给这条流分配稳定的 synthetic `qpn`

因此当前更准确的说法是：

- 一条 `tag/src/dst` flow 对应一条仿真的 QP
- `qpn` 是该 flow 的稳定局部编号，不是直接等于 `tag`

## 5. 一个 NIC 上会有多个 QP 吗？为什么？

会，而且真实系统里通常就是多个。

原因不是“实现习惯”，而是协议和工程上都需要：

1. **不同通信需要独立状态**
   - 每条连接都有自己的 `PSN / ACK / 重传 / 状态`
   - 这些状态不能混成一套

2. **避免所有流共享一个顺序域**
   - 如果只有一个 QP，一个远端阻塞会拖住其他远端
   - 这会导致明显的 HOL blocking

3. **支持多个远端并发通信**
   - GPU / NIC 往往要同时与多个 peer 通信
   - 每个 peer 或每类流量都可能需要独立 QP

4. **资源和权限隔离**
   - 不同应用、不同优先级、不同 memory region，通常都希望隔离

所以更准确地说：

- `NIC` 是承载多个 QP 的设备
- `QP` 是挂在 NIC 上的逻辑通信上下文

## 6. 当前项目里的 QP 简化到了什么程度

当前工程中的轻量 QP 上下文定义在：

- `src/min-hop-mac-learning/model/roce-sim-types.h`

当前保留的字段主要是：

- `state`
- `nextPsn`
- `expectedRxPsn`
- `pendingRxPsns`
- `sqDepth`
- `rqDepth`
- `txPackets`
- `rxPackets`

因此它当前主要承担：

- QP 状态机
- PSN 分配与顺序检查
- future PSN 的轻量乱序跟踪
- 发送/接收合法性校验
- RoCE 头生成的上下文支撑

它目前还没有完整承担：

- 显式 `SQ`
- 显式 `RQ`
- 显式 `CQ`
- `SEND` 所需的 posted receive 资源约束
- 待发 WQE 排队
- 多个待发请求的仲裁
- inflight/send window
- ACK 驱动发送推进
- 超时重传
- 乱序 ACK/重排闭环
- 完整 CQ 驱动的发送调度

## 7. 当前项目里 QP 相关代码的真实行为

当前已经不是“触发即发包”了。

当前发送路径是：

1. `TriggerSendTo_now(...)`
2. `EnqueueRoceWork_(...)`
3. `RoceTxScheduler`
4. `DispatchOneRoceSlice_()`
5. `BuildRoceTxHeader_(...)`
6. `realsend(...)`

也就是说，当前 QP / scheduler 已经开始接管：

- 发送入口
- 控制类优先级
- 数据类按 QP 的轮询顺序
- 逐 slice 出网

但当前它仍然没有承担下面这些 transport 能力：

1. transport ACK 驱动的推进
2. inflight/send window
3. retransmission / timeout
4. 乱序 ACK / NAK / 重排闭环
5. 完整 CQ 驱动发送

因此当前 QP / scheduler 更准确的定位是：

- 已经开始承担“发送调度入口”
- 但还不是完整 RC transport endpoint

## 8. UB 已经实现了哪些 transport 能力

UB 的强项在 transport behavior，而不是更细的 RoCE verb 语义。

它已经明确实现的能力包括：

1. **传输端点本身就是可调度队列**
   - `UbTransportChannel` 继承 `UbIngressQueue`
   - [ub-transport.h](/home/lja/桌面/ns-3-ub-main/src/unified-bus/model/protocol/ub-transport.h:31)

2. **ACK 优先发送**
   - `m_ackQ` 高优先
   - [ub-transport.cc](/home/lja/桌面/ns-3-ub-main/src/unified-bus/model/protocol/ub-transport.cc:138)

3. **per-TP 的 WQE segment 队列**
   - [ub-transaction.cc](/home/lja/桌面/ns-3-ub-main/src/unified-bus/model/protocol/ub-transaction.cc:240)

4. **inflight/send window 限制**
   - [ub-transport.cc](/home/lja/桌面/ns-3-ub-main/src/unified-bus/model/protocol/ub-transport.cc:155)
   - [ub-transport.cc](/home/lja/桌面/ns-3-ub-main/src/unified-bus/model/protocol/ub-transport.cc:661)

5. **`snd_nxt / snd_una` 推进**
   - [ub-transport.cc](/home/lja/桌面/ns-3-ub-main/src/unified-bus/model/protocol/ub-transport.cc:208)
   - [ub-transport.cc](/home/lja/桌面/ns-3-ub-main/src/unified-bus/model/protocol/ub-transport.cc:331)

6. **接收侧 ACK 生成**
   - [ub-transport.cc](/home/lja/桌面/ns-3-ub-main/src/unified-bus/model/protocol/ub-transport.cc:581)

7. **乱序 bitmap**
   - [ub-transport.cc](/home/lja/桌面/ns-3-ub-main/src/unified-bus/model/protocol/ub-transport.cc:547)

8. **超时重传**
   - [ub-transport.cc](/home/lja/桌面/ns-3-ub-main/src/unified-bus/model/protocol/ub-transport.cc:614)

9. **拥塞控制**
   - `CAQM` 已实现
   - `LDCP / DCQCN` 目前主要还是枚举声明
   - [ub-congestion-control.h](/home/lja/桌面/ns-3-ub-main/src/unified-bus/model/protocol/ub-congestion-control.h:14)

10. **PFC / CBFC / shared credit**
   - [ub-flow-control.h](/home/lja/桌面/ns-3-ub-main/src/unified-bus/model/protocol/ub-flow-control.h:137)

## 9. 当前项目与 UB 的差异

一句话概括：

- 当前项目的 QP：更像“RoCE 协议状态检查器 + header/PSN 语义附加器”
- UB 的 TP/Jetty：更像“真正参与发送调度的 transport endpoint”

更细一点：

| 维度 | 当前项目 | UB |
|---|---|---|
| 上层模型 | Astra-Sim + 3P | WQE / Task / TP |
| QP/TP 角色 | 轻量语义层 | 真正可调度传输端点 |
| 发送仲裁 | 已进入 `sendflowclient + RoceTxScheduler` 阶段 1 路径 | 在 TP / allocator 中完成 |
| 可靠性 | 轻量 PSN/QP 检查 | ACK / inflight / RTO / 重传 / 乱序 |
| 完成语义 | 业务 ACT | 协议 ACK 推进 |

## 10. 为什么下一阶段先做“QP 接管发送调度”

这是阶段 1 当时最合理的切分。

原因有三个：

1. **当前最大短板曾经是开了 RoCE 仍然是 legacy 直发**
   - 这是客户最容易看出来的问题
   - 当前阶段 1 已经把这点补上

2. **当时先不碰 RC 可靠闭环，能避免把调度问题和 ACK/重传问题搅在一起**
   - 调度入口和 RC 可靠闭环是两个不同层面的改造
   - 阶段 1 先收敛发送入口，比一口气把 ACK/重传全堆进来更稳

3. **能保住 Astra-Sim + 3P 主流程**
   - workload 和 REQUEST / DATA / ACT 观察口都不用推翻

所以第一阶段的已完成项是：

1. per-QP work queue
2. 多 QP 发送仲裁
3. 控制报文优先
4. 数据报文 round-robin
5. 逐 slice 出网

而当前阶段 1 暂不做：

1. transport ACK
2. inflight/send window
3. retransmission / timeout
4. 完整 CQ 驱动发送

按最新设计决议，这些能力已经明确属于后续 RoCE Phase 2，而不是永久不做。

## 11. 这套阶段划分能否对客户成立

可以，但表述要准。

可以说：

- 当前中间版本已经完成 RoCE 语义接入。
- 当前版本已经把发送入口和基本调度顺序迁移到 QP / scheduler 层。
- 可靠 ACK / 重传 / 更完整窗口控制属于下一阶段，将在 RoCE 层继续补齐。

不应该说：

- 已经完成完整 RoCE RC 传输层
- 当前 QP 已完整等价于真实 RNIC QP

这两句会把承诺说过头。
