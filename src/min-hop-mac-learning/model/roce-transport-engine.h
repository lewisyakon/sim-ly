#ifndef ROCE_TRANSPORT_ENGINE_H
#define ROCE_TRANSPORT_ENGINE_H

#include <cstdint>
#include <map>

#include "3PHeader.h"
#include "roce-three-phase-fsm.h"
#include "roce-tx-scheduler.h"

namespace ns3 {

// RoCE transport 逻辑层：
// - 维护 flow/lane -> qpn 映射
// - 维护 3P 语义状态机
// - 为 port engine 准备待发 WR/work item
class RoceTransportEngine {
public:
  RoceTransportEngine() = default;
  ~RoceTransportEngine();

  void SetNodeId(uint32_t nodeId);

  void Configure(const RoceConfig& cfg);
  void Reset();

  bool PrepareTxWork(RoceTxWorkItem* item);
  void OnActReceived(const RoceFlowKey& flowKey);

private:
  enum class RoceLaneRole : uint8_t {
    ReqCtrl = 0,
    Data = 1,
    ActCtrl = 2,
  };

  struct FlowLaneKey {
    RoceFlowKey flowKey;
    RoceLaneRole lane = RoceLaneRole::Data;

    bool operator==(const FlowLaneKey& other) const = default;

    bool operator<(const FlowLaneKey& other) const {
      if (flowKey != other.flowKey) {
        return flowKey < other.flowKey;
      }
      return static_cast<uint8_t>(lane) < static_cast<uint8_t>(other.lane);
    }
  };

  static RoceLaneRole GetLaneRole_(ThreePHeader::State state);
  uint32_t GetOrAllocQpn_(const RoceFlowKey& flowKey, RoceLaneRole lane);

private:
  RoceConfig m_cfg;
  RoceThreePhaseFsm m_fsm;

  std::map<FlowLaneKey, uint32_t> m_flowToQpn;

  uint32_t m_nextSyntheticQpn = 1;
  uint64_t m_nextWorkId = 1;
  uint32_t m_nodeId = 0;
  bool m_hasNodeId = false;
};

} // namespace ns3

#endif // ROCE_TRANSPORT_ENGINE_H
