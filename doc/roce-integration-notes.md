# RoCE 集成说明（当前分支）

本文档整理了当前项目中 RoCE 旁路集成的配置项、日志含义和关键代码实现位置，便于后续联调与扩展。

## 1. JSON 中 `roce` 配置项说明

配置路径：`simconfig/config.json` 的 `"roce"` 对象。

- `enable`（bool）
  - 含义：是否启用 RoCE 旁路逻辑。
  - 影响：`true` 时发送/接收路径会尝试附加并解析 `RoceSimHeader`，并启用 QP/FSM/CQ 语义；`false` 时走 legacy 逻辑。

- `mode`（string）
  - 含义：RoCE 语义模式名（当前实现会转小写做匹配）。
  - 影响：决定 3P 状态到 RoCE opcode 的映射倾向。
  - 当前常用：`"rc_3phase"`。

- `data_use_write`（bool）
  - 含义：DATA 阶段是否默认使用 RDMA WRITE 语义。
  - 影响：在非 `send`/`read` 显式模式下，DATA 通常映射到 `RdmaWrite`（opcode=2）。

- `strict_completion`（bool）
  - 含义：预留字段。
  - 影响：当前版本中 ACT 已经被还原为纯业务控制报文，该字段不再参与 ACT 处理。

- `sq_depth`（uint32）
  - 含义：默认 QP Send Queue 深度。
  - 影响：初始化 QP context 时用于发送队列容量约束。

- `rq_depth`（uint32）
  - 含义：默认 QP Receive Queue 深度。
  - 影响：初始化 QP context 时用于接收队列容量约束。

- `ether_type`（string/number）
  - 含义：RoCE 报文使用的二层 EtherType（可写 `"0x9999"` 或数字）。
  - 影响：应用初始化时协议号会绑定到该 EtherType，收发双方需一致。

## 2. 日志输出含义

### 2.1 当前你日志里出现的行

示例（`simulation.log`）：

```text
[ROCE][CFG] enable=true mode=rc_3phase dataUseWrite=true etherType=0x9999 reqOp=0 dataOp=2
```

字段含义：

- `enable`：是否启用 RoCE。
- `mode`：模式名（标准化后）。
- `dataUseWrite`：DATA 阶段是否偏向 WRITE 语义。
- `etherType`：当前绑定 EtherType。
- `reqOp`：REQUEST 阶段映射的 opcode（这里 `0` 对应 `Send`）。
- `dataOp`：DATA 阶段映射的 opcode（这里 `2` 对应 `RdmaWrite`）。

为什么会重复多行：每个 `sendflowclient` 实例在调用 `SetRoceConfig` 时都会打印一次配置日志。

### 2.2 其他可能出现的 RoCE 日志

- `[ROCE][ACTIVE] ...`
  - 第一次真正构建 RoCE 头时打印，表示该节点进入 RoCE 活跃发送路径。

- `[ROCE][TX] ...`
  - 发送侧 QP 不可用或构头失败时打印；该包会回退 legacy 路径。

- `[ROCE][RX] ...`
  - 收包侧头部不匹配、QP 拒收、phase/opcode 不一致时打印。

## 3. 代码改动位置与目的

### 3.1 主要逻辑文件

- `src/min-hop-mac-learning/model/my_sendflowclient.cc`
  - 发送路径：
    - `BuildRoceTxHeader_`：按 3P 状态构建 RoCE 头，维护 QP 就绪，写入 opcode/user-phase/PSN 等。
    - `SendPacket`：在 REQUEST/DATA/ACT 三类包发送前附加 RoCE 头，并正确扣减 MTU 预算中的 RoCE 头字节。
  - 接收路径：
    - `HandleRead`：在 legacy 头解析后探测 RoCE 头，存在时进入 `HandleRoceRxHeader_`。
    - `HandleRoceRxHeader_`：执行 QP 侧接收校验，并在 ACT 路径推进业务 FSM。

- `src/min-hop-mac-learning/model/my_sendflowclient.h`
  - 新增 RoCE 相关辅助方法声明与成员（配置、QP engine、FSM）。

