# RoCE 配置与日志说明

最近修改：2026-05-19

## 1. JSON 中 `roce` 配置项说明

配置路径：`simconfig/config.json` 的 `"roce"` 对象。

说明：

- 这份文档讲的是当前代码已经支持的配置和日志。
- 如果你想看当前 `SEND/WRITE`、`SQ/RQ/CQ`、RoCE 可见头、ACK/重传、有限乱序窗口落在哪里，请看：
  - [RoCE当前状态.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/RoCE当前状态.md)
  - [RoCE工作流.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/RoCE工作流.md)
  - [RoCE传输可靠性.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/RoCE传输可靠性.md)
- 如果你想看当前 GPU 端点的 inner/external bridge、`TerminalLearningOnly`、FlowIo/PFC 边界，请看：
  - [拓扑端点网桥模型.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/拓扑端点网桥模型.md)

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
  - 当前经验：这个值强依赖拓扑和 workload。近期 128 AllToAll 排查为了避免 ACK timeout 干扰队列分析，使用过很大的安全系数；不要把旧文档里的单个经验值当作通用推荐。

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
    - `full`：输出 `basic` 的全部内容，并额外输出 ACK / reorder 的细粒度调试日志。
    - `basic`：输出配置激活与关键可靠性事件（如 `RTO` / `RTX_SEND`）。
    - `off`：关闭 RoCE 信息日志。
  - 注意：`WARN/FATAL` 级别的异常日志不受这个开关影响，仍然会打印。

## 2. 旧字段说明

- `strict_completion`
  - 状态：已废弃，不再建议放进新的 JSON。
  - 原因：当前版本里 `ACT` 已恢复为纯业务控制报文，不再承担 completion/可靠 ACK 语义。

- `workload.auto_stop_on_finish`
  - 状态：当前代码未启用。
  - 原因：之前试过该机制，但已经回滚，保留在 JSON 里不会生效。

## 2A. PPP 队列与 PFC 相关配置

这些字段在 JSON 里位于 `"pfc"`，但会直接影响 RoCE 端口发送队列和 PFC 行为。

- `drop_tail_queue_num`
  - 含义：`PppRoundRobinQueue` 内部子队列数量。
  - 当前实际影响：
    - 物理发送队列会按该数量创建 RR 子队列。
    - `RdmaPortEngine` 的 qid/pause/admission 也按这个数量组织。
    - bridge ingress aggregate queue 容量会用 `NumQueues * MaxPacketsPerQueue` 计算。
  - 当前默认分类：DATA 包按 PPP 目的 MAC hash 后 `% NumQueues`。
  - 风险：如果目的 MAC 地址规律和队列数取模形成别名碰撞，多个 flow 会落到同一个 qid，RR 退化成该子队列内 FIFO。
  - 最近 128 AllToAll 观察：
    - `NumQueues=16` 时出现严重结构性碰撞，尾部约 `18.491us`。
    - `NumQueues=13` 时碰撞被打散，尾部约 `12.108us`。
  - 结论：改成 13 是有效实验 workaround，但长期应考虑 flow-aware qid classifier，而不是依赖特定队列数。

- `drop_tail_queue_size`
  - 含义：每个 PPP 子队列的容量。
  - 当前 bridge ingress aggregate queue 的容量会按该值乘以 `drop_tail_queue_num` 派生。

- `high_watermark` / `low_watermark`
  - 含义：PFC pause/resume 水位。
  - 在 bridge ingress 上按 aggregate occupancy 判断；在物理口上按对应 qid/subqueue 状态判断。

- `not_head_retry_ns`
  - 状态：兼容旧配置保留。
  - 当前 bridge ingress 已经是 token-based drain/retry，不再使用旧的 not-head retry 队首模型。

## 3. 日志输出含义

### 3.0 主程序收尾时间日志

当前 `scratch/my_AstraSimNetwork.cc` 在 `Simulator::Run()` 返回后会额外输出两条主程序级时间：

