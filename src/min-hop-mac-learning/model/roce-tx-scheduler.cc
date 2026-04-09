#include "roce-tx-scheduler.h"

#include <algorithm>
#include "roce-sim-header.h"

namespace ns3 {

namespace {

bool
DataUsesWrite(const RoceConfig& cfg) {
  return cfg.dataUseWrite || cfg.modeName == "write_only" ||
         cfg.modeName == "rc_3phase";
}

uint32_t
GetVisibleRoceWireBytesForState(ThreePHeader::State state, const RoceConfig& cfg) {
  switch (state) {
    case ThreePHeader::DATA:
      return RoceSimHeader::GetVisibleWireBytes(
          DataUsesWrite(cfg) ? RoceOpcode::RdmaWrite : RoceOpcode::Send);
    case ThreePHeader::REQUEST:
    case ThreePHeader::ACT:
    default:
      return RoceSimHeader::GetVisibleWireBytes(RoceOpcode::Send);
  }
}

} // namespace

void
RoceTxScheduler::Configure(const RoceConfig& cfg) {
  m_cfg = cfg;
  Reset();
}

void
RoceTxScheduler::Reset() {
  m_workQueues.clear();
  m_controlActiveQps.clear();
  m_dataActiveQps.clear();
  m_pendingWorkCount = 0;
}

bool
RoceTxScheduler::Enqueue(const RoceTxWorkItem& item) {
  if (item.remainingBytes == 0) {
    return false;
  }
  auto& queue = m_workQueues[item.qpKey];
  queue.push_back(item);
  MaybeActivateQp_(item.qpKey, item.txClass);
  ++m_pendingWorkCount;
  return true;
}

bool
RoceTxScheduler::HasPendingWork() const {
  return m_pendingWorkCount != 0;
}

uint64_t
RoceTxScheduler::GetPendingWorkCount() const {
  return m_pendingWorkCount;
}

uint64_t
RoceTxScheduler::GetPendingWorkCount(const RoceQpKey& qpKey) const {
  auto it = m_workQueues.find(qpKey);
  if (it == m_workQueues.end()) {
    return 0;
  }
  return static_cast<uint64_t>(it->second.size());
}

bool
RoceTxScheduler::PickNextSlice(uint32_t maxSliceBytes,
                               const std::set<RoceQpKey>* blockedQps,
                               RoceTxSlice* out) {
  if (out == nullptr || maxSliceBytes == 0) {
    return false;
  }
  if (PickFromClass_(RoceTxClass::Control, maxSliceBytes, blockedQps, out)) {
    return true;
  }
  return PickFromClass_(RoceTxClass::Data, maxSliceBytes, blockedQps, out);
}

bool
RoceTxScheduler::QueueHasSendableWork_(const WorkQueue& queue) const {
  return !queue.empty() && queue.front().remainingBytes != 0;
}

bool
RoceTxScheduler::PickFromClass_(RoceTxClass txClass,
                                uint32_t maxSliceBytes,
                                const std::set<RoceQpKey>* blockedQps,
                                RoceTxSlice* out) {
  auto& active = ActiveQps_(txClass);
  const size_t activeCount = active.size();
  for (size_t i = 0; i < activeCount; ++i) {
    const RoceQpKey qpKey = active.front();
    active.pop_front();
    if (blockedQps != nullptr && blockedQps->count(qpKey) != 0) {
      active.push_back(qpKey);
      continue;
    }
    auto it = m_workQueues.find(qpKey);
    if (it == m_workQueues.end()) {
      continue;
    }

    DropEmptyFrontWork_(qpKey);
    if (!QueueHasSendableWork_(it->second)) {
      continue;
    }

    RoceTxWorkItem& work = it->second.front();
    const uint32_t visibleRoceBytes =
        GetVisibleRoceWireBytesForState(work.state, m_cfg);
    const uint32_t effectiveBudget =
        maxSliceBytes > visibleRoceBytes ? (maxSliceBytes - visibleRoceBytes) : 1;
    const uint32_t sliceBytes = static_cast<uint32_t>(
        std::min<uint64_t>(work.remainingBytes, effectiveBudget));

    out->workId = work.workId;
    out->state = work.state;
    out->flowKey = work.flowKey;
    out->qpKey = work.qpKey;
    out->dest = work.dest;
    out->sendMode = work.sendMode;
    out->mindex = work.mindex;
    out->opcode = work.opcode;
    out->userPhase = work.userPhase;
    out->totalBytes = work.totalBytes;
    out->sliceBytes = sliceBytes;
    out->packetIndex = work.packetIndex;
    out->totalPackets = work.totalPackets;
    out->controlPacketBytes = work.controlPacketBytes;
    out->hasFlowTagHeader = work.hasFlowTagHeader;
    out->flowTagHeader = work.flowTagHeader;
    out->txClass = work.txClass;

    work.remainingBytes -= sliceBytes;
    work.packetIndex += 1;
    out->isLastSlice = (work.remainingBytes == 0);

    if (out->isLastSlice) {
      it->second.pop_front();
      if (m_pendingWorkCount != 0) {
        --m_pendingWorkCount;
      }
      if (QueueHasSendableWork_(it->second)) {
        active.push_back(qpKey);
      }
    } else {
      active.push_back(qpKey);
    }
    return true;
  }
  return false;
}

std::deque<RoceQpKey>&
RoceTxScheduler::ActiveQps_(RoceTxClass txClass) {
  return txClass == RoceTxClass::Control ? m_controlActiveQps : m_dataActiveQps;
}

const std::deque<RoceQpKey>&
RoceTxScheduler::ActiveQps_(RoceTxClass txClass) const {
  return txClass == RoceTxClass::Control ? m_controlActiveQps : m_dataActiveQps;
}

void
RoceTxScheduler::MaybeActivateQp_(const RoceQpKey& qpKey, RoceTxClass txClass) {
  auto& active = ActiveQps_(txClass);
  if (std::find(active.begin(), active.end(), qpKey) == active.end()) {
    active.push_back(qpKey);
  }
}

void
RoceTxScheduler::DropEmptyFrontWork_(const RoceQpKey& qpKey) {
  auto it = m_workQueues.find(qpKey);
  if (it == m_workQueues.end()) {
    return;
  }
  while (!it->second.empty() && it->second.front().remainingBytes == 0) {
    it->second.pop_front();
    if (m_pendingWorkCount != 0) {
      --m_pendingWorkCount;
    }
  }
}

} // namespace ns3
