#ifndef RDMA_PORT_ENGINE_H
#define RDMA_PORT_ENGINE_H

#include "ns3/callback.h"
#include "ns3/event-id.h"
#include "ns3/object.h"
#include "ns3/packet.h"
#include "ns3/point-to-point-net-device.h"
#include "ns3/address.h"
#include "ns3/event-id.h"
#include "3PHeader.h"
#include "FlowTagHeader.h"
#include "roce-qp-engine.h"
#include "roce-sim-header.h"
#include "roce-sim-types.h"
#include "roce-tx-scheduler.h"

#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <vector>

namespace ns3 {

class RdmaPortEngine : public Object {
public:
  using TxNotifyCallback = Callback<void, Ptr<Packet>>;

  static TypeId GetTypeId();

  RdmaPortEngine();
  ~RdmaPortEngine() override;

  void AttachToDevice(Ptr<PointToPointNetDevice> device);
  void Configure(const RoceConfig& cfg);
  void Reset();
  void SetTxNotifyCallback(TxNotifyCallback callback);
  void SetNodeId(uint32_t nodeId);
  bool PostSend(const RoceTxWorkItem& item);

  struct BufferedUpperPacket {
    Ptr<Packet> payload;
    ThreePHeader tpHeader;
    Address from;
    uint32_t formIndex = 0;
    bool hasTransportFragmentId = false;
    uint32_t transportFragmentId = 0;
    uint32_t rqPsn = 0;
  };

  struct RxResult {
    bool accepted = false;
    bool bufferedForReorder = false;
    bool overflowedReorderWindow = false;
    std::vector<BufferedUpperPacket> readyPackets;
  };

  bool SubmitFromFlowIo(const Address& src,
                        const Address& dst,
                        uint16_t protocol,
                        Ptr<Packet> packet);
  bool SubmitFromBridgeEgress(const Address& src,
                              const Address& dst,
                              uint16_t protocol,
                              Ptr<Packet> packet);
  RxResult HandleIncomingPacket(const RoceSimHeader& roceHeader,
                                const ThreePHeader& tpHeader,
                                const Ptr<Packet>& payload,
                                const Address& from,
                                uint32_t formIndex);
  bool HandleAck(const RoceSimHeader& roceHeader,
                 const ThreePHeader& tpHeader);
  uint32_t GetCompletionCount(const RoceQpKey& qpKey,
                              RoceCompletionKind kind = RoceCompletionKind::Any) const;
  bool PopCompletion(const RoceQpKey& qpKey,
                     RoceCompletion* completion,
                     RoceCompletionKind kind = RoceCompletionKind::Any);

protected:
  void DoDispose() override;

private:
  struct PortTxItem {
    Ptr<Packet> packet;
    Address src;
    Address dst;
    uint16_t protocol = 0;
    uint32_t qid = 0;
    bool controlClass = false;
  };

  struct OutstandingPacket {
    Ptr<Packet> packet;
    Address src;
    Address dst;
    uint16_t protocol = 0;
    uint32_t qid = 0;
    bool controlClass = false;
    RoceFlowKey flowKey;
    RoceQpKey qpKey;
    uint32_t psn = 0;
    uint32_t retries = 0;
  };

  struct RetransTimerState {
    EventId event;
    uint32_t trackedPsn = 0;
  };

  struct SqEntry {
    uint64_t workId = 0;
    ThreePHeader::State state = ThreePHeader::DATA;
    RoceFlowKey flowKey;
    uint64_t totalBytes = 0;
    uint64_t totalPackets = 1;
    RoceTxClass txClass = RoceTxClass::Data;
  };

  struct RqEntry {
    uint32_t psn = 0;
    uint32_t wrId = 0;
  };

  struct QpRuntime {
    uint32_t sqDepth = 0;
    uint32_t rqDepth = 0;
    uint32_t cqDepth = 0;
    Address dest;
    bool hasDest = false;
    RoceTxClass txClass = RoceTxClass::Data;
    std::deque<SqEntry> sqEntries;
    std::map<uint32_t, RqEntry> rqInflight;
    std::deque<RoceCompletion> completions;
    uint32_t sendCompletionCount = 0;
    uint32_t recvCompletionCount = 0;
    uint32_t cqOverflowCount = 0;
  };

