# RoCE 配置与日志说明

## 1. JSON 中 `roce` 配置项说明

配置路径：`simconfig/config.json` 的 `"roce"` 对象。

说明：

- 这份文档讲的是当前代码已经支持的配置和日志
- 不负责定义会议后刚收敛下来的下一阶段协议目标
- 如果你想看 `SEND/WRITE`、`SQ/RQ/CQ`、RoCE 可见头、ACK/重传、有限乱序窗口的目标设计，请看：
  - [roce-phase2-design.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-phase2-design.md)

- `enable`（bool）
  - 含义：是否启用 RoCE 旁路逻辑。
  - 影响：`true` 时发送/接收路径会附加并解析 `RoceSimHeader`，并启用 QP/FSM 语义；`false` 时走 legacy 逻辑。

- `mode`（string）
  - 含义：RoCE 语义模式名。
  - 影响：决定 3P 阶段到 RoCE opcode 的映射倾向。
  - 当前常用：`"rc_3phase"`。

- `data_use_write`（bool）
  - 含义：DATA 阶段是否默认使用 RDMA WRITE 语义。
  - 影响：在非 `send`/`read` 显式模式下，DATA 默认映射到 `RdmaWrite`。

- `sq_depth`（uint32）
  - 含义：默认 QP Send Queue 深度。
  - 影响：初始化 QP context 时用于发送资源上限。

- `rq_depth`（uint32）
  - 含义：默认 QP Receive Queue 深度。
  - 影响：初始化 QP context 时用于接收资源上限。

- `ether_type`（string/number）
  - 含义：RoCE 报文使用的二层 EtherType，可写 `"0x9999"` 或数字。
  - 影响：应用初始化时会按这个协议号绑定收发。

- `transport_ack_enable`（bool）
  - 含义：是否启用 transport ACK、timeout 和 retransmission。
  - 影响：关闭后，RoCE 仍保留 QP/调度语义，但不会进入当前这套 ACK/重传闭环。

- `transport_window_packets`（uint32）
  - 含义：每个 QP 同时允许存在的 transport outstanding packet 数上限。
  - 影响：这是当前最重要的发送 admission control 之一。达到上限后，该 QP 会暂时停止发送新的普通 packet，直到 ACK 回来释放窗口。

- `reorder_window_packets`（uint32）
  - 含义：每个 QP 接收侧允许缓存的 future packet 乱序窗口大小，单位是 packet。
  - 影响：
    - 只接受区间 `[expectedRxPsn, expectedRxPsn + reorder_window_packets)` 内的 future packet 进入 reorder buffer。
    - 超出窗口的 future packet 不再缓存，不上送业务层，而是继续只回累计 ACK，等待发送端 timeout/recovery 补洞。
  - 当前约束：
    - `1 <= reorder_window_packets <= transport_window_packets`

- `ack_timeout_mode`（string）
  - 含义：ACK timeout 的来源。
  - 推荐写法：`"auto"`、`"manual"`。
  - 影响：
    - `manual`：直接使用 `ack_timeout_ns`
    - `auto`：根据拓扑、链路速率、MTU 和 switch delay 推导一个最坏 RTT 基线，再乘系数

- `ack_timeout_factor`（double）
  - 含义：自动 timeout 模式下，乘在理论 RTT 基线上的放大系数。
  - 影响：最终 `ack_timeout_ns = ceil(ackTimeoutBaseNs * ack_timeout_factor)`。

- `ack_timeout_ns`（uint64）
  - 含义：手工指定的 ACK timeout，单位 ns。
  - 影响：仅在 `ack_timeout_mode=manual` 时直接生效。

- `max_retransmissions`（uint32）
  - 含义：单个 transport packet 最大允许重传次数。
  - 影响：超过这个上限后，该 packet 会被判定为 retransmission drop。

- `log_level`（string/int）
  - 含义：RoCE 信息日志等级。
  - 推荐写法：`"full"`、`"basic"`、`"off"`。
  - 兼容写法：`2/1/0`。
  - 影响：
    - `full`：完整输出配置、激活、入队、逐 slice 发送等调试日志。
    - `basic`：只输出配置、激活和入队日志，足够看流向与阶段。
    - `off`：关闭 RoCE 信息日志。
  - 注意：`WARN/FATAL` 级别的异常日志不受这个开关影响，仍然会打印。

## 2. 旧字段说明

- `strict_completion`
  - 状态：已废弃，不再建议放进新的 JSON。
  - 原因：当前版本里 `ACT` 已恢复为纯业务控制报文，不再承担 completion/可靠 ACK 语义。