```text
[main] network drained abs_time(ns)=... abs_time(us)=... abs_time(ms)=... since_workload_start(ns)=... since_workload_start(us)=... since_workload_start(ms)=...
[main] last FLOW_TX_DONE abs_time(ns)=... abs_time(us)=... abs_time(ms)=... since_workload_start(ns)=... since_workload_start(us)=... since_workload_start(ms)=...
```

这两条和 Astra-Sim 终端里的：

```text
all passes finished at time: ...
```

不是一个口径。

- `network drained`
  - 不是 `Simulator::Run()` 的最终退出时刻。
  - 当前定义为 `sendflowclient` 视角下最后一次真实网络活动时间。
  - 它会在真实 `realsend()` 和 `HandleRead()` 上更新，因此更接近“网络侧最后一个包活动结束”的时刻。

- `last FLOW_TX_DONE`
  - 当前定义为最后一个 `[FLOW][TX_DONE]` 发生的时间。
  - 它更接近业务事务全部闭环的时刻。

- `since_workload_start`
  - 统一以 Astra-Sim workload `fire()` 的时间作为零点。
  - 当前默认对应 `warmupSeconds`，也就是常见配置里 `floadtime_s` 之后的业务开始时刻。

因此后面看时间时，建议这么分：

- Astra-Sim 的 `all passes finished`
  - workload / stream 层口径
- `[main] last FLOW_TX_DONE`
  - 3P 事务层口径
- `[main] network drained`
  - 网络活动口径

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

### 3.2 transport ACK / reorder / retransmission 日志（当前实现）

当前代码中，和 transport 可靠性闭环直接相关的日志如下：

#### `[ROCE][ACK][TX]`（`full`）

含义：

- 接收侧发出了一个 transport ACK（协议 ACK，不是业务 `ACT`）。
- 可用于观察累计确认点推进（`ackPsn`）和接收侧状态（`expectedRxPsn`、`pendingRxCount`）。

#### `[ROCE][RX][REORDER_BUFFER]`（`full`）

含义：

- 某个 future packet 进入了接收侧 reorder buffer。
- 可用于观察有界乱序窗口内的缓存行为。

#### `[ROCE][RX][REORDER_RELEASE]`（`full`）

含义：

- reorder buffer 中连续可交付的数据被释放并上送。

#### `[ROCE][RX][REORDER_OVERFLOW]`（`WARN`，不受 `roce.log_level` 控制）

含义：

- 收到的 future packet 超出 `reorder_window_packets`，不会进入 reorder buffer。
- 当前策略是保持累计 ACK 并依赖发送侧 timeout/recovery 补洞。

#### `[ROCE][RTO]`（`basic/full`）

含义：

- outstanding 最老包触发超时，进入 recovery 处理。
- 这是当前代码里 timeout 日志前缀（不是旧前缀 `[ROCE][RTX][TIMEOUT]`）。

#### `[ROCE][RTX][SEND]`（`basic/full`）

含义：

- 某个重传包真正发出。

### 3.2A bridge / 端点相关日志

下面这些日志不属于 RoCE transport 内部状态机，但在当前端点双 bridge 模型里经常需要一起看。

#### `[BRIDGE][RETRY][NO_PATH]`

含义：

- 普通 forwarding bridge 的某个 ingress 包在完成 `switch_delay` 后，当前没有可用出路。
- bridge 会保留该包并安排 ingress drain retry，而不是立刻丢弃。
- 当前 `TerminalLearningOnly` 的修正点之一，就是终端外侧 bridge 上的 transit 包不再走这条 retry 链。

当前实现说明：

