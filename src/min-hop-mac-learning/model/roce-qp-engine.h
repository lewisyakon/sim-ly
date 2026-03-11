#ifndef ROCE_QP_ENGINE_H
#define ROCE_QP_ENGINE_H

#include <cstdint>
#include <map>
#include <queue>

#include "roce-sim-header.h"
#include "roce-sim-types.h"

namespace ns3 {

// RoCE QP 引擎（轻量仿真版）：
// - 维护 QP 状态机与 PSN
// - 生成发送头
// - 接收侧校验并产出 completion
class RoceQpEngine {
public:
  RoceQpEngine() = default;

  // 注入配置并清理运行态。
  void Configure(const RoceConfig& cfg);
  void Reset();

  // QP 生命周期管理。
  bool EnsureQp(const RoceQpKey& key);
  bool ModifyQpState(const RoceQpKey& key, RoceQpState target);
  RoceQpState GetQpState(const RoceQpKey& key) const;

  // 发送/接收是否允许投递。
  bool CanPostSend(const RoceQpKey& key) const;
  bool CanPostRecv(const RoceQpKey& key) const;

  // 根据当前 QP 状态和 PSN 生成一个发送头。
  bool BuildSendHeader(const RoceQpKey& key,
                       RoceOpcode opcode,
                       RoceUserPhase phase,
                       uint32_t payloadBytes,
                       uint32_t tag,
                       RoceSimHeader* out);

  // 处理接收头并做基础合法性检查（状态、opcode、PSN、长度）。
  bool OnRxHeader(const RoceQpKey& key,
                  const RoceSimHeader& header,
                  RoceCompletion* completion);

  // Completion 队列接口。
  void NotifyTxCompletion(const RoceQpKey& key, uint32_t wrId, bool success);
  bool PopCompletion(RoceCompletion* completion);

private:
  // QP 状态转移合法性判定。
  bool IsStateTransitionAllowed(RoceQpState from, RoceQpState to) const;

private:
  RoceConfig m_cfg;
  std::map<RoceQpKey, RoceQpContext> m_qps; // 所有活跃 QP 上下文
  std::queue<RoceCompletion> m_completions; // 简化 CQ
};

} // namespace ns3

#endif // ROCE_QP_ENGINE_H
