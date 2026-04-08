# RoCE 当前状态

这份文档只回答四个问题：

1. 当前版本到底已经做了什么。
2. 最新一次真实运行验证了什么。
3. 当前版本最值得警惕的点是什么。
4. 下一步修正实现时，应该优先盯哪里。

## 1. 当前版本的准确定位

当前版本不应再简单描述成“RoCE 已经解耦到 `RoceTransportEngine`”。

更准确的说法是：

1. 第一阶段已经完成：
   - 从 `sendflowclient` 中抽出 `RoceTransportEngine`
   - 把 lane/QPN/QP/PSN、scheduler、RX reorder 等语义收进去

2. 之后又完成了一轮端口侧下沉：
   - 引入 `RdmaDispatchTag`
   - 引入 `RdmaPortEngine`
   - 让 `FlowIo / bridge` 可以把带 tag 的 packet 提交给具体 outPort
   - 让 pause-aware 准入和端口 inflight 控制开始下沉

3. 当前最新这一版又往前走了一步：
   - 引入 `RdmaIngressPortTag`
   - bridge 在本地上送应用前写入真实 ingress p2p device index
   - `sendflowclient` 在 ACK 收包时，先把 ACK 路由到对应 ingress 口上的 `RdmaPortEngine`
   - `RdmaPortEngine` 现在开始真实维护端口侧 outstanding/ACK retire/retransmission timer

所以当前实现的真实形态是：

- `sendflowclient`
  - 业务 facade
- `RoceTransportEngine`
  - RoCE 语义协调器
- `RdmaPortEngine`
  - 端口执行层

这三者共同组成当前版本的 RoCE 实现。

## 2. 当前版本已经完成的内容

当前版本已经具备：

1. `REQUEST / DATA / ACT` 三条逻辑 lane
2. `flow + lane -> QPN` 稳定映射
3. per-QP work queue
4. 控制类优先
5. 数据类按活跃 QP 轮询
6. `RoceSimHeader` 生成
7. QP/PSN/RX reorder 语义
8. bounded reorder window 与 buffered upper packet
9. transport ACK
10. ACK 生成与 ACK 收包处理
11. 每个物理 p2p 口上的 `RdmaPortEngine`
12. per-qid pending queue
13. per-qid inflight packet 限制
14. pause-aware 准入
15. ACK retire 开始下沉到端口侧
16. retransmission timer 开始下沉到端口侧
17. retry 计数从“timeout 即消耗”改成“真正重发时才消耗”

## 3. 当前版本还处于过渡态的部分

当前版本还不能被描述成“完整 port-centric RoCE 模型”，因为下面几件事还没有做完：

1. `QP/SQ/RQ/CQ` 还没有完整下沉到端口侧
2. `RoceTransportEngine` 仍然保留一部分镜像状态：
   - `m_outstandingPackets`
   - `m_recoveryTargetPsns`
   - `m_bufferedUpperPackets`
3. ACK 路径当前是“双通道状态更新”：
   - 端口侧 `RdmaPortEngine::HandleAck(...)`
   - 语义侧 `RoceTransportEngine::OnRxPacket(...) -> HandleAck_(...)`
4. ACK 报文本身仍然由 `sendflowclient` 组包并发送，不是直接由端口侧独立生成
5. RX reorder 仍然主要在 `RoceTransportEngine`，还没有完整转到端口侧

因此，这一版最准确的描述是：

- “RoCE 已经从单纯应用层实现，进入应用 facade + 语义 transport + 端口执行层并存的过渡态。”

## 4. 最新一次真实运行验证

当前仓里保留的最新一次 `ROCE=true` 真实运行使用的是：

- [config.json](/home/lja/sim-ly/sim-ly/simconfig/config.json)
- [microAllReduce.txt](/home/lja/sim-ly/sim-ly/simworkload/microAllReduce.txt)
- [simulation.log](/home/lja/sim-ly/sim-ly/simulation.log)

这次可以直接确认的结果是：

1. `REQ_DONE = 48`
2. `RX_DONE = 48`
3. `TX_DONE = 48`
4. `total send count = 48`
5. `total events = 443645`
6. `last FLOW_TX_DONE since_workload_start(us) = 244.406`
7. `network drained since_workload_start(us) = 244.734`

对应日志：