- `workload.auto_stop_on_finish`
  - 状态：当前代码未启用。
  - 原因：之前试过该机制，但已经回滚，保留在 JSON 里不会生效。

## 3. 日志输出含义

### 3.1 配置与激活日志

示例：

```text
[ROCE][CFG] enable=true mode=rc_3phase dataUseWrite=true etherType=0x9999 logLevel=full transportAck=true transportWindowPackets=32 reorderWindowPackets=32 ackTimeoutMode=auto ackTimeoutBaseNs=2824 ackTimeoutFactor=2 ackTimeoutNs=5648 maxRetrans=8 reqOp=0 dataOp=2 actOp=0
[ROCE][ACTIVE] node=20 mode=rc_3phase dataUseWrite=true sqDepth=1024 rqDepth=1024 etherType=0x9999 logLevel=full transportAck=true transportWindowPackets=32 reorderWindowPackets=32 ackTimeoutMode=auto ackTimeoutBaseNs=2824 ackTimeoutFactor=2 ackTimeoutNs=5648 maxRetrans=8
```

字段含义：

- `enable`
  - 是否启用 RoCE。

- `mode`
  - 当前模式名。

- `dataUseWrite`
  - DATA 阶段是否偏向 `WRITE`。

- `etherType`
  - 当前二层协议号。

- `logLevel`
  - 当前 RoCE 信息日志等级。

- `reqOp/dataOp/actOp`
  - 三个 3P 阶段映射到的 opcode。
  - 当前 `ACT` 应为 `Send`，因为它是业务控制报文。

- `transportAck`
  - 是否启用 transport ACK / timeout / retransmission。

- `transportWindowPackets`
  - 当前每个 QP 的 in-flight packet 上限。
  - 这个值现在会明显影响发送节奏，甚至可能比设备队列长度更主导 offered load。

- `reorderWindowPackets`
  - 当前每个 QP 接收侧允许缓存的 future packet 窗口。
  - 它和 `transportWindowPackets` 一起决定 sender/receiver 两端对“最大离散程度”的共同上界。

- `ackTimeoutMode`
  - `manual` 或 `auto`。

- `ackTimeoutBaseNs`
  - 自动模式下，根据拓扑和链路参数算出的未乘系数 RTT 基线。

- `ackTimeoutFactor`
  - 自动模式下的放大系数。

- `ackTimeoutNs`
  - 当前实际使用的 timeout。

- `maxRetrans`
  - 单包最大重传次数。

### 3.2 调度器日志

#### `[ROCE][SCHED][ENQ]`

示例：

```text
[ROCE][SCHED][ENQ] workId=5 state=1 tag=1 src=0 dst=1 qpn=5 totalBytes=16384 totalPackets=4 class=1
```

含义：

- 说明一个发送工作已经进入 QP / RoCE 调度器。
- `state`
  - `0`：REQUEST
  - `1`：DATA
  - `2`：ACT
- `src/dst`
  - 本条流的发送端和接收端。
- `qpn`
  - 该 flow 当前绑定的 QP 编号。
- `totalPackets`
  - 该工作最终会被拆成多少个 slice。
- `class`
  - `0`：控制类
  - `1`：数据类

这是 `basic` 级别最核心的日志，主要用于看：

- 流方向
- 三阶段顺序
- 哪些 flow 挂在哪个 QP 上

#### `[ROCE][SCHED][TX]`

示例：

```text
[ROCE][SCHED][TX] workId=5 state=1 tag=1 src=0 dst=1 qpn=5 packetIndex=2 sliceBytes=4096 totalPackets=4 last=false class=1
```

含义：

- 说明调度器真正放出了一个 packet slice。
- `packetIndex`
  - 这是该 work 的第几个 slice。
- `sliceBytes`
  - 该次发送的 slice 字节数。
- `last`
  - 是否为该 work 的最后一个 slice。

这是 `full` 级别才会输出的日志，主要用于看：

- 多活跃 QP 是否在轮转
- 一个 DATA work 是否被逐 slice 调度
- 控制报文是否插到数据报文前面

### 3.3 transport ACK / retransmission 日志

下面这几类都只在 `full` 级别输出。

#### `[ROCE][ACK][TX]`

含义：

- 接收侧刚刚发出了一个 transport ACK。
- 它是协议 ACK，不是业务 `ACT`。

#### `[ROCE][ACK][RX]`

含义：

- 发送侧收到了一个 transport ACK。
- `ackPsn` 表示累计确认边界。
- 这条日志出现时，发送侧会清掉 `ackPsn` 及之前的 outstanding，并尝试继续调度新的普通 packet。

