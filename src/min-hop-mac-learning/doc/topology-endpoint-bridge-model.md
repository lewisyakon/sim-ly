# GPU 端点双 Bridge / 外部 Bridge 模型

最近修改：2026-04-23

记录时间：2026-04-23

这份文档记录当前 GPU 端点的拓扑建模方式，重点回答 4 个问题：

1. 为什么现在要把端点统一成“内侧 bridge + 可选外侧 bridge”的模型。
2. 外侧 bridge 在当前代码里到底是不是一个真正的 transit bridge。
3. `FlowIo`、`MinHopBridgeNetDevice`、`RdmaPortEngine` 和 PFC 现在怎么分工。
4. 这次 FatTree/Clos 迁移里新增的 `TerminalLearningOnly` 到底做了什么。

## 1. 为什么统一成双 bridge 端点

当前项目的目标不是只跑单一拓扑，而是把 testbed 逐步推向更大规模、更多端点口型、更复杂的内外网组合。

因此当前采用的原则是：

1. 端点模型优先对齐“未来可扩展”的多口/内外网形态。
2. 多口退化成单口容易，单口再升级到多口则会牵动上层发包逻辑、收包绑定、学表、PFC 和日志口径。
3. HPN 已经证明端点需要同时表达：
   - 内部高带宽域
   - 面向后端网络的外部口/外部 rail
   - 外部口只承担端点出入网，不一定承担 transit 转发

这也是为什么当前把 HPN 上已经收敛出来的端点模型，继续复用到 FatTree/Clos 的外部 bridge 迁移上。

## 2. 当前端点布局

当前 GPU 端点有两种布局：

### 2.1 BridgeOnly

只有一个内侧 bridge：

```text
sendflowclient / FlowIo
  -> innerBridge
  -> selected outPort
```

这仍适用于没有独立外部 bridge 的拓扑。

### 2.2 BridgeAndExternal

同时存在 `innerBridge` 和 `externalBridge`：

```text
sendflowclient
  -> FlowIo binds innerBridge socket
  -> FlowIo binds externalBridge socket
  -> innerBridge: 内部域、主 endpoint MAC
  -> externalBridge: 外侧 egress manager
       -> one or more external rail p2p ports
```

当前实现里的几个关键事实：

1. `sendflowclient::initialization(...)` 在这种模式下会同时绑定 `innerBridgeIfIndex` 和 `externalBridgeIfIndex`。
2. endpoint MAC 统一取 `innerBridge` 的 MAC，而不是外侧 rail 的 MAC。
3. `externalRailIfIndices` 现在已经建模保存下来，但 `BuildExternalSubflowPlan(...)` 仍返回单条子流，暂时没有做真正的多 rail striping。

对应代码：

- [scratch/my_common.h](/home/lja/sim-ly/sim-ly/scratch/my_common.h)
- [my_sendflowclient.cc](/home/lja/sim-ly/sim-ly/src/min-hop-mac-learning/model/my_sendflowclient.cc)

## 3. 外侧 bridge 的当前语义

当前外侧 bridge 的角色不是“任何场景都像内部交换机那样负责 transit 转发”。

更准确地说，它是：

- GPU 端点的外部出入口管理器
- 学习外部路由信息的载体
- 外发时把本地注入解析到真实物理 `outPort` 的对象

只有在需要真实 transit 行为的 bridge 上，才应该让它承担完整转发职责。对 GPU 端点外侧 bridge 而言，当前迁移目标是：

1. 保留 bridge 的学表能力。
2. 保留桥上 `ResolveLocalInjectionEgressPort(...)` 的解析逻辑。
3. 不让它把“从外部 port 收到的 transit flood/广播/多播”继续当成一个普通转发表项去转发。

这个边界正是通过 `TerminalLearningOnly` 实现的。

## 4. `TerminalLearningOnly` 的语义

### 4.1 它做什么

`TerminalLearningOnly=true` 时，bridge 仍然会：

1. 在 flood 包上学习源 MAC 到 ingress 口的映射。
2. 对本地收包继续执行正常 `PACKET_HOST` 上送。
3. 对本地注入继续执行 egress 解析。

