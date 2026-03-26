#include "roce-qp-engine.h"

namespace ns3 {

namespace {

bool
IsSupportedOpcode(RoceOpcode opcode) {
  // 当前仿真路径允许的 opcode 白名单。
  switch (opcode) {
    case RoceOpcode::Send:
    case RoceOpcode::SendWithImm:
    case RoceOpcode::RdmaWrite:
    case RoceOpcode::RdmaWriteWithImm:
    case RoceOpcode::RdmaReadReq:
    case RoceOpcode::RdmaReadResp:
    case RoceOpcode::Ack:
      return true;
    default:
      return false;
  }
}

} // namespace

void
RoceQpEngine::Configure(const RoceConfig& cfg) {
  m_cfg = cfg;
}

void
RoceQpEngine::Reset() {
  // 清理 QP 表和 completion 队列，保证新一轮仿真不受旧状态影响。
  m_qps.clear();
  while (!m_completions.empty()) {
    m_completions.pop();
  }
}

bool
RoceQpEngine::EnsureQp(const RoceQpKey& key) {
  auto it = m_qps.find(key);
  if (it != m_qps.end()) {
    return true;
  }

  RoceQpContext ctx;
  // 新建 QP 时从配置注入默认队列深度。
  ctx.state = RoceQpState::Reset;
  ctx.nextPsn = 0;
  ctx.expectedRxPsn = 0;
  ctx.sqDepth = m_cfg.defaultSqDepth;
  ctx.rqDepth = m_cfg.defaultRqDepth;
  m_qps.emplace(key, ctx);
  return true;
}

bool
RoceQpEngine::ModifyQpState(const RoceQpKey& key, RoceQpState target) {
  EnsureQp(key);
  auto it = m_qps.find(key);
  if (it == m_qps.end()) {
    return false;
  }
  if (!IsStateTransitionAllowed(it->second.state, target)) {
    return false;
  }
  it->second.state = target;
  if (target == RoceQpState::Reset) {
    it->second.nextPsn = 0;
    it->second.expectedRxPsn = 0;
    it->second.pendingRxPsns.clear();
    it->second.rxOutOfOrderPackets = 0;
    it->second.rxDuplicatePackets = 0;
    it->second.rxReorderOverflowPackets = 0;
  }
  return true;
}

RoceQpState
RoceQpEngine::GetQpState(const RoceQpKey& key) const {
  auto it = m_qps.find(key);
  if (it == m_qps.end()) {
    return RoceQpState::Reset;
  }
  return it->second.state;
}

bool
RoceQpEngine::CanPostSend(const RoceQpKey& key) const {
  return GetQpState(key) == RoceQpState::Rts;
}

bool
RoceQpEngine::CanPostRecv(const RoceQpKey& key) const {
  RoceQpState state = GetQpState(key);
  return state == RoceQpState::Init || state == RoceQpState::Rtr ||
         state == RoceQpState::Rts;
}

bool
RoceQpEngine::BuildSendHeader(const RoceQpKey& key,
                              RoceOpcode opcode,
                              RoceUserPhase phase,
                              uint32_t payloadBytes,
                              uint32_t tag,
                              RoceSimHeader* out) {
  if (out == nullptr) {
    return false;
  }

  EnsureQp(key);
  auto it = m_qps.find(key);
  if (it == m_qps.end()) {
    return false;
  }
  if (!CanPostSend(key)) {
    return false;
  }

  uint32_t psn = it->second.nextPsn;
  uint32_t ackPsn = 0;
  if (opcode == RoceOpcode::Ack || opcode == RoceOpcode::RdmaReadResp) {
    // ACK/READ_RESP 的序号锚定到当前累计确认点，而不是维护一条独立增长的
    // 反向 PSN 流。这样重复 ACK 不会把接收端 expectedRxPsn 冲到很高的位置，
    // ACT 等后续反向控制包也能继续沿同一累计序号闭环。
    ackPsn = it->second.expectedRxPsn == 0 ? 0 : (it->second.expectedRxPsn - 1);
    psn = ackPsn;
    if (it->second.nextPsn < ackPsn + 1) {
      it->second.nextPsn = ackPsn + 1;
    }
  } else {
    psn = it->second.nextPsn++;
  }
  it->second.txPackets++;

  // 发送头由 QP 上下文驱动，保证同一 QP 上 PSN 单调递增。
  out->SetVersion(1);
  out->SetOpcode(opcode);
  out->SetUserPhase(phase);
  out->SetQpn(key.qpn);
  out->SetPsn(psn);
  out->SetLength(payloadBytes);
  out->SetTag(tag);
  out->SetSrc(key.src);
  out->SetDst(key.dst);
  if (opcode == RoceOpcode::Ack || opcode == RoceOpcode::RdmaReadResp) {
    // ACK/READ_RESP 回显最近一个按序接收的 PSN。
    out->SetAckPsn(ackPsn);
  }
  return true;
}

bool
RoceQpEngine::OnRxHeader(const RoceQpKey& key,
                         const RoceSimHeader& header,
                         RoceCompletion* completion,
                         bool* shouldDeliverToUpper,
                         bool* bufferedForReorder,
                         bool* overflowedReorderWindow) {
  if (shouldDeliverToUpper != nullptr) {
    *shouldDeliverToUpper = false;
  }
  if (bufferedForReorder != nullptr) {
    *bufferedForReorder = false;
  }
  if (overflowedReorderWindow != nullptr) {
    *overflowedReorderWindow = false;
  }
  EnsureQp(key);
  auto it = m_qps.find(key);
  if (it == m_qps.end()) {
    return false;
  }
  if (!CanPostRecv(key)) {
    // 非法状态收包直接置 Err。
    it->second.state = RoceQpState::Err;
    return false;
  }
  if (!IsSupportedOpcode(header.GetOpcode())) {
    // 不支持的 opcode 视为协议错误。
    it->second.state = RoceQpState::Err;
    return false;
  }

  const uint32_t psn = header.GetPsn();
  if ((header.GetOpcode() == RoceOpcode::RdmaWrite ||
       header.GetOpcode() == RoceOpcode::RdmaReadResp) &&
      header.GetLength() == 0) {
    it->second.state = RoceQpState::Err;
    return false;
  }

  if (psn < it->second.expectedRxPsn) {
    // 旧包或重复包：说明该 PSN 已经在过去的有序推进里被覆盖，不再把 QP 置错。
    it->second.rxDuplicatePackets++;
    return true;
  }

  if (psn > it->second.expectedRxPsn) {
    const uint32_t reorderWindowPackets =
        std::max<uint32_t>(1, m_cfg.reorderWindowPackets);
    const uint64_t reorderWindowEndExclusive =
        static_cast<uint64_t>(it->second.expectedRxPsn) +
        static_cast<uint64_t>(reorderWindowPackets);
    if (static_cast<uint64_t>(psn) >= reorderWindowEndExclusive) {
      // 超出 bounded reorder window：不缓存 future packet，不上送业务层，
      // 继续依赖累计 ACK + timeout/recovery 把缺失洞补回来。
      it->second.rxReorderOverflowPackets++;
      if (overflowedReorderWindow != nullptr) {
        *overflowedReorderWindow = true;
      }
      return true;
    }

    const auto [_, inserted] = it->second.pendingRxPsns.insert(psn);
    if (!inserted) {
      // 同一个 future PSN 重复到达，只计重复，不影响 QP 状态。
      it->second.rxDuplicatePackets++;
      return true;
    }
    // 当前阶段先允许乱序存在于 QP 内部缓存中；后续由 expectedRxPsn 追平时自然消化。
    it->second.rxOutOfOrderPackets++;
    it->second.rxPackets++;
    if (bufferedForReorder != nullptr) {
      *bufferedForReorder = true;
    }
    return true;
  } else {
    it->second.expectedRxPsn = psn + 1;
    it->second.rxPackets++;
  }

  while (!it->second.pendingRxPsns.empty()) {
    auto next = it->second.pendingRxPsns.begin();
    if (*next != it->second.expectedRxPsn) {
      break;
    }
    it->second.pendingRxPsns.erase(next);
    it->second.expectedRxPsn++;
  }

  if (shouldDeliverToUpper != nullptr) {
    *shouldDeliverToUpper = true;
  }

  if (completion != nullptr) {
    // completion 采用 psn 作为 wrId，便于与上层 flow credit 对齐。
    completion->src = key.src;
    completion->dst = key.dst;
    completion->qpn = key.qpn;
    completion->psn = psn;
    completion->wrId = psn;
    completion->success = true;
  }
  return true;
}

uint32_t
RoceQpEngine::GetExpectedRxPsn(const RoceQpKey& key) const {
  auto it = m_qps.find(key);
  if (it == m_qps.end()) {
    return 0;
  }
  return it->second.expectedRxPsn;
}

uint32_t
RoceQpEngine::GetPendingRxCount(const RoceQpKey& key) const {
  auto it = m_qps.find(key);
  if (it == m_qps.end()) {
    return 0;
  }
  return static_cast<uint32_t>(it->second.pendingRxPsns.size());
}

void
RoceQpEngine::NotifyTxCompletion(const RoceQpKey& key,
                                 uint32_t wrId,
                                 bool success) {
  // 发送完成事件入队，等待上层消费。
  RoceCompletion c;
  c.src = key.src;
  c.dst = key.dst;
  c.qpn = key.qpn;
  c.psn = wrId;
  c.wrId = wrId;
  c.success = success;
  m_completions.push(c);
}

bool
RoceQpEngine::PopCompletion(RoceCompletion* completion) {
  if (completion == nullptr || m_completions.empty()) {
    return false;
  }
  *completion = m_completions.front();
  m_completions.pop();
  return true;
}

bool
RoceQpEngine::IsStateTransitionAllowed(RoceQpState from, RoceQpState to) const {
  if (from == to) {
    return true;
  }

  switch (from) {
    // 简化 RC 路径：Reset -> Init -> Rtr -> Rts，异常进入 Err，Err 可回 Reset。
    case RoceQpState::Reset:
      return to == RoceQpState::Init;
    case RoceQpState::Init:
      return to == RoceQpState::Rtr || to == RoceQpState::Err;
    case RoceQpState::Rtr:
      return to == RoceQpState::Rts || to == RoceQpState::Err;
    case RoceQpState::Rts:
      return to == RoceQpState::Err;
    case RoceQpState::Err:
      return to == RoceQpState::Reset;
    default:
      return false;
  }
}

} // namespace ns3
