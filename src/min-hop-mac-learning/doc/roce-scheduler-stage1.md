# RoCE Scheduler Stage 1

这份文档只讲当前阶段 1 的一件事：

- RoCE 包在 `sendflowclient` 里是怎么入队的
- 入队之后又是怎么被选出来并发出去的

如果你只想知道“当前版本停在哪”，先看：

- [roce-current-status.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-current-status.md)

## 1. 当前阶段的边界

当前 scheduler 已经负责：

1. RoCE 开启时的发送入口接管
2. per-QP work queue
3. 控制类优先
4. 数据类按活跃 QP 轮询
5. DATA 逐 slice 出网

当前 scheduler 还不负责：

1. transport ACK 驱动推进
2. inflight/send window
3. retransmission / timeout
4. 设备发送完成驱动的下层调度
5. CQ 驱动发送推进

另外要注意：

- 当前文档描述的是 stage 1 的实际实现
- 不等于会议后已经确定的最终协议目标
- 关于 `SEND/WRITE`、`SQ/RQ/CQ`、可见 RoCE 头、ACK/重传、有限乱序窗口，请看：
  - [roce-phase2-design.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-phase2-design.md)

## 2. 入队发生在什么时候

发送入口在：

- `sendflowclient::TriggerSendTo_now(...)`

当前逻辑分两条：

- `RoCE disable`
  - 仍走 legacy `SendPacket(...)`

- `RoCE enable`
  - 不直接发包
  - 改为调用 `EnqueueRoceWork_(...)`

因此当前阶段的第一步不是“立即发 REQUEST/DATA/ACT”，而是“先创建 work item”。

## 3. work item 长什么样

调度器里管理的基本单位是 `RoceTxWorkItem`，定义在：

- `src/min-hop-mac-learning/model/roce-tx-scheduler.h`

每个 work item 对应一次业务发送请求，不等于一个 packet。

它包含的核心信息有：

1. `workId`
   - 这次发送工作的编号

2. `state`
   - `REQUEST / DATA / ACT`

3. `flowKey`
   - `{tag, src, dst}`

4. `qpKey`
   - `{src, dst, qpn}`

5. `totalBytes / remainingBytes`
   - 整笔 work 的总大小和剩余待发大小

6. `packetIndex / totalPackets`
   - 当前已经切到第几个 slice，以及总共会切多少个 slice

7. `txClass`
   - `Control` 或 `Data`

## 4. 入队时怎么分类

当前分类规则很简单：

1. `REQUEST`
   - 控制类

2. `ACT`
   - 控制类

3. `DATA`
   - 数据类

所以在 log 里你会看到：

- `class=0`
  - 控制类
- `class=1`
  - 数据类

## 5. 队列结构是什么

调度器内部维护的是：

1. `m_workQueues`
   - `map<RoceQpKey, deque<RoceTxWorkItem>>`
   - 每个 QP 一条 work queue

2. `m_controlActiveQps`
   - 当前有可发控制 work 的活跃 QP 列表

3. `m_dataActiveQps`
   - 当前有可发数据 work 的活跃 QP 列表

也就是说，当前不是“一条全局发送队列”，而是：

- 先按 QP 分开存
- 再按控制类 / 数据类维护活跃 QP 列表

## 6. 出队是怎么触发的

每次成功入队后，`sendflowclient` 会调用：

- `ScheduleRoceDispatch_()`

它只做一件事：

- 如果当前没有挂起的 dispatch event，就 `Schedule(Seconds(0), DispatchOneRoceSlice_)`

因此当前 scheduler 的运行方式是：

1. 入队触发一次 dispatch
2. 每次 dispatch 只发一个 slice
3. 如果队列里还有活，就继续调下一次 dispatch

这是一种“事件驱动的逐包出队”，不是 while 循环一次性打光。

## 7. 真正选下一个包时怎么做

`DispatchOneRoceSlice_()` 会调用：

- `RoceTxScheduler::PickNextSlice(...)`

选择顺序是：

1. 先看控制类
2. 如果控制类没有可发 work，再看数据类

也就是：

- `Control` 优先级高于 `Data`

### 7.1 控制类怎么选

