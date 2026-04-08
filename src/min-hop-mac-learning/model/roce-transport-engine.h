#ifndef ROCE_TRANSPORT_ENGINE_H
#define ROCE_TRANSPORT_ENGINE_H

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <vector>

#include "3PHeader.h"
#include "flow-io.h"
#include "ns3/address.h"
#include "ns3/event-id.h"
#include "ns3/packet.h"
#include "roce-qp-engine.h"
#include "roce-sim-header.h"
#include "roce-three-phase-fsm.h"
#include "roce-tx-scheduler.h"

namespace ns3 {

// RoCE transport 运行时：
// - 持有 QPN/QP/FSM/调度器
// - 管理 ACK、超时重传、reorder upper buffer
// - 对外暴露“待发动作”和“收包结果”，由 sendflowclient 做 I/O 与业务桥接
class RoceTransportEngine {
public:
  using DispatchWakeupFn = std::function<void()>;

  struct OutstandingPacket {
    Ptr<Packet> packet;
    Address dest;
    FlowSendMode sendMode = FlowSendMode::External;
    int mindex = 0;
    RoceFlowKey flowKey;
    RoceQpKey qpKey;
    uint32_t psn = 0;
    uint32_t retries = 0;
  };

  struct BufferedUpperPacket {
    Ptr<Packet> payload;
    ThreePHeader tpHeader;
    Address from;
    uint32_t formIndex = 0;
    bool hasTransportFragmentId = false;
    uint32_t transportFragmentId = 0;
  };

  enum class DispatchKind : uint8_t {
    None = 0,
    Retransmission = 1,
    NewSlice = 2,
  };

  struct DispatchAction {
    DispatchKind kind = DispatchKind::None;
    RoceTxSlice slice;
    OutstandingPacket retransmission;
  };

  struct RxResult {
    bool accepted = false;
    bool needAck = false;
    bool bufferedForReorder = false;
    bool overflowedReorderWindow = false;
    std::vector<BufferedUpperPacket> readyPackets;
  };

  RoceTransportEngine() = default;
  ~RoceTransportEngine();

  void SetDispatchWakeup(DispatchWakeupFn wakeup);
  void SetNodeId(uint32_t nodeId);

  void Configure(const RoceConfig& cfg);
  void Reset();

  bool HasPendingWork() const;
  bool HasPendingSchedulerWork() const;
  bool HasPendingRetransmissions() const;

  bool Enqueue(RoceTxWorkItem* item);
  bool TryGetNextDispatchAction(uint32_t mtu, DispatchAction* out);

  bool BuildTxHeader(ThreePHeader::State state,
                     uint64_t totalDataSize,
                     uint32_t payloadBytes,
                     uint32_t tag,
                     uint32_t src,
                     uint32_t dst,
                     RoceSimHeader* headerOut);
  bool BuildAckHeader(uint32_t tag,
                      uint32_t src,
                      uint32_t dst,
                      uint32_t qpn,
                      RoceSimHeader* headerOut);

  void OnPacketSent(const RoceTxSlice& slice,
                    const Ptr<Packet>& packet,
                    const RoceSimHeader& header);
  void OnPacketWireTx(const RoceQpKey& qpKey,
                      uint32_t psn,
                      bool isRetransmission);
  void OnRetransmissionSent(const RoceQpKey& qpKey);

  RxResult OnRxPacket(const RoceSimHeader& roceHeader,
                      const ThreePHeader& tpHeader,
                      const Ptr<Packet>& payload,
                      const Address& from,
                      uint32_t formIndex);

  uint32_t GetExpectedRxPsn(const RoceQpKey& qpKey) const;
  uint32_t GetPendingRxCount(const RoceQpKey& qpKey) const;
  uint32_t GetOutstandingCount(const RoceQpKey& qpKey) const;
  uint32_t GetOldestOutstandingPsn(const RoceQpKey& qpKey) const;
  uint32_t GetRecoveryTargetPsn(const RoceQpKey& qpKey) const;

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

  struct RetransTimerState {
    EventId event;
    uint32_t trackedPsn = 0;
  };

  static RoceLaneRole GetLaneRole_(ThreePHeader::State state);
  static const char* GetLaneName_(RoceLaneRole lane);
  uint32_t GetOrAllocQpn_(const RoceFlowKey& flowKey, RoceLaneRole lane);
  void RememberQpn_(const RoceFlowKey& flowKey, RoceLaneRole lane, uint32_t qpn);
  bool EnsureQpReadyToSend_(const RoceQpKey& key);

  void HandleAck_(const RoceFlowKey& flowKey,
                  const RoceQpKey& qpKey,
                  const RoceSimHeader& roceHeader);
  void HandleRetransTimeout_(RoceQpKey qpKey);
  void ArmRetransTimer_(const RoceQpKey& qpKey);
  void CancelRetransTimer_(const RoceQpKey& qpKey);

  uint32_t GetTransportWindowPackets_() const;
  std::set<RoceQpKey> CollectBlockedQps_() const;
  void CollectReadyBufferedPackets_(const RoceQpKey& qpKey,
                                    std::vector<BufferedUpperPacket>* out);
  void NotifyDispatchNeeded_();

private:
  RoceConfig m_cfg;
  RoceQpEngine m_qpEngine;
  RoceThreePhaseFsm m_fsm;
  RoceTxScheduler m_txScheduler;

  std::map<FlowLaneKey, uint32_t> m_flowToQpn;
  std::map<RoceQpKey, std::map<uint32_t, OutstandingPacket>> m_outstandingPackets;
  std::map<RoceQpKey, RetransTimerState> m_retransTimers;
  std::map<RoceQpKey, std::map<uint32_t, BufferedUpperPacket>> m_bufferedUpperPackets;
  std::map<RoceQpKey, OutstandingPacket> m_pendingRetransmissions;
  std::map<RoceQpKey, uint32_t> m_recoveryTargetPsns;

  uint32_t m_nextSyntheticQpn = 1;
  uint64_t m_nextWorkId = 1;
  DispatchWakeupFn m_dispatchWakeup;
  uint32_t m_nodeId = 0;
  bool m_hasNodeId = false;
};

} // namespace ns3

#endif // ROCE_TRANSPORT_ENGINE_H
