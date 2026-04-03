# RoCE Phase 2 设计说明

这份文档记录最近一次讨论后已经明确下来的设计边界。

它不描述“当前代码已经做到什么”，而描述：

1. 下一阶段要把 RoCE 做成什么样
2. 哪些概念已经定死
3. 当前代码和目标之间还差什么

## 1. 目标

下一阶段的目标不是做完整 RNIC，也不是做真实内存系统，而是把当前版本从“RoCE 语义旁路”推进到“更像 RC transport 的网络仿真”。

边界如下：

- 继续服务 `Astra-Sim + 3P`
- 只公开对外宣讲 `SEND + WRITE`
- `READ` 可以保留代码入口，但不再作为对外能力
- `ACT` 继续是业务控制报文，不是 transport ACK
- 重传和有限乱序处理由 RoCE 层负责，而不是底层以太网层

## 2. 已确定的设计口径

## 2.1 发送语义

当前对外只保留两种主语义：

1. `SEND`
2. `WRITE`

其中：

- `REQUEST`
  - 作为控制报文，默认走 `SEND`
- `DATA`
  - 作为数据报文，默认走 `WRITE`
- `ACT`
  - 作为控制报文，默认走 `SEND`

`READ` 相关 opcode 可以暂时保留在代码和枚举中，但不再作为对外能力描述。

## 2.2 `post` 的定义

`post_send` / `post_recv` 是本地动作，不是网络中的控制报文。

因此：

1. `post_send`
   - 只修改本地 `SQ`
2. `post_recv`
   - 只修改本地 `RQ`

但 `SEND` 的成立前提是：

- 发送端本地有 `SQ` entry
- 接收端本地也已经有可消费的 `RQ` entry

这不意味着发送端要通过网络“发一个 post 包”去改对端状态。

更合理的仿真口径是：

- 控制接收路径长期具备 `RQ` credit
- 或由更上层协议在更早阶段协调好接收端 `RQ`

## 2.3 QP 结构

QP 在目标设计中不再只是状态和 PSN 容器，而应显式包含：

1. `SQ`
2. `RQ`
3. `CQ`

当前讨论口径中：

- `SQ`
  - 保存本地待发的 `SEND/WRITE` 请求
- `RQ`
  - 保存本地已贴好的接收项，供 `SEND` 消耗
- `CQ`
  - 保存发送/接收完成项，供上层轮询或回调

## 2.4 MTU 与字节口径

这里已经确定两套不同的字节模型。

### legacy 模式

`datasize` 的含义是：

- `以太网 payload 总预算`

当前 `FlowTagHeader / ThreePHeader / HopCountHeader` 这类仿真控制内容，允许继续隐藏在业务区里。

### RoCE 模式

RoCE 头是真实存在的，不再允许像 legacy 那样完全隐藏。

因此，对每一个 RoCE 包：

- `payloadBudgetPerPacket = MTU - visibleRoceBytes`

其中：

- `visibleRoceBytes`
  - 只统计真实可见的 RoCE transport 头
- `FlowTagHeader / ThreePHeader / HopCountHeader`
  - 仍然属于业务区内部的隐藏仿真控制内容

也就是说：

1. 分片预算时
   - 只把 `visibleRoceBytes` 当成额外开销
2. 真正构造 ns-3 `Packet` 时
   - 如果这些隐藏控制内容仍用 `Header` 写入包体
   - 它们依然要从业务区预算里扣出

因此高层公式应固定为：

```text
rocePayloadBudget = MTU - visibleRoceBytes
zeroFillBytes = rocePayloadBudget - hiddenSimBytesThisPacket
```

## 2.5 RoCE 可见头

当前讨论中，RoCE 头按下面三类看待：

1. `BTH`
   - 所有 RoCE transport 包的基础头
2. `RETH`
   - `WRITE/READ` 请求使用
3. `AETH`
   - `ACK` 使用

因此下一阶段不能再把 `RoceSimHeader::GetSerializedSize()` 直接当作标准 RoCE 线长。

需要单独引入：

- `GetVisibleRoceWireBytes(opcode)`

按包型返回标准等效大小。

## 2.6 重传与 ACK

重传责任已经重新划回 RoCE 层。

目标机制是：

1. 每包 ACK
2. 基于 ACK 的超时重传
3. 按 QP 维护未确认包状态

下一阶段至少要有：

1. `snd_nxt`
2. `snd_una`
3. outstanding packet table
4. retransmission timer
5. ACK 处理路径

## 2.7 有限乱序缓冲

当前目标不再是“接受所有乱序包并记账”，而是：

1. QP 维护有限大小的乱序窗口
2. future PSN 包先进入重排缓冲
3. 只有连续追平后才按序释放给业务层
4. 超出缓冲窗口时触发重传机制

因此乱序处理后面必须与 ACK / retransmission 闭环连起来。

## 3. 当前代码和目标设计的差距

## 3.1 `SEND` 还不完整

当前代码里：

- 发送端已经有类似 `post_send` 的雏形
  - `EnqueueRoceWork_(...)`
- 但接收端还没有真正的 `RQ`

因此当前 `SEND` 还只是 opcode 语义，不是完整 verbs 行为。

## 3.2 `SQ/RQ/CQ` 只有壳

当前项目里：

- `sqDepth/rqDepth` 只是数字
- `RoceTxScheduler` 更像外置的发送队列
- `CQ` 只有简化 completion 队列壳子，尚未接入主流程

## 3.3 RoCE 分片预算仍按原始 MTU

当前 RoCE 路径里：

- `totalPackets`
  - 仍按原始 `MTU` 计算
- `sliceBytes`
  - 也仍按原始 `MTU` 切

这与新的 RoCE 字节口径不一致。

下一阶段应改为：

- 先求 `rocePayloadBudget = MTU - visibleRoceBytes`
- 再按 `rocePayloadBudget` 分片

## 3.4 当前乱序处理还不是真正的重排缓冲

当前 `RoceQpEngine` 虽然已有：

- `pendingRxPsns`

但它现在做的是：

1. 记录 future PSN
2. 不再把简单乱序直接判错

它还没有做到：

1. 有限大小窗口
2. 延迟向业务层交付
3. 超窗重传

## 4. 推荐的实施顺序

## 4.1 Phase 2A：先修字节模型

先做下面三件事：

1. 固化 legacy / RoCE 两套 `datasize` 语义
2. 新增 `GetVisibleRoceWireBytes(opcode)`
3. 把 RoCE 分片从 `MTU` 改成 `MTU - visibleRoceBytes`

这是所有后续改动的前提。

## 4.2 Phase 2B：补最小 `SQ/RQ/CQ`

目标：

1. `SQ`
   - 与当前 scheduler 绑定
2. `RQ`
   - 为 `SEND` 提供真实资源约束
3. `CQ`
   - 为 ACK / 完成 / 上层回调提供落点

## 4.3 Phase 2C：补 ACK / 重传 / 有限乱序窗口

最后再把这些接上：

1. per-packet ACK
2. timeout retransmission
3. bounded reorder buffer
4. overflow / gap-driven retransmission

## 5. 当前阶段的推荐表述

如果现在对外介绍当前分支，建议表述为：

- 已实现 RoCE 语义接入与轻量 QP/PSN
- 已实现发送入口调度和最小乱序容忍
- 下一阶段将补 `SQ/RQ/CQ`、标准 RoCE 可见头开销、每包 ACK 和超时重传

不要表述为：

- 已完成完整 RC
- 已完成真实 SEND/WRITE verbs 行为
- 已完成标准 RoCE 头与完整重排闭环
