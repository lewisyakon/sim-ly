# RoCE 工作流说明

这一份文档只讲一件事：一条普通 P2P 流从 Astra-Sim 下放之后，当前项目里的 RoCE 旁路是如何参与处理的。

可以把主链条概括成：

`Astra-Sim workload -> ASTRASimNetwork(sim_send/sim_recv) -> my_entry::SendFlow -> sendflowclient -> RoCE(QP/FSM/Header) -> ns-3 网络 -> 对端 sendflowclient -> 回调 Astra-Sim`

## 1. Astra-Sim 下放阶段

1. Astra-Sim 在 workload/stream 层决定发起一个 P2P 通信。
2. 发送侧调用 `sim_send`，接收侧调用 `sim_recv`。
3. 当前分支里，这个网络后端实现在 `scratch/my_AstraSimNetwork.cc`。

发送侧关键动作：

- 计算当前仿真使用的 `src/dst`
- 从 `request->flowTag` 里取出 `tag_id`
- 调用 `my_entry.h` 里的 `SendFlow(...)`

接收侧关键动作：

- 先在本节点登记接收回调
- 如果数据其实已经在本地 `recvHash` 中就绪，就立即回调，不再等待网络

## 2. `my_entry` 的桥接作用

`SendFlow(...)` 是 Astra-Sim 与 `sendflowclient` 之间的桥。

发送侧：

1. 找到源节点对应的 `sendflowclient`
2. 把发送完成回调写进 `m_sendfunctable`
3. 把 `sim_request` 写进 `m_request`
4. 调用 `TriggerSendTo_now(...)`

接收侧：

1. `sim_recv` 通过 `set_recvfunc(...)` 预注册回调
2. 后续最后一个 DATA 到达、或 `recvHash` 满足条件时，触发回调

## 3. `sendflowclient` 发送侧 workflow

### 3.1 决定发 REQUEST 还是 DATA

当前规则很直接：

- `datasize == 1023`：视为 REQUEST
- 其他：视为 DATA

然后统一进入发送路径。

### 3.2 RoCE 开启后，先入调度器而不是立即发包

当前发送路径已经分成两条：

- `RoCE disable`
  - 仍走 legacy `SendPacket(...)` 直发路径

- `RoCE enable`
  - `TriggerSendTo_now(...)` 不再直接发包
  - 先调用 `EnqueueRoceWork_(...)`
  - work item 进入 `RoceTxScheduler`
  - 再由 scheduler 选择下一个 slice，调用 `DispatchOneRoceSlice_()`

因此当前版本已经不是“带 RoCE 头的 legacy 直发”，而是：

- 业务仍由 3P 决定
- 出网顺序开始由 RoCE scheduler 控制

### 3.3 真正出网前，先构建 3P 头，再叠加 RoCE 头

发送包时，无论是否启用 RoCE，都会先准备：

- `HopCountHeader`
- `ThreePHeader`
- `FlowTagHeader`（仅 DATA）

若 RoCE 启用，还会调用 `BuildRoceTxHeader_(...)`：

1. 根据 `tag/src/dst` 构造 `RoceQpKey`
2. 调用 `EnsureQpReadyToSend(...)`
3. 根据当前 3P 状态决定 opcode
4. 让 `RoceQpEngine` 填好 `qpn/psn/opcode/user phase/length`
5. 再补齐仿真使用的 `remoteAddr/rkey/immData`

因此当前实现不是“用 RoCE 替换 3P”，而是：

- 3P 仍然是主业务头
- RoCE 头是附加的语义层
- 当前 `SEND` 也还不是完整 verbs 语义，因为接收侧 `RQ` 资源模型尚未补齐

### 3.4 三种报文的发送差异

说明：

- 下面描述的是当前 stage 1 实现的实际行为
- 不等于会议后已经定下来的最终字节口径
- 关于 `SEND/WRITE`、`SQ/RQ/CQ`、RoCE 可见头和 MTU 预算的目标设计，请同时参考：
  - [roce-phase2-design.md](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/doc/roce-phase2-design.md)

#### REQUEST

- 只发一个小控制包
- 在 RoCE 开启时，先以控制类 work 入队
- 真正出网后再触发发送回调

#### DATA

- 按 MTU 分片
- 当前 stage 1 里，分片预算仍按原始 `MTU` 走
- 下一阶段会改成：每包只扣可见 RoCE 头，其他仿真控制信息继续隐藏在业务区里
- 每个分片都带 `ThreePHeader::DATA`
- 每个分片都可携带 `RoceSimHeader`
- 在 RoCE 开启时，以数据类 work 入队
- 之后由 scheduler 逐 slice 放出
- 当前调度规则是：
  - 控制类优先
  - 数据类按活跃 QP 轮询