  void EnsureQueueState_();
  uint32_t GetNumQueues_() const;
  uint32_t ClassifyQueueId_(const Address& dst) const;
  bool IsQueuePaused_(uint32_t qid) const;
  uint32_t GetInflightLimit_() const;
  bool HasPendingWork_() const;
  bool CanAdmit_(uint32_t qid) const;
  void EnqueuePending_(PortTxItem&& item);
  bool TrySendNow_(PortTxItem* item);
  void DrainPending_();
  void ScheduleDrain_(bool delayed);
  void TrackSuccessfulSend_(const PortTxItem& item);
  void HandleRetransTimeout_(const RoceQpKey& qpKey);
  void ArmRetransTimer_(const RoceQpKey& qpKey);
  void CancelRetransTimer_(const RoceQpKey& qpKey);
  uint32_t GetOutstandingCount_(const RoceQpKey& qpKey) const;
  uint32_t GetOldestOutstandingPsn_(const RoceQpKey& qpKey) const;
  uint32_t GetRecoveryTargetPsn_(const RoceQpKey& qpKey) const;
  void OnQueuePauseState_(uint32_t qid, bool paused);
  void OnPhyTxEnd_(Ptr<const Packet> packet);
  QpRuntime& EnsureQpRuntime_(const RoceTxWorkItem& item);
  QpRuntime& EnsureQpRuntime_(const RoceQpKey& key);
  bool EnsureQpReadyToSend_(const RoceQpKey& key);
  std::set<RoceQpKey> CollectBlockedQps_() const;
  bool DispatchFromScheduler_();
  bool BuildPacketForSlice_(const RoceTxSlice& slice,
                            Ptr<Packet>* packetOut,
                            RoceSimHeader* headerOut = nullptr);
  bool BuildAckPacket_(uint32_t tag,
                       uint32_t src,
                       uint32_t dst,
                       uint32_t qpn,
                       Ptr<Packet>* packetOut,
                       RoceSimHeader* headerOut = nullptr);
  void MaybeSendAck_(const Address& dest,
                     uint32_t tag,
                     uint32_t src,
                     uint32_t dst,
                     uint32_t qpn,
                     RoceOpcode rxOpcode);
  void PushCompletion_(const RoceQpKey& qpKey,
                       uint32_t psn,
                       uint32_t wrId,
                       bool success,
                       RoceCompletionKind kind);
  void CompleteSqEntry_(const RoceQpKey& qpKey,
                        uint64_t workId,
                        uint32_t psn);
  bool AcquireRqEntry_(const RoceQpKey& qpKey,
                       uint32_t psn);
  void ReleaseRqEntry_(const RoceQpKey& qpKey,
                       uint32_t psn);
  void CollectReadyBufferedPackets_(const RoceQpKey& qpKey,
                                    std::vector<BufferedUpperPacket>* out);

private:
  Ptr<PointToPointNetDevice> m_device;
  RoceConfig m_cfg;
  TxNotifyCallback m_txNotify;
  RoceTxScheduler m_txScheduler;
  RoceQpEngine m_qpEngine;

  std::vector<std::deque<PortTxItem>> m_controlPending;
  std::vector<std::deque<PortTxItem>> m_dataPending;
  std::vector<uint32_t> m_inflightPackets;
  std::vector<bool> m_knownPaused;
  std::map<RoceQpKey, QpRuntime> m_qpRuntimes;
  std::map<RoceQpKey, std::map<uint32_t, BufferedUpperPacket>> m_bufferedUpperPackets;
  std::map<RoceQpKey, std::map<uint32_t, OutstandingPacket>> m_outstandingPackets;
  std::map<RoceQpKey, RetransTimerState> m_retransTimers;
  std::map<RoceQpKey, OutstandingPacket> m_pendingRetransmissions;
  std::map<RoceQpKey, uint32_t> m_recoveryTargetPsns;
  EventId m_drainEvent;
  bool m_tracesConnected = false;
  uint32_t m_rrIndex = 0;
  uint32_t m_nodeId = 0;
  bool m_hasNodeId = false;
};

} // namespace ns3

#endif // RDMA_PORT_ENGINE_H