#### `[ROCE][RTX][TIMEOUT]`

含义：

- 某个 QP 的 oldest outstanding packet 超时。

#### `[ROCE][RX][REORDER_OVERFLOW]`

含义：

- 接收侧发现某个 future packet 超出了当前 `reorder_window_packets`。
- 这个 packet 不会进入 reorder buffer，也不会上送业务层。
- 当前处理策略是：
  - 保持累计 ACK 在 `expectedRxPsn - 1`
  - 依赖发送侧 timeout/recovery 来补回缺失洞
- 当前实现会让该 QP 进入 recovery，并阻塞新的普通 packet。

#### `[ROCE][RTX][SEND]`

含义：

- 当前有一个待发 retransmission 通过统一 dispatch 路径发出。

#### `[ROCE][RTX][DROP]`

含义：

- 某个 packet 超过 `max_retransmissions` 上限，被放弃。

### 3.4 事务流日志

这几类日志不受 `roce.log_level` 控制。

原因是：

- 它们描述的是业务事务何时触发、何时收尾
- 不是 RoCE 协议内部调试信息

#### `[FLOW][REQ_DONE]`

示例：

```text
[FLOW][REQ_DONE] {tag,src,dst}= 5 0 1 workId=18 TIME=500000000000000000
```

含义：

- `REQUEST` 这类控制 work 的最后一个 slice 已经真正发出
- 对应的发送回调即将被触发

#### `[FLOW][RX_DONE]`

示例：

```text
[FLOW][RX_DONE] {tag,src,dst}= 5 0 1 dataPackets=400/400 dataBytes=1638400 TIME=500000000000000000
```

含义：

- 接收侧已经收齐该 flow 的全部 DATA 分片
- 接收侧会在这个时间点附近生成 `ACT`
- 若上层已经登记接收回调，也会在这里附近回调 Astra-Sim

#### `[FLOW][TX_DONE]`

示例：

```text
[FLOW][TX_DONE] {tag,src,dst}= 5 0 1 actPackets=1/1 TIME=500000000000000000
```

含义：

- 发送侧已经收到该 flow 的最终 `ACT`
- 该 flow 的发送完成回调会在这里触发

#### `TriggerSendTo_now declared now time = ...`

示例：

```text
TriggerSendTo_now declared now time = 500000000123456789 src=0 dst=1 tag=5 size=1638400
```

含义：

- Astra-Sim / `my_entry` 已经把一条点到点事务下放到 `sendflowclient`
- 这是事务进入发送路径的最早观察点

这几类事务流日志主要用于看：

- 一个 flow 什么时候真正收尾
- 为什么 log 尾部只有大量 `[ROCE][SCHED][TX]` 而看不到事务完成信息
- 某个 flow 是否停在“数据已发出但还没收到 ACT”的阶段

### 3.5 异常日志

- `[ROCE][TX] ...`
  - 发送侧构头失败、或 QP 状态异常时打印。
  - 当前版本里，如果 `EnsureQpReadyToSend()` 后 QP 仍不可发，会直接报错退出。

- `[ROCE][RX] ...`
  - 接收侧校验 RoCE 头时发现异常时打印，例如：
    - QP 拒收
    - phase 不一致
    - opcode 不一致
    - 其他真正协议错误

- `[ROCE][QPN] ...`
  - flow 三元组和收到的 `qpn` 映射不一致时打印。

## 4. 推荐的日志等级用法

- `full`
  - 用于调试调度器、QP 和逐 slice 行为。
  - 也用于调试 transport ACK / timeout / retransmission。
  - 当前阶段要验证多活跃 QP 的 RR，建议先用这个等级。

- `basic`
  - 用于日常观察业务流方向。
  - 日志量明显小于 `full`，但仍能看出 `REQUEST -> DATA -> ACT` 和 `src/dst/qpn`。
  - 当前不会打印包级 ACK / retransmission。

- `off`
  - 用于大规模跑 workload，不看 RoCE 信息日志时使用。

## 5. 一个候选的多活跃 QP 观察 workload

文件：

- `simworkload/roceMultiQpRR.txt`

设计目的：

- 用 `ALLTOALL_EP` 在同一个 sender 上制造多目的端发送。
- 更容易让同一 sender 同时挂多个活跃 QP，从而观察数据类 RR。

但需要注意：

### 5.1 第一次失败现象

在一次早期测试中，这个 workload 使用了：

- `model_parallel_NPU_group: 2`
- `ep: 16`

终端会提示：