- bridge ingress 不再使用按 qid 拆分的 `PppRoundRobinQueue` 作为缓冲。
- 每个 ingress port 只有一个 aggregate queue，容量等于对应物理 egress queue 的 `NumQueues * MaxPacketsPerQueue`。
- 每个包入队后分配一个独立 token，并按自己的 token 在 `switch_delay` 后 drain；不会因为其他 qid 的队首包而额外等待。
- `pfc.not_head_retry_ns` / `NotHeadRetryDelay` 目前仅为兼容旧配置保留，不再参与 ingress 调度。
- `logging.bridge_diag` / `BridgeDiagEnable` 目前也仅为兼容旧配置保留，不会输出旧的 bridge residence 诊断。

#### `[BRIDGE][TERMINAL][DROP]`

含义：

- 终端外侧 bridge 已经利用收到的 transit 包完成必要学习。
- 但该 bridge 不承担后续 transit 转发，因此在学习后直接停止转发。

当前说明：

- 代码里这条日志模板仍然保留在 terminal-drop 分支。
- 默认 `NS_LOG_INFO` 输出已注释，避免大规模 flood 学习阶段日志爆炸。
- 因此看到它，说明 terminal 分支在工作；看不到它，不代表 terminal 分支不存在。

#### `[PFC][THRESH][LOCAL][PAUSE/RESUME]`

含义：

- bridge 检测到本地注入队列达到高/低水位。
- 当前动作是通过 `FlowIo::PauseNodeQueue(...)` / `ResumeNodeQueue(...)` 控制本地软件发送队列。
- 阈值按 aggregate ingress queue 计算；触发后会对该 bridge 配置的所有上游 qid 执行 pause/resume。

#### `[PFC][THRESH][NONLOCAL][PAUSE/RESUME]`

含义：

- bridge 检测到来自真实 ingress port 的队列达到高/低水位。
- 当前动作是向上游物理口回发 `PauseHeader`。
- 阈值同样按 aggregate ingress queue 计算；触发后会向上游物理口的各 qid 发送 pause/resume。

### 3.3 事务流日志

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
- 为什么收尾阶段看起来“网络还在动但事务是否已经闭环”
- 某个 flow 是否停在“数据已发出但还没收到 ACT”的阶段

### 3.4 PPP / PFC / 端口队列排查口径

当前默认不保留临时逐包 instrumentation。若需要证明某个 flow 实际落到哪个 PPP qid，应临时加窄范围日志，跑完后删除。

不加临时日志时，可以先用这些现象判断是否存在队列分类问题：

1. `TriggerSendTo_now` 的 DATA 触发范围是否基本不变。
2. `FLOW_RX_DONE/FLOW_TX_DONE` 的全局 spread 是否随 `drop_tail_queue_num` 大幅变化。
3. 日志中是否没有 `DROP/PFC/RETRANS/TIMEOUT/NO_PATH/SQ_FULL/RQ_FULL`。
4. 如果 1-3 同时成立，优先怀疑 PPP qid 分类和子队列 FIFO 退化，而不是上层触发顺序或重传。

近期 128 AllToAll 就属于这个模式：DATA 触发不变，但 16 队列到 13 队列后尾部显著下降。

### 3.5 异常 / 告警日志（当前实现）

#### 3.5.1 `WARN` 类（不受 `roce.log_level` 控制）

- `[ROCE][SQ][FULL]`
  - SQ 深度用尽，PostSend 拒绝该 work。
- `[ROCE][RQ][FULL] packet rejected before rx header`
  - RQ 已满，包在进入 QP 头处理前被拒收。
- `[ROCE][RQ][FULL] buffered packet rejected`
  - 需要进 reorder buffer 的包在占用 RQ 条目时失败。
- `[ROCE][RQ][FULL] packet rejected`
  - 正常上送路径占用 RQ 条目失败。
- `[ROCE][RX] QP rejected packet`
  - QP 引擎拒绝该包（状态/语义不满足）。
- `[ROCE][RX][REORDER_OVERFLOW]`
  - future packet 超出乱序窗口。