- [simulation.log:39298](/home/lja/sim-ly/sim-ly/simulation.log#L39298)
- [simulation.log:39299](/home/lja/sim-ly/sim-ly/simulation.log#L39299)
- [simulation.log:39301](/home/lja/sim-ly/sim-ly/simulation.log#L39301)
- [simulation.log:39302](/home/lja/sim-ly/sim-ly/simulation.log#L39302)

同时，这份最新日志里没有搜到：

- `[ROCE][RTX]`
- `REORDER_OVERFLOW`
- `flow remap detected`
- `QP rejected`
- `opcode mismatch`
- `phase mismatch`

因此可以确认：

1. 当前版本在这组 workload 上是功能闭环的。
2. 当前端口侧 ACK retire / retrans timer 下沉后，没有直接打断 `REQ_DONE / RX_DONE / TX_DONE`。

## 5. 当前最重要的风险提示

这一版最重要的事实不是“它能跑通”，而是“它跑通的同时，行为已经变了”。

当前仓里最新日志显示：

- `last FLOW_TX_DONE since_workload_start(us) = 244.406`
- `network drained since_workload_start(us) = 244.734`

而这轮端口侧下沉前，开发过程里同 workload 的时间口径曾经明显更大。当前仓内旧日志已经被覆盖，所以这里不再给旧值做文件引用，但这个行为变化本身是本轮实现 review 时必须正视的事实。

因此，当前版本不能被简单当作：

- “纯重构”
- “只是换个类放代码”

更准确的说法是：

- 功能闭环已经恢复
- 但时序行为已经明显改变
- 当前需要继续确认这是更合理的模型，还是某些窗口/节流语义被削弱了

## 6. 上一轮 `ROCE=false` 回归应怎样看待

之前单独做过一次 `ROCE=false` 回归，结论是：

1. 传统 3P 路径是通的。
2. `REQ_DONE / RX_DONE / TX_DONE` 都闭环。
3. 没看到 `Callback not found / REORDER_OVERFLOW / phase mismatch / opcode mismatch`。

但要注意：

- 那次 `ROCE=false` 回归是在“3 lane 稳定后、端口侧最新 ACK/retrans 下沉之前”完成的。
- 当前这份文档记录的是“最新实现状态”。
- 最新这轮端口侧改动后，还没有再单独留一份新的 `ROCE=false` 日志存档。

所以当前对 `ROCE=false` 最准确的说法是：

- 有过一次较早阶段的真实回归通过。
- 但对“最新端口侧下沉版本”来说，`ROCE=false` 还没有再次单独留档验证。

## 7. 现在最值得继续检查的实现点

如果你现在要 review 这版实现，我最建议优先看下面四件事：

1. `RdmaPortEngine` 和 `RoceTransportEngine` 是否还存在重复状态。
   - 当前 ACK retire 和 outstanding 至少存在双份视角。

2. 当前端口侧 inflight 限制是否削弱了原来想表达的 transport window 语义。
   - 这和时间变化直接相关。

3. 当前 ACK 先路由到 ingress `RdmaPortEngine`、再进入 `RoceTransportEngine` 的顺序是否有隐藏副作用。

4. 当前 scheduler 仍在 `RoceTransportEngine`，而执行准入已在 `RdmaPortEngine`。
   - 这意味着“谁能发”和“谁被挑中去发”还没彻底统一。

## 8. 当前明确还没有完成的内容

当前版本还不能宣称完成下面这些能力：

1. 真正的 `SQ / RQ / CQ`
2. 端口侧完整 `QP runtime`
3. RX 路径完整下沉
4. ACK 生成完整下沉
5. completion queue 驱动
6. port credit、QP resource limit 与 scheduler 的完全统一
7. 更真实的 `RQ/SRQ/CQ` 资源模型

## 9. 当前为什么要先停下来更新文档

当前已经到了一个很容易“代码能跑，但边界感开始模糊”的阶段。

如果这时候只继续写代码，不先把文档更新清楚，后面很容易出现两种误判：

1. 误以为 `RdmaPortEngine` 已经接管了全部 transport 逻辑。
   - 实际上还没有。

2. 误以为当前版本只是“无行为变化的解耦”。
   - 实际上当前功能闭环虽然通，但时序行为已经明显变化。

所以这份文档的目的不是报喜，而是把当前版本的真实位置钉住，方便下一步继续修实现。