### 3.2 配置接入文件

- `scratch/my_common.h`
  - 解析 `roce` JSON 子对象并写入全局 `g_roce_cfg`。
  - 在应用初始化时调用 `App->SetRoceConfig(g_roce_cfg)` 并绑定对应协议号。

## 4. 设计意图（为什么这样写）

- 不打断现有 workload 时序：RoCE 作为旁路语义叠加，不替换原 3P 工作流。
- 渐进增强：RoCE 头异常、QP 未就绪时可回退 legacy（除非开启严格 completion）。
- 可调试：关键节点输出 `[ROCE][CFG]/[ACTIVE]/[TX]/[RX]`，便于快速定位是“配置问题、构头问题”还是“接收校验问题”。
- 可扩展：QP、FSM、Header 语义均拆分为独立类，后续可继续填充更完整的 RC/READ/重传行为。

## 5. 当前项目中 P2P 流的完整 Workflow

这一节只讨论一条普通 P2P 流从 Astra-Sim 下放到 `my_entry` 之后，RoCE 旁路如何参与处理。可把它理解成：

`Astra-Sim workload -> ASTRASimNetwork(sim_send/sim_recv) -> my_entry::SendFlow -> sendflowclient -> RoCE(QP/FSM/Header) -> ns-3 网络 -> 对端 sendflowclient -> 回调 Astra-Sim`

### 5.1 Astra-Sim 下放阶段

1. Astra-Sim 在 workload/stream 层决定发起一个点到点通信后，会调用网络后端的 `sim_send` / `sim_recv`。
2. 在当前分支里，这个后端实现是 `scratch/my_AstraSimNetwork.cc` 里的 `ASTRASimNetwork`。
3. 发送侧 `sim_send` 的关键动作：
   - 把 `dst` 加上 `npu_offset`，对齐当前仿真的节点编号。
   - 从 `request->flowTag` 中取出 `tag_id` 作为后续 flow 键。
   - 调用 `my_entry.h` 里的 `SendFlow(...)`，把这次发送正式下放到 `sendflowclient`。
4. 接收侧 `sim_recv` 的关键动作：
   - 先在本节点的 `sendflowclient` 上登记一个接收回调。
   - 如果数据其实已经在本地 `recvHash` 中准备好了，就直接触发回调，不必再次等待网络包。

这里要注意：Astra-Sim 看到的是“发起一次逻辑 P2P 发送/接收”；RoCE 旁路从这一层开始，主要负责给后续 3P 报文增加更像 RoCE 的语义信息，而不是改写 Astra-Sim 的调度逻辑。

### 5.2 `my_entry` 负责把逻辑发送转成 `sendflowclient` 动作

`SendFlow(...)` 是 Astra-Sim 和 ns-3 应用层之间的桥。

1. 发送侧先通过 `nodeapptable[src]` 找到源节点对应的 `sendflowclient`。
2. 对普通数据流：
   - 把 Astra-Sim 的发送完成回调塞进 `m_sendfunctable`。
   - 把 `sim_request` 写入 `m_request`，后续 DATA/ACT 阶段都会用到里面的 `flowTag`。
   - 调用 `TriggerSendTo_now(...)`，真正触发 REQUEST 或 DATA 阶段发包。
3. 对接收侧：
   - `sim_recv` 通过 `set_recvfunc(...)` 预注册回调。
   - 后续当最后一个 DATA 包到达、或者 `recvHash` 已经满足条件时，再把回调还给 Astra-Sim。

这一步本身还不是 RoCE 逻辑，但它决定了 RoCE 能拿到哪些关键字段：

- `tag/src/dst`：后续既是 3P flow 键，也是 RoCE 仿真里的 `qpn`/completion 归并键。
- `flowTag`：DATA 报文里会通过 `FlowTagHeader` 保留。
- `msg_handler/fun_arg`：最后仍由 legacy 发送完成/接收完成回调通知 Astra-Sim。

### 5.3 `sendflowclient` 发送侧 workflow

#### 5.3.1 决定发 REQUEST 还是 DATA

`TriggerSendTo_now(...)` 当前采用一个很直接的规则：