- `[ROCE][PORT][BUILD_FAIL]`
  - scheduler 选中的 slice 构包失败。
- `[ROCE][ACK][TX] failed to build ack`
  - ACK 包构建失败。
- `[ROCE][CQ][OVERFLOW]`
  - CQ 深度溢出，最老 completion 被挤掉。
- `[ROCE][SQ][COMPLETE_MISS]`
  - SQ 为空但收到完成通知。
- `[ROCE][SQ][COMPLETE_UNKNOWN]`
  - SQ 非空但找不到对应 `workId`。
- `[ROCE][SCHED] request callback missing`
  - REQUEST 收尾时未找到对应 send callback。
- `[ROCE][RX] missing/invalid RoCE header, fallback legacy parse`
  - 开启 RoCE 时收到无效头，回落 legacy 解析。

#### 3.5.2 `FATAL` 类（直接终止）

- `[ROCE][RX] ingress port engine missing`
- `[ROCE][SCHED] missing transport engine`
- `[ROCE][SQ] failed to prepare work`
- `[ROCE][SQ] failed to resolve tx port`
- `[ROCE][SQ] failed to post work`

## 4. 推荐的日志等级用法

- `full`
  - 输出 `basic` 的全部内容。
  - 额外输出 `ACK_TX`、`REORDER_BUFFER/REORDER_RELEASE` 等细粒度日志。
  - 适合定位 reorder / timeout / retransmission 细节。

- `basic`
  - 输出配置激活与关键可靠性事件（如 `[ROCE][RTO]`、`[ROCE][RTX][SEND]`）。
  - 适合日常排障，日志量明显小于 `full`。

- `off`
  - 用于大规模跑 workload，不看 RoCE 信息日志时使用。
  - 注意：`WARN/FATAL` 仍会输出；事务流日志是否可见还受 ns-3 组件开关影响。

### 4.1 与 `logging.ns3_components` 的关系

- `roce.log_level` 只控制 RoCE 的信息日志（`INFO`）子集。
- `NS_LOG_WARN/NS_LOG_UNCOND/std::cout` 不受 `roce.log_level` 控制。
- `SendFlowClient` / `RdmaPortEngine` 组件若未启用 `LOG_LEVEL_INFO`，对应 `INFO` 日志也不会出现在 `simulation.log`。

## 5. 当前排查建议

排查时建议按下面顺序看：

1. 当前配置指向的 `simlog/*.log` 里是否出现 `TriggerSendTo_now`。
2. 是否出现 `[ROCE][ACTIVE]`。
3. `[main] total send count` 是否大于 `0`。
4. 是否出现 `[FLOW][REQ_DONE] / [FLOW][RX_DONE] / [FLOW][TX_DONE]` 完整闭环。
5. 若怀疑乱序窗口或恢复机制问题，再看 `[ROCE][RX][REORDER_OVERFLOW]`、`[ROCE][RTO]`、`[ROCE][RTX][SEND]`。

## 6. 配置是如何进入代码的

主要入口在两个地方：

1. `scratch/my_common.h`
   - 解析 JSON 的 `roce` 字段
   - 写入全局 `g_roce_cfg`

2. `src/min-hop-mac-learning/model/my_sendflowclient.cc`
   - `sendflowclient::SetRoceConfig(...)`
   - 配置 `RoceTransportEngine`
   - 调用 `InitializeRdmaPortEngines_()`，把配置灌入每个 `PointToPointNetDevice` 上的 `RdmaPortEngine`

## 7. 当前语义边界

这几点仍然很重要：

1. `ACT` 是业务控制报文，不是 RC 传输 ACK。
2. `RoCE enable` 不等于“已经完成完整 RNIC/verbs/RoCEv2/DCQCN 模型”。
3. 当前 ACK、重传、reorder 和 port admission 已经在 `RdmaPortEngine` 里形成最小闭环，但仍是服务网络仿真的 `RC-like` 近似模型。
