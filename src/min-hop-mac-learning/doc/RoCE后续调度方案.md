# RoCE 后续调度方案

最近修改：2026-05-19

这份文档只记录当前仍值得讨论的调度问题。早期 `Stage 1` 调度状态已经归档，不再作为当前实现说明。

当前主路径请先看：

- [RoCE工作流.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/RoCE工作流.md)
- [RoCE当前状态.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/RoCE当前状态.md)
- [队列对与传输职责.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/队列对与传输职责.md)

历史背景见：

- [历史归档/RoCE调度第一阶段归档.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/历史归档/RoCE调度第一阶段归档.md)

## 1. 当前调度链路

当前 RoCE packet runtime 已经下沉到 `RdmaPortEngine`：

```text
RdmaPortEngine::PostSend
  -> QpRuntime::sqEntries
  -> RoceTxScheduler
  -> RdmaPortEngine::DispatchFromScheduler_
  -> RdmaPortEngine::TrySendNow_
  -> PointToPointNetDevice::SendFrom
  -> PppRoundRobinQueue::Enqueue/Dequeue
```

`DispatchFromScheduler_()` 会尽量 drain scheduler，直到遇到下面任一限制：

1. QP outstanding 达到 `transportWindowPackets`。
2. QP 正在 recovery。
3. 目标 qid pause 或 per-qid inflight 达到上限。
4. 设备发送入口暂时不能准入。

真正把 packet 推到链路上的是 `PointToPointNetDevice`。如果设备处于 `BUSY`，新包只会入队；等 `TransmitComplete()` 后再从 PPP 队列取下一个包。

## 2. 当前已经确认的问题：PPP 分类退化

最近 128 GPU AllToAll 说明，当前最直接的问题不在 `Dequeue()` 是否轮询，而在 DATA 包如何进入 PPP 子队列。

当前 `PppRoundRobinQueue` 的默认分类方式：

1. 解析 PPP header。
2. DATA 包调用 `CalculateQueueIndex()`。
3. `work_model=1` 时只对目的 MAC 做 hash。
4. `qid = hash(dst_mac) % NumQueues`。

这导致：

- 同一源端发往多个目的的 DATA flow 可能落到同一个 qid。
- 如果落到同一个 qid，PPP RR 只能看到一个非空子队列，实际退化成该子队列内 FIFO。
- 16 个队列时，当前拓扑地址规律和 `%16` 形成明显别名碰撞。
- 13 个队列打破碰撞后，尾部时延大幅下降。

因此，`drop_tail_queue_num=13` 是当前 workload 的有效 workaround，不是原则性的长期方案。

## 3. 优先方案 A：flow-aware qid classifier

短期最值得做的是让 DATA 的 qid 分类至少包含 flow key。

候选键：

```text
ThreePHeader::src
ThreePHeader::dst
ThreePHeader::tag
PPP src/dst MAC
```

目标：

1. 同一个 flow 的所有 packet 仍落到同一 qid，保持 flow 内顺序。
2. 同一源端发往多个目的的 flow 尽量分散到不同 qid。
3. `RdmaPortEngine::ClassifyQueueId_()` 和 `PppRoundRobinQueue::CalculateQueueIndex()` 使用一致逻辑。
4. PFC pause/admission 看到的 qid 与实际 PPP 入队 qid 一致。

注意点：

- 不建议使用当前 `work_model=0` 的 wall-clock 随机方式；它可能把同一 flow 的不同 packet 分到不同 qid，引入不必要重排序。
- 如果只改 PPP 入队，不改 RDMA port 侧 qid 分类，会出现 admission/pause 与实际队列不一致。
- 如果 `drop_tail_queue_num` 同时表示 PFC qid 数和调度 bucket 数，语义会继续混在一起。

## 4. 方案 B：更强 bit mixer，但仍按目的 MAC

这是更小的兼容性改动：

1. 保留目的 MAC 分类语义。
2. 在 `% NumQueues` 前对 MAC 数值做稳定 bit mixing。
3. 避免 power-of-two 队列数只看低位地址规律。

优点：

- 改动小。
- 对旧配置语义冲击最小。
- 能缓解 `NumQueues=16` 这类低位碰撞。

缺点：

- 仍然不是 flow-aware。
- 多个 flow 仍可能因为同目的或 hash 碰撞落入同一个桶。
- 不能从根本上表达“逐流公平”。

## 5. 方案 C：per-flow DRR

更彻底的做法是在 PPP 队列内部维护 flow queue，并对活跃 flow 做 Deficit Round Robin。

优点：

1. 不依赖固定 bucket 数。
2. hash 碰撞不会直接退化成 full-flow FIFO。
3. 更接近逐流公平。

代价：

1. 需要维护 flow state 和清理 inactive flow。
2. 需要重新审 PFC 统计、容量统计和 qid pause 语义。
3. 大规模仿真下会增加队列状态和调度开销。

这个方案不适合作为第一步窄改动。

## 6. 方案 D：设备 ready 驱动的 port pull

这是更接近真实 NIC/port dequeue 的方案：

- 只有当下层设备真正完成一次发送、再次 ready 时，才触发 RoCE scheduler 选择下一个 slice。
- `PointToPointNetDevice::TransmitComplete()` 需要提供更明确的向上 callback 或 trace source。
- `RdmaPortEngine` 从“尽量 drain”改成“设备 ready 时 pull 一个或一小批 slice”。

优点：

1. 链路速率和包长自然反映到调度节拍。
2. 更容易解释 QP scheduler 真正在控制发送机会。

代价：

1. 改动会深入到底层设备。
2. 需要小心不要和现有 PPP 队列、PFC、per-qid inflight 形成重复调度。
3. 验证面大于 flow-aware classifier。

## 7. 当前建议

短期不建议继续用“换队列数量”作为正式方案。队列数量可以作为实验变量，但不能作为算法机制。

如果下一步要改，建议顺序是：

1. 先做可配置的 qid classifier。
2. 默认保留旧目的 MAC 模式，新增 `flow-hash` 模式。
3. 确保 RDMA port 和 PPP 队列使用同一套 qid 结果。
4. 用 128 AllToAll 验证：把 `drop_tail_queue_num` 改回 16 后，尾部时间应接近当前 13 队列结果。
5. 如果 flow-hash 仍不足，再讨论 per-flow DRR 或设备 ready driven pull。
