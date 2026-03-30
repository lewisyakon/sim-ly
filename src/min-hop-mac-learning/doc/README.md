# Min-Hop Mac Learning RoCE 文档索引

本文档集记录当前分支中 RoCE 旁路集成的设计、配置和代码行为，重点服务下面三个目标：

1. 解释当前版本到底“已经做了什么”。
2. 说明它与 legacy 3P、与 UB 的关系。
3. 为后续把 QP 从语义层推进到发送调度层提供稳定参考。

## 当前状态

当前版本的定位是：

- `Astra-Sim + 3P` 仍然是主业务流程。
- RoCE 已经作为旁路语义层接入。
- 已支持轻量 `QP / QPN / PSN / opcode / user phase`。
- 当前对外能力口径收敛为 `SEND + WRITE`，`READ` 只保留代码入口，不再作为对外能力描述。
- `ACT` 已被明确还原为纯业务控制报文，不再承担可靠传输 ACK/completion 含义。
- `RoCE enable` 时，若 `EnsureQpReadyToSend()` 后 QP 仍不可发，会直接报错，不再回退 legacy。
- `RoCE enable` 时，发送入口已经改成：
  - 先入 `RoceTxScheduler`
  - 再按 QP / 类别调度出网
- 当前已经有：
  - per-QP work queue
  - 控制类优先
  - 数据类按 QP 轮询
  - 逐 slice 出网
  - bounded reorder window 与 overflow 处理
  - transport ACK
  - ACK 侧带累计确认语义
  - 每 QP 在途窗口
  - oldest-unacked 单定时器
  - recovery 模式
  - ACK 高优先级
  - 自动 ACK timeout 基线
  - 主程序级 `network drained / last FLOW_TX_DONE` 时间观测

当前还没有完成的部分：

- 真正的 `SQ / RQ / CQ`
- `sq_depth/rq_depth` 与 scheduler / backpressure 的更深绑定
- 更完整的 CQ 驱动发送推进
- retransmission 更深地并入统一 scheduler
- 动态 RTO
- 事务完成统计与 transport 完整性的更严格绑定
- 更完整的累计 ACK / recovery 策略细化

## 文档导航

1. [roce-current-status.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-current-status.md)
   - 当前版本到底停在哪
   - 最近一次真实运行验证了什么
   - 当前为什么决定先不继续深挖调度

2. [roce-config-and-logs.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-config-and-logs.md)
   - `roce` JSON 配置项
   - `[ROCE][CFG] / [ACTIVE] / [TX] / [RX]` 日志解释
   - 配置在代码里如何接入

3. [roce-workflow.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-workflow.md)
   - 一条 P2P 流从 Astra-Sim 下放到 `my_entry`
   - `sendflowclient` 如何发 REQUEST / DATA / ACT
   - 接收侧如何解析 3P 头和 RoCE 头
   - callback 如何回到 Astra-Sim

4. [roce-scheduler-stage1.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-scheduler-stage1.md)
   - RoCE 包如何入队
   - 调度器如何从队列中选择下一个 slice
   - 当前阶段的控制优先与数据 RR 规则
   - 当前阶段还没有做哪些 transport 能力

5. [roce-qp-and-transport.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-qp-and-transport.md)
   - QP 的定义、组成和 QPN 含义
   - 一个 NIC 上为什么会有多个 QP
   - 当前项目的轻量 QP 到底负责什么
   - UB 已实现哪些 transport 能力
   - `post_send/post_recv` 与 `SQ/RQ/CQ` 的正确口径

6. [roce-phase2-design.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-phase2-design.md)
   - 会议后收敛出的下一阶段设计口径
   - `SEND/WRITE`、`SQ/RQ/CQ`、RoCE 可见头、ACK/重传、有限乱序窗口

7. [roce-transport-reliability.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-transport-reliability.md)
   - 最近几轮 `ACK / 窗口 / 重传 / 重排 / ACK 优先级 / 自动 timeout` 的具体实现
   - 这些改动分别落在哪些文件
   - 为什么现在缩小队列也不一定能复现丢包和重传

8. [roce-future-scheduling-options.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-future-scheduling-options.md)
   - 未来两类候选调度方案
   - 为什么当前先不实现

## 阅读建议

- 如果你想先知道当前分支到底做到哪，先看 `roce-current-status.md`。
- 如果你要先改配置或看日志，再看 `roce-config-and-logs.md`。
- 如果你要理解现在代码是怎么跑的，再看 `roce-workflow.md`。
- 如果你要看包入队后怎么出队，再看 `roce-scheduler-stage1.md`。
- 如果你要快速回顾最近几轮 `ACK / 窗口 / 重传 / 重排` 的落地代码，再看 `roce-transport-reliability.md`。
- 如果你要继续做协议能力补全，先看 `roce-phase2-design.md`。
- 如果你以后要重新讨论更深的发送调度，再看 `roce-qp-and-transport.md` 和 `roce-future-scheduling-options.md`。
