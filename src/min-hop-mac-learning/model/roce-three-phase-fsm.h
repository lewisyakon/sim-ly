#ifndef ROCE_THREE_PHASE_FSM_H
#define ROCE_THREE_PHASE_FSM_H

#include <cstdint>
#include <map>

#include "roce-sim-types.h"

namespace ns3 {

// flow 级键：沿用 tag/src/dst 三元组。
struct RoceFlowKey {
  uint32_t tag = 0;
  uint32_t src = 0;
  uint32_t dst = 0;

  bool operator<(const RoceFlowKey& other) const {
    if (tag != other.tag) {
      return tag < other.tag;
    }
    if (src != other.src) {
      return src < other.src;
    }
    return dst < other.dst;
  }
};

// 3P 语义状态（用于 REQUEST/DATA/ACT 生命周期跟踪）。
enum class ThreePhaseState : uint8_t {
  Idle = 0,
  RequestIssued = 1,
  DataIssued = 2,
  Completed = 3,
};

// 轻量 flow 状态机：
// 只跟踪阶段推进，不控制发送时序。
class RoceThreePhaseFsm {
public:
  RoceThreePhaseFsm() = default;

  // 配置注入（主要用到 enable/dataUseWrite）。
  void Configure(const RoceConfig& cfg);

  // 清空所有 flow 或单个 flow 状态。
  void Reset();
  void ResetFlow(const RoceFlowKey& key);

  // 对应 3P 三个阶段的事件输入。
  void OnRequestIssued(const RoceFlowKey& key);
  void OnDataIssued(const RoceFlowKey& key);
  void OnActReceived(const RoceFlowKey& key);

  // 查询接口。
  ThreePhaseState GetState(const RoceFlowKey& key) const;
  bool IsCompleted(const RoceFlowKey& key) const;

  // 配置透传：是否启用 + DATA 阶段默认 opcode 选择。
  bool IsEnabled() const;
  RoceOpcode DataOpcode() const;

private:
  RoceConfig m_cfg;
  std::map<RoceFlowKey, ThreePhaseState> m_states; // flow 当前状态
};

} // namespace ns3

#endif // ROCE_THREE_PHASE_FSM_H