但它不会把“从真实 bridge port 进来的 transit 包”继续往其他 bridge port 转发。

当前实现里，这个模式还有一个配套行为：

- 对这类 transit 包，不再进入普通 bridge 的 `NO_PATH -> retry` 逻辑。

也就是说，终端 bridge 会“学习，然后丢弃”，而不是“没路径就重试等待后续 flood 学到路”。

### 4.2 为什么只给外侧 bridge 用

这个模式只应该给外侧 bridge 用，不应该给内部 bridge 普遍开启。

原因很直接：

1. 外侧 bridge 的主要职责是端点出入网与学表。
2. 内部 bridge 在某些拓扑里真的要承担 transit 转发，例如 torus 这类内网拓扑。
3. 如果把内部 bridge 也做成 terminal learning-only，会把真实内网转发语义直接切断。

当前代码里，`TerminalLearningOnly` 只在 `CreateGpuExternalBridge(...)` 上打开。

## 5. 现在的发送路径怎么走

### 5.1 Legacy / 非 RoCE 路径

legacy 路径仍然主要通过 `FlowIo` 的 socket/device 发送。

此时：

1. `FlowIo` 根据 `FlowSendMode` 选择 `innerBridge` 或 `externalBridge`。
2. 如果发送失败或者本地软件队列已暂停，会进入 `FlowIo` 的软件 buffer。
3. 后续通过 `DrainBuffered()` 重试。

### 5.2 RoCE 路径

RoCE 主路径不再靠 `FlowIo` 的 buffer/runtime 推进，而是：

```text
sendflowclient
  -> RoceTransportEngine::PrepareTxWork
  -> ResolveRoceTxPortEngine_
  -> selected outPort's RdmaPortEngine
  -> PointToPointNetDevice::SendFrom
```

当前 `ResolveRoceTxPortEngine_()` 的行为是：

1. `External`
   - 先尝试把 `m_outDeviceIndex` 直接当物理 `PointToPointNetDevice`
   - 如果 `m_outDeviceIndex` 其实是 `externalBridge`，则让 `externalBridge` 调 `ResolveLocalInjectionEgressPort(...)` 找真实 `outPort`

2. `Loopback`
   - 若 loopback 落在 `m_outDeviceIndex`，走外侧逻辑

3. `Internal / 其他 Loopback`
   - 通过 `m_bridgeDevice`，也就是 `innerBridge`，解析真实 `outPort`

这意味着：

1. 外侧 bridge 已经进入 RoCE 外发解析链。
2. 但真正的 packet runtime 仍然只在物理口上的 `RdmaPortEngine`。

## 6. 为什么 `FlowIo` 现在还保留 buffer

`FlowIo` 现在仍保留 software buffer，不是因为它还是 RoCE 的主 runtime，而是因为它还负责两类事情：

1. legacy / 非 RoCE 的 socket 发送重试。
2. 本地注入路径上的软件级 pause/backpressure 适配。

当前 `FlowIo::Send(...)` 的优先级是：

1. 如果包已经带 `RdmaDispatchTag`，并且目标设备是带 `RdmaPortEngine` 的物理 `PointToPointNetDevice`
   - 先尝试 `TrySubmitViaRdmaPortEngine(...)`
2. 否则走原有 `SendOnDevice(...)`
3. 如果发送失败或 queue 已暂停，再进入 `m_txBuffer`

所以当前保留 buffer 是有明确边界的：

- 它是 legacy/fallback/local-pause 机制
- 不是 RoCE 主窗口、主 ACK、主重传状态机

## 7. PFC 现在分成哪两层

当前 PFC 相关行为分成两层，不是重复实现。

### 7.1 bridge / 本地注入层

`MinHopBridgeNetDevice` 会根据 ingress queue 和 egress queue 的水位做两类动作。

当前 bridge ingress 缓冲模型是：

1. 每个 ingress port 一个 aggregate queue。
2. 这个 aggregate queue 的容量等于该端口对应物理 egress queue 的 `NumQueues * MaxPacketsPerQueue`。
3. 包入队时只占用这个 aggregate queue 的一个计数，并记录独立 packet token。
4. 包在经过 `switch_delay` 后按自己的 token drain，不再依赖 `PppRoundRobinQueue::Peek/Dequeue` 的队首选择。
5. 如果 drain 时发现目标 egress 因 PFC 水位不可用，保留该 token 并按 `switch_delay` retry。

