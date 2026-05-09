# RoCE 后续调度方案备忘

最近修改：2026-04-23

记录时间：2026-04-10

这份文档只记录“如果以后继续提高调度真实性，可以考虑什么”。它不再描述早期 stage 1 调度状态。

当前主路径请先看：

- [roce-workflow.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-workflow.md)
- [roce-current-status.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-current-status.md)
- [roce-qp-and-transport.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-qp-and-transport.md)

如果需要了解旧阶段背景，再看：

- [archive/roce-scheduler-stage1.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/archive/roce-scheduler-stage1.md)

## 1. 当前调度状态

当前 RoCE packet runtime 已经下沉到 `RdmaPortEngine`。

当前发送大致是：

```text
RdmaPortEngine::PostSend
  -> QpRuntime::sqEntries
  -> RoceTxScheduler
  -> RdmaPortEngine::DispatchFromScheduler_
  -> RdmaPortEngine::TrySendNow_
  -> PointToPointNetDevice::SendFrom
```

`DispatchFromScheduler_()` 会尽量 drain scheduler，直到遇到下面任一限制：

1. QP outstanding 达到 `transportWindowPackets`。
2. QP 正在 recovery。
3. 目标 qid pause 或 per-qid inflight 达到上限。
4. 设备发送入口暂时不能准入。

这比旧 stage 1 更接近 port runtime，但仍不等于真实 RNIC 基于 wire-time completion 的 dequeue。

## 2. 方案 A：基于真实发送完成事件的 pull 调度

思路：

- 只有当下层设备真正完成一次发送、再次 ready 时，才触发 RoCE scheduler 选择下一个 slice。

可能需要改：

1. `PointToPointNetDevice`
   - 在发送完成时提供更明确的向上 callback 或 trace source。

2. `RdmaPortEngine`
   - 从“尽量 drain”改成更接近“设备 ready 时 pull 一个或一小批 slice”。

3. `FlowIo / bridge` 边界
   - 如果未来 external path 也要统一纳入该节拍，需要重新梳理职责。

优点：

1. 最接近真实 NIC/port dequeue 语义。
2. 链路速率和包长自然反映到调度节拍。
3. 更容易解释“QP scheduler 真正在控制发送机会”。

代价：

1. 改动会深入到底层设备。
2. 需要小心不要和现有 `PppRoundRobinQueue`、PFC、per-qid inflight 形成重复调度。
3. 验证成本比文档级或上层 pacing 改动高。

## 3. 方案 B：port 层 per-slice pacing 近似

思路：

- 不改真实设备 ready 回调。
- 在 `RdmaPortEngine` 层估算 slice wire time。
- 每次只放出有限 slice，下一轮按估算发送时间延迟调度。

更合理的近似是：

```text
dispatch_gap = slice_wire_bytes / link_rate (+ IFG)
```

优点：

1. 改动面小于方案 A。
2. 不一定需要改 `PointToPointNetDevice`。
3. 可以更快观察多 QP RR 在时间线上是否符合预期。

缺点：

1. 仍然不是基于真实设备 completion。
2. 容易和下层设备队列形成双重调度。
3. 如果估算不准，可能引入额外空泡或过强限速。

## 4. 当前建议

短期内先不继续改调度层。

理由是：

1. 当前 RoCE 主路径、ACK、重传、reorder 和内存问题刚收敛，继续深入设备节拍会扩大验证面。
2. 当前最需要先做的是用一致配置跑更大 workload，确认 `transportWindowPackets / PFC / per-qid inflight` 组合是否稳定。
3. 如果后面发现多 QP 公平性或时间行为明显不合理，再回头在方案 A 和方案 B 之间选。