- `datasize == 1023`：视为 REQUEST 控制流，发送 `ThreePHeader::REQUEST`
- 其他：视为普通数据流，发送 `ThreePHeader::DATA`

然后统一进入 `SendPacket(...)`。

#### 5.3.2 先构建 legacy 3P 头，再尝试叠加 RoCE 头

`SendPacket(...)` 无论 RoCE 是否开启，都会先准备：

- `HopCountHeader`
- `ThreePHeader`
- `FlowTagHeader`（仅 DATA 阶段）

如果 RoCE 开启，还会额外执行 `BuildRoceTxHeader_(...)`：

1. 先根据 `tag/src/dst` 生成一个仿真的 `RoceQpKey`
   - 当前实现里 `qpn = tag`
2. 调用 `EnsureQpReadyToSend(...)`
   - 自动把 QP 从 `Reset -> Init -> Rtr -> Rts`
3. 根据当前 3P 状态决定 RoCE `opcode`
   - `REQUEST -> Send`，或在 read 模式下变成 `RdmaReadReq`
   - `DATA -> RdmaWrite / Send / RdmaReadResp`
   - `ACT -> Send`
4. 调用 `RoceQpEngine::BuildSendHeader(...)`
   - 写入 `opcode / user-phase / qpn / psn / length / tag / src / dst`
   - 同一条 QP 的 `psn` 会单调递增
5. 再补齐仿真用的操作元数据
   - `remoteAddr`
   - `rkey`
   - `immData`
   - `flags`
6. 最后把 `RoceSimHeader` 插到包里，再进入 `FlowIo` 发包

所以当前发送路径不是“用 RoCE 替换 3P”，而是“在 3P 包格式外再套一层轻量 RoCE 语义头”。

#### 5.3.3 REQUEST / DATA / ACT 三种报文在发送侧的区别

1. REQUEST
   - 只发一个小控制包
   - 发完后立即调用发送回调
   - 本质上更像一次 request-notify

2. DATA
   - 按 MTU 分片
   - 每个分片都带 `ThreePHeader::DATA`
   - 每个分片都可以带一个 `RoceSimHeader`
   - 发送前把 `m_ackPacketsExpected[tag,src,dst]` 置为 1，表示这轮数据最终期待一个 ACT

3. ACT
   - 由接收侧在“最后一个 DATA 包到齐”时反向发出
   - 当前语义上把它映射为一个业务控制包，因此 RoCE opcode 选的是 `Send`
   - 它不是 RC 传输层内部 ACK，而是 3P 显式可见的第三阶段控制报文

### 5.4 报文进入 ns-3 网络前的绑定关系

`scratch/my_common.h` 在创建每个 `sendflowclient` 时做了两件对 RoCE 很关键的事：

1. `App->SetRoceConfig(g_roce_cfg)`
   - 把 JSON 里的 `roce` 配置注入到每个 `sendflowclient`
2. `App->initialization(proto, ...)`
   - `proto` 在 RoCE 开启时取 `g_roce_cfg.etherType`
   - legacy 时回退到 `0x9999`

这意味着当前项目里的 RoCE 是一个二层协议号绑定：

- 应用收发 socket 监听的就是 `etherType`
- 网络设备/队列调度看到的是同一个二层 frame
- 3P 头和 RoCE 头都在这条二层路径里传递

### 5.5 `sendflowclient` 接收侧 workflow

#### 5.5.1 先按 legacy 路径拆头，再探测 RoCE 头

`HandleRead(...)` 的顺序是：

1. 先拷贝包
2. 移除 `HopCountHeader`
3. 移除 `ThreePHeader`
4. 从 `ThreePHeader` 拿到 `tag/src/dst`
5. 如果 RoCE 开启，先用 probe 包试探 `RoceSimHeader`
6. 只有当 `RoceSimHeader` 中的 `tag/src/dst` 与 3P 头匹配时，才真正消费这层 RoCE 头

这样做的目的，是确保 RoCE 只是旁路增强，不会把 legacy 包误判成 RoCE 包。

#### 5.5.2 收到 RoCE 头后，先交给 QP 引擎和 3P FSM

