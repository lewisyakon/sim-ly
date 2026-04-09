#include "roce-transport-engine.h"

#include <cctype>

namespace ns3 {

namespace {

std::string
ToLowerCopy(std::string v) {
  for (char& c : v) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return v;
}

RoceUserPhase
ToRoceUserPhase(ThreePHeader::State state) {
  switch (state) {
    case ThreePHeader::REQUEST:
      return RoceUserPhase::Request;
    case ThreePHeader::DATA:
      return RoceUserPhase::Data;
    case ThreePHeader::ACT:
      return RoceUserPhase::Act;
    default:
      return RoceUserPhase::None;
  }
}

RoceOpcode
ToRoceOpcode(ThreePHeader::State state,
             const RoceConfig& cfg,
             const RoceThreePhaseFsm* fsm) {
  const std::string mode = ToLowerCopy(cfg.modeName);
  const bool preferRead = mode.find("read") != std::string::npos;
  const bool preferSend = mode.find("send") != std::string::npos;

  switch (state) {
    case ThreePHeader::REQUEST:
      return preferRead ? RoceOpcode::RdmaReadReq : RoceOpcode::Send;
    case ThreePHeader::DATA:
      if (preferRead) {
        return RoceOpcode::RdmaReadResp;
      }
      if (preferSend) {
        return RoceOpcode::Send;
      }
      if (fsm != nullptr) {
        return fsm->DataOpcode();
      }
      return RoceOpcode::RdmaWrite;
    case ThreePHeader::ACT:
      return RoceOpcode::Send;
    default:
      return RoceOpcode::Send;
  }
}

} // namespace

RoceTransportEngine::~RoceTransportEngine() {
  Reset();
}

void
RoceTransportEngine::SetNodeId(uint32_t nodeId) {
  m_nodeId = nodeId;
  m_hasNodeId = true;
}

void
RoceTransportEngine::Configure(const RoceConfig& cfg) {
  m_cfg = cfg;
  m_fsm.Configure(m_cfg);
}

void
RoceTransportEngine::Reset() {
  m_flowToQpn.clear();
  m_nextSyntheticQpn = 1;
  m_nextWorkId = 1;
  m_fsm.Reset();
}

RoceTransportEngine::RoceLaneRole
RoceTransportEngine::GetLaneRole_(ThreePHeader::State state) {
  switch (state) {
    case ThreePHeader::REQUEST:
      return RoceLaneRole::ReqCtrl;
    case ThreePHeader::DATA:
      return RoceLaneRole::Data;
    case ThreePHeader::ACT:
      return RoceLaneRole::ActCtrl;
    default:
      return RoceLaneRole::Data;
  }
}

bool
RoceTransportEngine::PrepareTxWork(RoceTxWorkItem* item) {
  if (item == nullptr || !m_cfg.enable) {
    return false;
  }

  const RoceLaneRole lane = GetLaneRole_(item->state);
  if (item->qpKey.qpn == 0) {
    item->qpKey.qpn = GetOrAllocQpn_(item->flowKey, lane);
  }
  if (item->workId == 0) {
    item->workId = m_nextWorkId++;
  }
  item->userPhase = ToRoceUserPhase(item->state);
  item->opcode = ToRoceOpcode(item->state, m_cfg, &m_fsm);
  item->txClass =
      (item->state == ThreePHeader::DATA) ? RoceTxClass::Data : RoceTxClass::Control;

  if (item->state == ThreePHeader::REQUEST) {
    m_fsm.OnRequestIssued(item->flowKey);
  } else if (item->state == ThreePHeader::DATA) {
    m_fsm.OnDataIssued(item->flowKey);
  }
  return true;
}

void
RoceTransportEngine::OnActReceived(const RoceFlowKey& flowKey) {
  m_fsm.OnActReceived(flowKey);
}

uint32_t
RoceTransportEngine::GetOrAllocQpn_(const RoceFlowKey& flowKey,
                                    RoceLaneRole lane) {
  const FlowLaneKey flowLaneKey{flowKey, lane};
  auto it = m_flowToQpn.find(flowLaneKey);
  if (it != m_flowToQpn.end()) {
    return it->second;
  }

  uint32_t qpn = m_nextSyntheticQpn++;
  if (qpn == 0) {
    qpn = m_nextSyntheticQpn++;
  }
  m_flowToQpn[flowLaneKey] = qpn;
  return qpn;
}

} // namespace ns3
