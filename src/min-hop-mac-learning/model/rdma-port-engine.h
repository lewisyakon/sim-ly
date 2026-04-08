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
#include "roce-sim-header.h"
#include "roce-sim-types.h"

#include <cstdint>
#include <deque>
#include <map>
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

  bool SubmitFromFlowIo(const Address& src,
                        const Address& dst,
                        uint16_t protocol,
                        Ptr<Packet> packet);
  bool SubmitFromBridgeEgress(const Address& src,
                              const Address& dst,
                              uint16_t protocol,
                              Ptr<Packet> packet);
  bool HandleAck(const RoceSimHeader& roceHeader,
                 const ThreePHeader& tpHeader);

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

private:
  Ptr<PointToPointNetDevice> m_device;
  RoceConfig m_cfg;
  TxNotifyCallback m_txNotify;

  std::vector<std::deque<PortTxItem>> m_controlPending;
  std::vector<std::deque<PortTxItem>> m_dataPending;
  std::vector<uint32_t> m_inflightPackets;
  std::vector<bool> m_knownPaused;
  std::map<RoceQpKey, std::map<uint32_t, OutstandingPacket>> m_outstandingPackets;
  std::map<RoceQpKey, RetransTimerState> m_retransTimers;
  std::map<RoceQpKey, OutstandingPacket> m_pendingRetransmissions;
  std::map<RoceQpKey, uint32_t> m_recoveryTargetPsns;
  EventId m_drainEvent;
  bool m_tracesConnected = false;
  uint32_t m_rrIndex = 0;
};

} // namespace ns3

#endif // RDMA_PORT_ENGINE_H