`HandleRoceRxHeader_(...)` 会做三类事：

1. QP 侧接收校验
   - `OnRxHeader(...)` 检查：
     - 当前 QP 状态是否允许接收
     - opcode 是否在支持白名单里
     - `psn` 是否与 `expectedRxPsn` 严格匹配
     - 对 `WRITE/READ_RESP` 是否带了合法长度
   - 如果失败，就把 QP 置 `Err` 并打印 `[ROCE][RX]`

2. 语义一致性检查
   - `user-phase` 是否与当前 `ThreePHeader::State` 对齐
   - `opcode` 是否与 `ToRoceOpcode(...)` 推导一致

3. ACT 业务事件推进
   - 如果当前收到的是 `ACT`
   - 且 QP 接收合法
   - 则直接驱动 `m_roceFsm->OnActReceived(...)`

#### 5.5.3 三种 3P 状态在接收侧如何收敛

1. REQUEST
   - 如果本地已经登记了 `recvfunc`
   - 直接把 REQUEST 当作“请求到达”事件，回调 Astra-Sim

2. DATA
   - 每到一个 DATA 包，就更新 `m_dataPacketsReceived`
   - 第一个 DATA 会带来 `FlowTagHeader`，从而建立 flow 的元信息
   - 当最后一个 DATA 到达时：
     - 反向调度一个 `ACT`
     - 在本地写入 `recvHash`
     - 如果接收回调已经登记，就立即触发上层回调

3. ACT
   - 这是发送方收到的“第三阶段控制报文”
   - 最终再触发发送方当初登记的 `sendfunc`

### 5.6 completion / callback 的真实关系

这是当前实现里最容易混的地方。

当前版本里，完成关系已经明确收敛为一套：

1. 业务完成
   - DATA 全到后，对端显式发一个 ACT
   - 发送方收到 ACT 后，触发发送完成回调

也就是说：

- ACT 只是 3P 的业务控制报文
- 它不再承担 RoCE 可靠传输 completion 的含义

这也意味着当前版本更适合做“GPU 三阶段业务过程建模”，而不是完整替代真实 RNIC 的 RC 可靠传输。

## 6. 当前 Workflow 与 UB Workflow 的对比

下面把两边最核心的主链条并排说明。

### 6.1 当前项目的 workflow

1. Astra-Sim 发起 P2P 流
2. `ASTRASimNetwork::sim_send/sim_recv`
3. `my_entry::SendFlow`
4. `sendflowclient::TriggerSendTo_now`
5. `sendflowclient::SendPacket`
6. 叠加 `RoceSimHeader`
7. `FlowIo` 发到二层网络
8. 对端 `sendflowclient::HandleRead`
9. 解析 3P 头 + RoCE 头
10. 最终通过 REQUEST/DATA/ACT 回调 Astra-Sim

特点：

- 3P 是主流程
- RoCE 是语义旁路
- 完成依赖显式 ACT
- QP/FSM/CQ 只是增强约束，不是完整可靠传输核心

### 6.2 UB 的 workflow

UB 不是围绕 3P 做的，它更像：

1. `UbApp::SendTraffic(...)` 产生一个高层任务
2. 按任务类型分两条路：
   - `URMA_WRITE`：走 `UbTransaction + UbTransportChannel`
   - `MEM_STORE / MEM_LOAD`：走 `UbLdstInstance + UbLdstApi`
3. 发送侧把任务切成 `WqeSegment` 或 `TaskSegment`
4. `UbTransportChannel::GenDataPacket(...)` 或 `UbLdstApi::GenDataPacket(...)` 构造多层协议头
5. 交换机根据头类型把包分发到：
   - `RecvTpAck(...)`
   - `RecvDataPacket(...)`
   - `RecvResponse(...)`
6. 接收侧自动生成 TP ACK 或 transaction/read response
7. 发送侧收到 ACK 后推进：
   - `m_psnSndUna`
   - 重传计时器
   - WQE/task 完成回调

特点：