当前控制类也按活跃 QP 列表走，但因为 REQUEST/ACT 通常是单包，所以看起来更像“先到先发”。

### 7.2 数据类怎么选

数据类是按活跃 QP 轮询的：

1. 从 `m_dataActiveQps` 头部取一个 `qpKey`
2. 找到这个 QP 对应队列的队首 work
3. 切出一个 slice
4. 如果 work 还没发完，就把这个 `qpKey` 放回活跃列表尾部
5. 下次轮到别的活跃 QP

这就是当前阶段 1 的 RR 核心。

## 8. 一个 DATA work 如何被切片

对 `DATA`：

1. 入队时先记录：
   - `totalBytes`
   - `remainingBytes`
   - `totalPackets`

2. 出队时根据 `maxSliceBytes` 切一个 slice
   - 当前 stage 1 一般就是设备原始 `MTU`
   - 这只是当前实现，不是最新冻结下来的 RoCE 字节口径

3. 每发一个 slice：
   - `remainingBytes -= sliceBytes`
   - `packetIndex += 1`

4. 当 `remainingBytes == 0`：
   - 这个 work 从队首弹出
   - 调度器再看该 QP 后面还有没有下一个 work

## 9. 当前日志和出队逻辑的对应关系

### 9.1 入队日志

```text
[ROCE][SCHED][ENQ] ...
```

表示：

- 一个 work item 已经成功进入 per-QP queue

### 9.2 出队日志

```text
[ROCE][SCHED][TX] ...
```

表示：

- 调度器选中了一个 slice
- 这个 slice 已进入真正的构包和发送路径

关键字段：

1. `workId`
   - 哪个 work

2. `qpn`
   - 属于哪条 QP

3. `packetIndex`
   - 这是第几个 slice

4. `last`
   - 是否是这个 work 的最后一个 slice

## 10. 当前阶段为什么还看不到“完整 transport 行为”

虽然现在已经有了入队和出队，但还不能把它说成完整 RC transport，原因是：

1. 出队不受 ACK / `snd_una` 约束
2. 没有 inflight window
3. 没有 timeout / retransmission
4. QP 目前只负责：
   - 状态
   - PSN
   - header 语义
   - 调度入口

所以当前最准确的表述是：

- **RoCE enable 时，发送顺序已经开始由 QP/scheduler 控制**
- **但发送推进仍不是完整 RC 可靠传输闭环**

## 11. 当前两次真实运行的结论

以 `simworkload/roceMultiQpRR.txt` 且 `ep: 4` 的两次真实运行为例，当前已经能确认下面几件事：

1. 同一个 sender 确实会创建多个 `REQUEST` QP
2. 同一个 sender 后续也确实会创建多个 `DATA` QP
3. 调度器路径已经生效
   - 这点能从 `[ROCE][SCHED][ENQ/TX]` 直接确认

但这两次运行也说明：

- **多活跃 QP 的数据 RR 仍未在真实 workload 中被验证**
- 原因不是 RR 逻辑不存在，而是 data work 进入 scheduler 的时机仍然偏串行

### 11.1 第一次运行暴露的问题

第一次有效运行中，日志里出现了大量：

- `[ROCE][RX] QP rejected packet ...`

这说明旧版 `RoceQpEngine` 的接收检查过于严格，会把乱序误判成协议错误。

### 11.2 修正乱序后的复测结果

在接收侧加入最小乱序容忍后，再次运行相同 workload：

- `QP rejected` 已经收敛到 `0`
- `total send count` 仍然保持正常

这说明当前剩下的主要问题已经不是“接收侧乱序误判”，而是“多活跃 DATA QP 的 RR 还没有在线路节拍上被打出来”。

因此，这一阶段最稳妥的对外说法仍然是：

- **RoCE 已接管发送入口和基本调度顺序**
- **接收侧最小乱序容忍已经补上**
- **但发送推进和多 QP 线速仲裁仍不是完整 RC**

## 12. 延期方案

关于后续两类调度改造方案：

1. 基于真实发送完成事件的 pull 调度
2. 基于 per-slice pacing 的折中方案

都已单独记录在：

- [roce-future-scheduling-options.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-future-scheduling-options.md)

当前决定是：

- 先把当前版本停在 stage 1
- 暂不继续深挖调度层
