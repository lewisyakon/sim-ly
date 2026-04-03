# RoCE 后续调度方案备忘

这份文档只记录两类后续候选方案，以及为什么当前决定暂不实现。

## 1. 当前决定

当前决定是：

- 先不继续深入改调度层
- 当前版本停在“发送入口已进入 QP / scheduler 层，但尚未下沉到真实设备发送机会”这个状态

原因：

1. 接收侧乱序问题已经修到可接受
2. 再往下做调度需要动到底层设备或引入额外 pacing 近似
3. 当前收益抵不过改动和维护代价

## 2. 方案 A：基于真实发送完成事件的 Pull 调度

### 2.1 思想

把 RoCE scheduler 的触发时机，从：

- `sendflowclient` 自己反复 `Schedule(0)` 推包

改成：

- 只有当下层设备真正完成一次发送、再次 ready 时
- 才由下层回调触发 RoCE scheduler 选择下一个 slice

### 2.2 需要改哪些层

1. `PointToPointNetDevice`
   - 在 `TransmitComplete()` 时提供向上的 callback 或 trace source

2. `FlowIo`
   - 变成 RoCE producer 和设备之间的 broker
   - 在设备 ready 时向上拉一个 slice

3. `sendflowclient`
   - 不再自驱动 `Schedule(0)` dispatch
   - 改成提供“取下一个 slice”的接口

### 2.3 优点

1. 最接近真实 NIC / port dequeue 语义
2. 链路速率和包长会自然反映到调度节拍
3. 更容易对客户解释“QP scheduler 真正在控制发送机会”

### 2.4 代价

1. 改动会深入到底层设备
2. `FlowIo` 职责会明显变复杂
3. 需要重新梳理与 `PppRoundRobinQueue` 的职责边界

## 3. 方案 B：上层 per-slice pacing 的折中方案

### 3.1 思想

不改底层设备，只在当前 `sendflowclient -> RoceTxScheduler` 这一层做人为节流。

也就是：

- 每次调度只放出有限个 slice
- 下一轮调度不再是 `0ns`
- 而是等待一个估算出的发送时间后再继续

### 3.2 更合理的 pacing 方式

如果真做这个方案，不应使用随意常数等待时间。

更合理的近似是：

- `dispatch_gap = 该 slice 的实际包长 / 链路速率 (+ IFG)`

这样至少不会平白引入过多空泡。

### 3.3 优点

1. 改动面比方案 A 小很多
2. 不需要改 `PointToPointNetDevice`
3. 很快就能让多 QP RR 在日志里变得可见

### 3.4 缺点

1. 仍然不是基于真实设备 ready
2. 只是软件估算，不是实际 wire-time completion
3. 容易和下层设备队列形成双重调度
4. 解释力度弱于方案 A

## 4. 为什么当前不实现方案 B

用户这轮的判断是合理的：

- 当前再改动的收益，抵不过继续改底层或引入 pacing 近似的代价

我同意这个判断。

因此当前分支只保留：

- 已有的 stage 1 scheduler
- 已修正的接收侧乱序容忍

而把两种后续调度方案都记录下来，留待以后重新评估。