- WQE/Task 是主流程，不是 3P
- TP ACK 是协议内部自然产生的，不需要单独设计一个显式 ACT 业务包
- 有独立的发送窗口、ACK 队列、乱序位图、RTO 重传
- 应用层完成更接近“WQE 被传输层确认完成”

### 6.3 两边最本质的区别

| 维度 | 当前项目 | UB |
|---|---|---|
| 上层驱动对象 | Astra-Sim P2P/3P 流 | WQE / LDST Task |
| 主流程 | REQUEST -> DATA -> ACT | Data -> TP ACK / TA ACK / Response |
| 控制报文地位 | ACT 是显式业务控制报文 | ACK 主要是协议内部行为 |
| RoCE/URMA 语义位置 | 叠加在 3P 之上 | 事务层、传输层本身就是主模型 |
| QP/连接抽象 | `tag -> synthetic qpn`，轻量 QP | Jetty / TP / TPN，连接管理更完整 |
| 可靠性 | 只有轻量 PSN/QP/CQ 检查，无完整重传 | 有 ACK、发送窗口、RTO、重传、乱序处理 |
| 完成判定 | ACT 业务完成 | ACK 推进 `snd_una`，再完成 WQE/task |
| 更像什么 | 更像“带 RoCE 语义的 GPU 三阶段工作流” | 更像“数据中心传输协议栈行为仿真” |

### 6.4 哪个 workflow 更适合当前项目

如果目标是你当前这个项目的核心诉求，即：

- workload 仍由 Astra-Sim 主导
- 保留 GPU 的 REQUEST/DATA/ACT 三阶段观察口
- 在此基础上叠加 RoCE 语义

那么当前 workflow 更合适，因为它没有破坏你原来的业务时序。

如果目标改成：

- 更真实地模拟可靠传输
- 更真实地模拟 ACK/重传/乱序/拥塞闭环
- 让“完成”由传输层自然产生

那么 UB 的 workflow 更强，因为它的传输层闭环远比我们当前版本完整。

可以把两边的定位概括成一句话：

- 当前项目：以 3P 业务流为中心，RoCE 是增强层
- UB：以事务/传输协议为中心，应用任务是上层负载

## 7. QP 说明

### 7.1 QP 是什么

`QP` 是 `Queue Pair`，即“队列对”。在真实 RoCE/IB 中，它是一个通信端点。应用不会直接对“网卡整体”发 RDMA 操作，而是对某个 QP 投递 send/recv/read/write 请求。

从功能上看，一个 QP 可以理解成：

- 一条逻辑连接的本地状态
- 一组发送/接收队列
- 一套可靠传输上下文

### 7.2 QP 的核心组成

真实语义里的 QP 至少包含下面这些部分：

1. `SQ`，Send Queue
   - 发送队列
   - 保存发送工作请求 `WR/WQE`
   - 常见操作有 `SEND`、`RDMA WRITE`、`RDMA READ`

2. `RQ`，Receive Queue
   - 接收队列
   - 保存接收缓冲描述符
   - 对 `SEND` 语义尤其重要，因为接收端通常需要先贴好接收缓冲

3. `QP State`
   - QP 生命周期状态机
   - 典型状态包括：
     - `RESET`
     - `INIT`
     - `RTR`
     - `RTS`
     - `ERR`

4. `PSN` 相关状态
   - `PSN` 是包序号
   - 可靠传输通常要维护：
     - 下一个发送 PSN
     - 期望接收的下一个 PSN
   - 用于按序检查、ACK、重传和乱序处理

5. 队列深度和资源限制
   - `SQ depth`
   - `RQ depth`
   - 用于限制在途工作请求和接收资源

6. 连接属性
   - `QPN`
   - 对端 `QPN`
   - 传输类型，如 `RC / UC / UD`
   - MTU、重试、超时、访问权限等

### 7.3 与 QP 强相关但不完全属于 QP 本体的对象

这几个对象通常与 QP 一起出现：

1. `CQ`，Completion Queue
   - 保存 send/recv 完成事件
   - 应用通过 CQ 获知某个请求已经完成

2. `MR`
   - 已注册内存区域
   - `WRITE/READ` 语义要依赖注册内存和 `rkey`

