#include "roce-three-phase-fsm.h"

namespace ns3 {

void
RoceThreePhaseFsm::Configure(const RoceConfig& cfg) {
  m_cfg = cfg;
}

void
RoceThreePhaseFsm::Reset() {
  // 清理所有 flow 状态。
  m_states.clear();
}

void
RoceThreePhaseFsm::ResetFlow(const RoceFlowKey& key) {
  m_states.erase(key);
}

void
RoceThreePhaseFsm::OnRequestIssued(const RoceFlowKey& key) {
  // REQUEST 触发后进入 RequestIssued。
  m_states[key] = ThreePhaseState::RequestIssued;
}

void
RoceThreePhaseFsm::OnDataIssued(const RoceFlowKey& key) {
  // DATA 发出后进入 DataIssued。
  m_states[key] = ThreePhaseState::DataIssued;
}

void
RoceThreePhaseFsm::OnActReceived(const RoceFlowKey& key) {
  // 收到 ACT 业务控制报文后认为该 flow 完成。
  m_states[key] = ThreePhaseState::Completed;
}

ThreePhaseState
RoceThreePhaseFsm::GetState(const RoceFlowKey& key) const {
  auto it = m_states.find(key);
  if (it == m_states.end()) {
    return ThreePhaseState::Idle;
  }
  return it->second;
}

bool
RoceThreePhaseFsm::IsCompleted(const RoceFlowKey& key) const {
  return GetState(key) == ThreePhaseState::Completed;
}

bool
RoceThreePhaseFsm::IsEnabled() const {
  return m_cfg.enable;
}

RoceOpcode
RoceThreePhaseFsm::DataOpcode() const {
  // DATA 阶段 opcode：可在 WRITE 和 SEND 之间切换。
  return m_cfg.dataUseWrite ? RoceOpcode::RdmaWrite : RoceOpcode::Send;
}

} // namespace ns3