- 当前还额外受 transport 窗口约束：
  - 每个 QP 最多只允许 `transportWindowPackets` 个 packet outstanding
  - ACK 回来后窗口才会继续释放

#### ACT

- 由接收侧在“最后一个 DATA 包到齐”后反向发出
- 当前 RoCE opcode 为 `Send`
- 它是 3P 业务控制报文，不是 transport ACK
- 在 RoCE 开启时，它也先进入控制类队列，再由 scheduler 放出

## 4. 报文进入网络前的绑定

在 `scratch/my_common.h` 里，应用初始化时会：

1. `App->SetRoceConfig(g_roce_cfg)`
2. `App->initialization(proto, ...)`

其中 `proto` 在 RoCE 开启时取 `g_roce_cfg.etherType`。

所以当前 RoCE 是一个二层协议号绑定：

- 收发 socket 监听的是这个 EtherType
- 3P 头和 RoCE 头都走同一条二层路径

## 5. `sendflowclient` 接收侧 workflow

### 5.1 先按 legacy 路径拆头，再探测 RoCE 头

`HandleRead(...)` 的顺序是：

1. 去掉 `HopCountHeader`
2. 去掉 `ThreePHeader`
3. 取出 `tag/src/dst`
4. 若 RoCE 开启，先用 probe 包试探 `RoceSimHeader`
5. 只有 `tag/src/dst` 匹配时，才真正消费 RoCE 头

这样做的目的，是防止把 legacy 包误判成 RoCE 包。

### 5.2 收到 RoCE 头后做什么

`HandleRoceRxHeader_(...)` 做三类事：

1. QP 接收校验
   - 状态是否合法
   - opcode 是否支持
   - 长度是否合法

2. 语义一致性检查
   - `user phase` 是否与 `ThreePHeader::State` 一致
   - `opcode` 是否与本地推导一致

3. 轻量乱序接收跟踪
   - future PSN 会先进入 `pendingRxPsns`
   - 旧包/重复包不会再把 QP 直接打成 `Err`
   - future packet 不会再直接上送业务层
   - 当前会先进入 sendflowclient 的 reorder buffer，等前面的洞补齐后再按序释放

4. ACT 到达时推进业务 FSM
   - `ACT` 仅推进业务完成
   - 不再承担 transport completion 语义

5. transport ACK
   - 只要收到的不是 `RoceOpcode::Ack`，当前实现就会发一个协议 ACK
   - ACK 是 transport 层报文，不是业务 `ACT`
   - ACK 收到后会推进发送端的 outstanding 窗口

### 5.3 接收侧三种 3P 状态如何收敛

#### REQUEST

- 若接收回调已登记，立即回调 Astra-Sim

#### DATA

- 每到一个 DATA 分片就增加计数
- 第一个 DATA 会带来 `FlowTagHeader`
- 最后一个 DATA 到达时：
  - 反向发送一个 ACT
  - 写入 `recvHash`
  - 若回调已登记，则立即回调上层

#### ACT

- 发送方收到 `ACT` 后
- 触发最初登记的发送完成回调

## 6. completion / callback 的真实关系

当前版本里，完成关系已经收敛为下面这套：

1. 接收方收齐 DATA
2. 接收方显式发 ACT
3. 发送方收到 ACT
4. 发送方触发业务发送完成回调

所以：

- `ACT` 只是 3P 的业务控制报文
- 它不是 RC 可靠传输里的 ACK/completion

与此同时，transport 层自己的可靠性闭环现在已经存在：

1. 每个非 ACK transport packet 都会进入 outstanding 集合
2. 每个 QP 只维护一个 oldest-unacked timer
3. timeout 后 QP 进入 recovery
4. recovery 期间新的普通 packet 会被阻塞
5. retransmission 会先于普通 slice 发送

## 7. 当前 workflow 与 UB workflow 的差异

当前项目：

1. Astra-Sim 发起 P2P 流
2. `ASTRASimNetwork::sim_send/sim_recv`
3. `my_entry::SendFlow`
4. `sendflowclient`
5. `RoceTxScheduler` 入队与出队
6. 叠加 `RoceSimHeader`
7. 网络发送
8. 对端解析 3P + RoCE
9. 通过 REQUEST / DATA / ACT 回调 Astra-Sim

UB：

1. 应用发起 task / WQE
2. `UbTransaction` 或 `UbLdstInstance`
3. 切成 segment
4. `UbTransportChannel` 或 `UbLdstApi` 构包
5. 接收侧自动产生 ACK / response
6. ACK 推进窗口并完成 WQE/task

一句话概括：

- 当前项目：以 3P 业务流为中心，RoCE 是增强层
- UB：以事务/传输协议为中心，应用任务是上层负载
