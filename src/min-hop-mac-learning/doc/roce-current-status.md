# RoCE 当前状态

这份文档只回答四个问题：

1. 当前分支里的 RoCE 到底已经做了什么
2. 最近一次真实运行验证了什么
3. 当前还缺什么
4. 为什么调度改造先停在这里

## 1. 当前版本已经完成的内容

当前分支的定位是：

- `Astra-Sim + 3P` 仍然是主业务流程
- RoCE 作为旁路语义层接入
- `ACT` 已明确回归为纯业务控制报文
- `RoCE enable` 时，发送入口已改成：
  - 先入 `RoceTxScheduler`
  - 再按 QP / 类别调度出网

当前已经具备：

1. 轻量 `QP / QPN / PSN / opcode / user phase`
2. per-QP work queue
3. 控制类优先
4. 数据类按活跃 QP 轮询
5. DATA 逐 slice 出网
6. transport ACK
7. 每 QP 在途窗口 `transportWindowPackets`
8. 每 QP 一个 oldest-unacked 定时器
9. recovery 模式
10. ACK 高优先级
11. bounded reorder window 与 overflow 处理
12. ACK 侧带累计确认语义
13. 接收侧 future packet 的缓冲与按序释放
14. 自动 ACK timeout 基线推导

## 2. 最近一次讨论后已经定死的口径

这一轮讨论后，下面这些边界已经固定：

1. 对外只讲 `SEND + WRITE`
   - `READ` 可以继续留在枚举和代码入口里，但不再作为对外能力表述
2. `ACT` 继续只是业务控制报文
   - 不再回到 transport ACK 语义
3. `post_send / post_recv` 是本地动作
   - 不是网络中的“post 包”
4. QP 的目标结构应显式包含 `SQ / RQ / CQ`
   - 当前代码还没有做到这一点
5. legacy 与 RoCE 的字节口径不同
   - legacy：`datasize = 以太网 payload 总预算`
   - RoCE：每包真实额外开销只扣可见 RoCE 头，其他仿真控制信息仍隐藏在业务区里
6. ACK / 重传 / 有限乱序窗口属于 RoCE 层，不再推给底层以太网层

## 3. 最近一次真实运行结论

使用：

- [config.json](/home/lja/sim-ly/sim-ly/simconfig/config.json)
- [microAllReduce.txt](/home/lja/sim-ly/sim-ly/simworkload/microAllReduce.txt)

最近一次真实运行的结论是：

1. workload 能真实下发发送
2. `RoCE` 路径真实激活
3. transport ACK 活跃工作
4. 没有看到 `RTX_TIMEOUT / RTX_SEND / RTX_DROP`
5. 全部事务流都完成

这次运行里可直接确认的结果：

- `total send count = 192`
- `total events = 1135708`
- `ACK_RX = 77760`
- `ACK_TX = 77760`
- `REQ_DONE = 192`
- `RX_DONE = 192`
- `TX_DONE = 192`
- `RTX_TIMEOUT = 0`
- `RTX_SEND = 0`
- `RTX_DROP = 0`

这轮现象说明：

- 当前 workload + 队列参数下，transport 层已经足够稳定，没有触发 timeout retransmission。
- 这不能证明“重传路径永远正确”，但能说明当前结构修复后，ACK / 窗口 / 优先级这条链路已经不再自然制造重传风暴。

最近进一步把 `transport_window_packets` 提高到接近当前 workload 的“近似一次性发送”上限时，也没有重新出现真实丢包和 retransmission，但总完成时间会明显变长。当前更合理的解释是：

1. `PFC` 与 external path backpressure 已经能在硬丢包前接住压力
2. ACK sideband 化后，不会再把反向有序控制流状态机带偏
3. 更大的 sender window 会提高注入强度，增加排队与 pause/resume 周期
4. 因此系统现在更像是进入了“无丢包但更高排队/更高回压”的性能区间，而不是再次进入 correctness 失败区间

## 4. 最近一轮现象的一个重要解释

最近把队列长度从 `2000 -> 350 -> 200` 逐步缩小时，仍然没有明显看到新的丢包和重传。

当前更合理的解释不是“队列大小完全不重要”，而是：

1. `transportWindowPackets` 已经对每个 QP 的在途流量形成了强 admission control
2. ACK 高优先级后，窗口释放更快
3. ACK sideband 化后，不再被反向 reorder window 卡住
4. oldest-unacked 单 timer 把早期那种 timeout 风暴切掉了
5. future packet 不再直接上送业务层，累计 ACK 行为更稳定
6. external path 现在按下游 RRQ 子队列阈值做 backpressure，不再出现“下游先炸、上游还在塞”的错位

因此在当前这组 workload 下，更主导整体行为的已经是 transport 层的“类滑动窗口”，而不是设备队列长度本身。

## 5. 当前明确还没有完成的内容

当前版本还不能宣称为完整 RC transport，缺口主要在：

1. 真正的 `SQ / RQ / CQ`
2. `SEND` 所需的 `RQ` 资源模型
3. retransmission 更深地并入统一 scheduler
4. 更真实的 receiver-side `RQ` 资源模型
5. `sq_depth/rq_depth` 与 scheduler / backpressure 的更深绑定
6. CQ 驱动发送推进
7. 动态 RTO
8. 事务完成统计与 transport 完整性的更严格绑定
9. 更完整的累计 ACK / recovery 策略细化

## 6. 当前为什么还没看到理想的多 QP 数据 RR

当前 `RoceTxScheduler` 的 RR 逻辑已经存在，但最近一次真实运行里没有看到理想的交替发送，原因主要有两点：

1. 同一 sender 的多个 `DATA` work 进入 scheduler 的时机仍然偏串行
2. `sendflowclient` 这层会在零延迟里连续 dispatch slice，把一个 QP 的多个 slice 很快提交给下层

因此当前更准确的说法是：

- 已经有“QP 级发送入口调度”
- 但还没有把“QP 级 wire-time 发送仲裁”做实

## 7. 当前决策

当前决定是：

- 先把版本停在这里
- 不继续为了“把 RR 做得更像 NIC”而深入改底层设备

原因很直接：

1. 接收侧乱序问题已经修到可接受
2. 再往下做调度需要明显更多底层改动
3. 当前收益不足以覆盖改动代价

后续如果重新讨论这件事，请直接看：

- [roce-future-scheduling-options.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-future-scheduling-options.md)
- [roce-phase2-design.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-phase2-design.md)

## 8. 建议阅读顺序

如果你是回头再看这个分支，建议顺序如下：

1. 先看这份当前状态文档
2. 再看 [roce-workflow.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-workflow.md)
3. 然后看 [roce-scheduler-stage1.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-scheduler-stage1.md)
4. 如果要看最近这轮 `ACK / 窗口 / 重传 / 重排` 的具体改动，先看 [roce-transport-reliability.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-transport-reliability.md)
5. 如果要继续做协议能力补全，再看 [roce-phase2-design.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-phase2-design.md)
6. 需要比较 UB 或继续谈 QP，再看 [roce-qp-and-transport.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-qp-and-transport.md)