```text
mock_nccl_grobal_group_init: DP_size=8 is not divisible by EP_size=16. Disabling EP (setting EP_size=1).
mock_nccl_comms_init: DP_size=8 is not divisible by EP_size=16. Disabling EP (setting EP_size=1).
```

对应现象是：

- `simulation.log` 只有 `[ROCE][CFG]`
- 没有 `[ROCE][ACTIVE] / [ROCE][SCHED][ENQ] / [ROCE][SCHED][TX]`
- `[main] total send count=0`

这说明：

- 不是 RoCE scheduler 出队有问题
- 而是 workload 的 `EP` 设置本身不合法，导致通信路径根本没有被下发

### 5.2 修正为 `ep: 4` 后的新现象

把 `ep` 改成 `4` 之后，这个 workload 能正常触发实际发送：

- 有大量 `TriggerSendTo_now`
- 有 `[ROCE][ACTIVE]`
- 有 `[ROCE][SCHED][ENQ]`
- 有 `[ROCE][SCHED][TX]`
- `[main] total send count=48`

但当前日志同时说明两件事：

1. **多目的端 QP 的创建是成功的**
   - 同一个 `src` 会先建多个 `REQUEST` QP
   - 后续也会出现多个 `DATA` QP

2. **数据类 RR 仍未被真正打出来**
   - 以 `src=0` 为例：
     - 先完整发完 `qpn=4` 的 64 个 slice
     - 再发 `qpn=5`
     - 再发 `qpn=6`
   - 这说明当前这次运行里，虽然同一 sender 最终确实产生了多个数据 QP，但这些 data work 不是同时活跃进入 scheduler 的

此外还有一个明显问题：

- 日志里出现了大量：
  - `[ROCE][RX] QP rejected packet ...`

这在旧版代码里通常说明轻量 `RoceQpEngine` 的严格按序接收检查被频繁触发了。
当前版本已经把这部分收敛成更稳妥的行为：

- `future PSN` 会先进入 `pendingRxPsns`
- 旧包/重复包只计数，不再直接把 QP 置 `Err`
- 只有真正的协议错误（非法状态、非法 opcode、非法长度）才继续触发 `QP rejected`

因此当前对这个 workload 的最新结论应当是：

- 它已经能用于验证“RoCE 路径真实激活、多 QP 被创建”
- 但它还**没有**成功验证“多活跃 QP 的数据 RR”
- 同时它提示接收侧后续仍需要补 ACK/重传一类更完整的 RC 语义

### 5.3 修正接收侧乱序后的复测结果

在接收侧加入最小乱序容忍后，再次运行同一组：

- `simworkload/roceMultiQpRR.txt`
- `ep: 4`

本次复测的关键信号是：

- `[main] total send count = 48`
- `[main] total events = 54439`
- `QP rejected = 0`

这说明：

1. workload 仍然真实下发了发送
2. 之前大量 `QP rejected` 的问题已经收敛
3. 当前剩下的主要问题不再是“接收侧把乱序误判成错误”，而是“多活跃 DATA QP 的 RR 仍没有被打出来”

排查时可以先看：

1. `simulation.log` 里是否出现 `TriggerSendTo_now`
2. 是否出现 `[ROCE][ACTIVE]`
3. `[main] total send count` 是否大于 `0`

只有这些条件满足后，才有意义进一步看 `[ROCE][SCHED][TX]` 的 RR 行为。

配套建议：

- `simconfig/config.json` 里把 `workloadpath` 切到 `simworkload/roceMultiQpRR.txt`
- `roce.log_level` 设为 `"full"`

如果该 workload 真正生效，`[ROCE][SCHED][TX]` 里应该能看到：

- 同一个 `src`
- 不同 `qpn/workId`
- `packetIndex` 交替出现

## 6. 配置是如何进入代码的

主要入口在两个地方：

1. `scratch/my_common.h`
   - 解析 JSON 的 `roce` 字段
   - 写入全局 `g_roce_cfg`

2. `src/min-hop-mac-learning/model/my_sendflowclient.cc`
   - `sendflowclient::SetRoceConfig(...)`
   - 把配置灌入 `RoceQpEngine`、`RoceThreePhaseFsm` 和 `RoceTxScheduler`

## 7. 当前语义边界

这几点仍然很重要：

1. `ACT` 是业务控制报文，不是 RC 传输 ACK。
2. `RoCE enable` 不等于“已经完成了完整 RC 可靠传输”。
3. 当前版本已经让 RoCE 开始接管发送入口和调度顺序，但还不是完整 RC transport。