这个模型的含义是：bridge ingress 只模拟“到达 bridge 后的处理延迟和有限缓冲”，不在 ingress 侧再次实现按优先级 qid 的调度。qid 优先级仍由物理 egress queue 和 RoCE port runtime 负责。

PFC 动作分为：

1. `LOCAL`
   - 当 `incomingPort == nullptr`，说明这是本地注入
   - bridge 通过 `FlowIo::PauseNodeQueue(...)` / `ResumeNodeQueue(...)` 控制节点本地软件发包队列
   - aggregate ingress queue 达到高/低水位后，会对本地配置的全部 qid 执行 pause/resume

2. `NONLOCAL`
   - 当包来自真实 ingress p2p 口
   - bridge 会构造 `PauseHeader` 并向上游物理口回发 pause/resume
   - aggregate ingress queue 达到高/低水位后，会向上游物理口的各 qid 扇出 pause/resume

历史兼容项：

- `pfc.not_head_retry_ns` / `NotHeadRetryDelay` 仍可被配置，但当前是兼容 no-op。
- `logging.bridge_diag` / `BridgeDiagEnable` 仍可被配置，但当前不再输出旧的 bridge residence 诊断。

### 7.2 物理口 / RoCE runtime 层

`PointToPointNetDevice` 收到 `PauseHeader` 后，会更新自己的 `QueuePauseState` trace。

随后：

1. `RdmaPortEngine` 通过 `OnQueuePauseState_(...)` 感知 qid pause/resume
2. pause 时阻止对应 qid 继续准入
3. resume 后重新 drain pending work

因此现在的分工是：

1. bridge 负责局部队列水位与 pause frame 传播
2. `FlowIo` 负责本地软件发送路径的暂停/恢复
3. `RdmaPortEngine` 负责物理口上的 RoCE 发送准入

## 8. 日志现在该怎么理解

### 8.1 `[BRIDGE][RETRY][NO_PATH]`

这表示一个普通 forwarding bridge 的某个 ingress 包在完成 `switch_delay` 后，当前找不到可用转发路径，因此不会立刻丢包，而是保留在 ingress token 表里等待后续 retry。

当前 `TerminalLearningOnly` 的一个修正点就是：

- 终端外侧 bridge 上的 transit 包不再进入这条 retry 链。

### 8.2 `[BRIDGE][TERMINAL][DROP]`

这表示：

1. 包是从真实 ingress port 进入终端外侧 bridge 的
2. bridge 已经利用它完成必要的学习
3. 但该 bridge 不承担后续 transit 转发，所以直接停止转发

当前代码里这条日志模板仍然保留在 terminal-drop 分支里，但默认 `NS_LOG_INFO` 输出已注释，避免在大规模 flood 学习阶段制造大量噪声。因此：

1. 看到它，说明 terminal 分支在工作
2. 看不到它，不代表 terminal 分支不存在

## 9. 当前还没做完的地方

1. `externalRailIfIndices` 已经落代码，但还没有真正做多 rail striping。
2. 每个 work 仍只会选一个物理 egress port，不会做单 work 多口并发。
3. `TerminalLearningOnly` 目前只对 GPU 外侧 bridge 开启，没有抽象成更广义的 endpoint role 体系。

## 10. HPN 参考材料

这次端点模型整理参考了本地两份 HPN 材料：

1. `Alibaba HPN - A Data Center Network for Large Language Model Training.pdf`
2. `alibaba.pptx`

当前从材料中吸收的、并已经反映到代码/文档里的高层约束主要是：

1. HPN 后端是两层、双平面、非堆叠双 ToR 的端点/网络分层模型。
2. 主机侧同时存在内部高带宽域和面向后端网络的外部口。
3. rail/多口建模应当优先在端点层表达出来，而不是等以后再从单口模型硬升级。

这份文档只记录这些参考如何影响当前实现，不试图在仓库里复述整篇 HPN 论文。