3. `PD`
   - 保护域
   - 把 QP、CQ、MR 等对象组织在一起

### 7.4 当前项目里 QP 的简化方式

当前工程里的 QP 是轻量仿真版，运行时上下文定义在：

- `src/min-hop-mac-learning/model/roce-sim-types.h`

当前保留的主要字段只有：

- `state`
- `nextPsn`
- `expectedRxPsn`
- `sqDepth`
- `rqDepth`
- `txPackets`
- `rxPackets`

因此它当前主要承担的是：

- QP 状态机
- PSN 分配与顺序检查
- 基本发送/接收合法性校验
- 业务 ACT 到达后的 FSM 完成推进

而没有完整承担：

- 待发送 WQE 排队
- 发送窗口推进
- ACK/NAK 处理
- 超时重传
- 乱序缓存
- 完整 CQ 驱动的发送调度

### 7.5 当前项目里的 QPN 是什么

`QPN` 是 `Queue Pair Number`，即 QP 编号。真实系统里，接收端 RNIC 会根据包头中的 QPN 找到对应的 QP 上下文。

当前项目做了一个简化：

- `qpn = tag`

也就是一条 `tag/src/dst` flow 对应一条仿真的 QP。这样做的目的，是用最小代价把 QP 状态、PSN 和 completion 路径挂起来。

## 8. 当前项目与 UB 在 QP/传输端点职责上的对比

### 8.1 当前项目里的 QP 现在承担什么职责

当前项目里的 QP 更接近“协议语义容器”，而不是“调度器”。

现在它负责的是：

- 维护 `Reset -> Init -> Rtr -> Rts -> Err` 状态
- 维护发送 `PSN` 和期望接收 `PSN`
- 构造 `RoceSimHeader`
- 校验接收头是否合法
- 推进 3P 业务完成

它不负责：

- 决定什么时候真正出包
- 多个待发请求之间的仲裁
- inflight window 限制
- ACK 驱动的发送推进
- 超时重传

所以当前的“什么时候发包”，主要还是由下面几层决定：

- Astra-Sim / `my_entry`
- `sendflowclient::TriggerSendTo_now`
- `sendflowclient::SendPacket`
- 底层网络队列和设备调度

### 8.2 UB 那边如何处理

UB 那边没有直接照搬 RoCE QP 命名，但它的 `UbTransportChannel` 在职责上更像一个真正参与调度的传输端点。

它同时承担了下面这些职责：

1. 自己就是一个可被调度的 ingress queue
   - `UbTransportChannel` 继承自 `UbIngressQueue`
   - 可以直接回答“现在有没有包可发”“下一包是什么”

2. 内部有真实的发送选择逻辑
   - ACK 队列优先
   - 再从 `WqeSegment` 队列中选数据
   - 再检查 inflight 限制和拥塞窗口

3. ACK 会真实推进发送窗口
   - 收到 ACK 后更新 `m_psnSndUna`
   - 解除 window limited 状态
   - 重置重传定时器

4. 有真实的重传逻辑
   - `RTO` 到期后回退发送指针
   - 重置 segment 的已发送字节
   - 重新触发发送

5. 更上层还有 `UbTransaction` 做 segment 轮询和绑定
   - 负责把 WQE segment 分配给具体 TP
   - 具有更明确的多任务调度含义

### 8.3 一句话概括两边的差异

- 当前项目的 QP：更像“RoCE 协议状态检查器 + header/PSN/completion 附加器”
- UB 的 TP/Jetty 体系：更像“真正参与发送调度的 transport endpoint”

### 8.4 为什么客户可能会有不同感受

从客户视角看，只要系统里出现了“QP”这个词，通常就会自然期待它至少承担这些职责：

- 有队列
- 有在途请求管理
- 有 ACK 推进
- 有 completion
- 有可靠发送状态

而当前项目里，QP 已经具备一部分“协议语义”，但还没有真正接管“传输调度”。

因此更准确的对外说法应该是：

- 当前项目已经有轻量 QP 语义层
- 但尚未把完整的发送调度、窗口和重传 fully 下沉到 QP 层
