#ifndef ROCE_TX_SCHEDULER_H
#define ROCE_TX_SCHEDULER_H

#include <cstdint>
#include <deque>
#include <map>
#include <set>

#include "3PHeader.h"
#include "flow-io.h"
#include "ns3/address.h"
#include "roce-sim-types.h"

namespace ns3 {

enum class RoceTxClass : uint8_t {
  Control = 0,
  Data = 1,
};

// 一个待发 work item 对应一次业务发送请求，不等于一个 packet。
struct RoceTxWorkItem {
  uint64_t workId = 0;
  ThreePHeader::State state = ThreePHeader::DATA;
  RoceFlowKey flowKey;
  RoceQpKey qpKey;
  Address dest;
  FlowSendMode sendMode = FlowSendMode::External;
  int mindex = 0;
  uint64_t totalBytes = 0;
  uint64_t remainingBytes = 0;
  uint64_t packetIndex = 0;
  uint64_t totalPackets = 1;
  RoceTxClass txClass = RoceTxClass::Data;
};

// 一个 slice 对应本轮真正要出网的一个 packet。
struct RoceTxSlice {
  uint64_t workId = 0;
  ThreePHeader::State state = ThreePHeader::DATA;
  RoceFlowKey flowKey;
  RoceQpKey qpKey;
  Address dest;
  FlowSendMode sendMode = FlowSendMode::External;
  int mindex = 0;
  uint64_t totalBytes = 0;
  uint32_t sliceBytes = 0;
  uint64_t packetIndex = 0;
  uint64_t totalPackets = 1;
  bool isLastSlice = false;
  RoceTxClass txClass = RoceTxClass::Data;
};

// 发送调度器第一阶段骨架：
// - 维护 per-QP work queue
// - 控制类优先
// - 数据类按 QP 轮询
class RoceTxScheduler {
public:
  void Configure(const RoceConfig& cfg);
  void Reset();

  bool Enqueue(const RoceTxWorkItem& item);
  bool HasPendingWork() const;
  bool HasPendingWorkForQp(const RoceQpKey& qpKey) const;
  uint64_t GetPendingWorkCount() const;
  void DropQp(const RoceQpKey& qpKey);

  bool PickNextSlice(uint32_t maxSliceBytes,
                     const std::set<RoceQpKey>* blockedQps,
                     RoceTxSlice* out);

private:
  using WorkQueue = std::deque<RoceTxWorkItem>;

  bool QueueHasSendableWork_(const WorkQueue& queue) const;
  bool PickFromClass_(RoceTxClass txClass,
                      uint32_t maxSliceBytes,
                      const std::set<RoceQpKey>* blockedQps,
                      RoceTxSlice* out);
  std::deque<RoceQpKey>& ActiveQps_(RoceTxClass txClass);
  const std::deque<RoceQpKey>& ActiveQps_(RoceTxClass txClass) const;
  void MaybeActivateQp_(const RoceQpKey& qpKey, RoceTxClass txClass);
  void DropEmptyFrontWork_(const RoceQpKey& qpKey);

private:
  RoceConfig m_cfg;
  std::map<RoceQpKey, WorkQueue> m_workQueues;
  std::deque<RoceQpKey> m_controlActiveQps;
  std::deque<RoceQpKey> m_dataActiveQps;
  uint64_t m_pendingWorkCount = 0;
};

} // namespace ns3

#endif // ROCE_TX_SCHEDULER_H
