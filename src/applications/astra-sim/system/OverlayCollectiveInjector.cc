/*
 * OverlayCollectiveInjector.cc
 */

#include "OverlayCollectiveInjector.hh"

#include "DataSet.hh"
#include "IntData.hh"
#include "MockNcclLog.h"
#include "Sys.hh"
#include "astra-sim/workload/Layer.hh"
#include "astra-sim/system/topology/GeneralComplexTopology.hh"

namespace AstraSim {

OverlayCollectiveInjector::OverlayCollectiveInjector(Sys* sys, const Config& cfg)
    : m_sys(sys), m_cfg(cfg) {
  // Default: all physical dimensions involved.
  if (m_sys != nullptr && m_cfg.involved_dims.empty()) {
    m_cfg.involved_dims = std::vector<bool>(m_sys->physical_dims.size(), true);
  }

  // Build a dedicated topology + implementation vector that always maps to
  // MockNCCL flow-model collectives, regardless of Sys default settings.
  if (m_sys != nullptr && !m_sys->physical_dims.empty()) {
    m_mock_nccl_impl_per_dim.assign(m_sys->physical_dims.size(), &m_mock_nccl_impl);
    m_mock_nccl_topology = new GeneralComplexTopology(
        m_sys->id, m_sys->physical_dims, m_mock_nccl_impl_per_dim);
  }
}

OverlayCollectiveInjector::~OverlayCollectiveInjector() {
  if (m_mock_nccl_topology != nullptr) {
    delete m_mock_nccl_topology;
    m_mock_nccl_topology = nullptr;
  }
}

void OverlayCollectiveInjector::Arm() {
  if (!m_cfg.enable || m_sys == nullptr) {
    return;
  }
  Tick delay = m_cfg.start_delay_ticks;
  // Use try_register_event(Tick&) to avoid int overflow.
  m_sys->try_register_event(this, EventType::General, nullptr, delay);
}

void OverlayCollectiveInjector::call(EventType /*event*/, CallData* data) {
  if (!m_cfg.enable) {
    return;
  }

  // Convention:
  //   - data == nullptr  => start overlay collective now
  //   - data != nullptr  => dataset completion (IntData)
  if (data == nullptr) {
    StartNow_();
    return;
  }

  IntData* id = static_cast<IntData*>(data);
  Finish_(id->data);
  delete id;
}

void OverlayCollectiveInjector::StartNow_() {
  if (m_started || m_sys == nullptr) {
    return;
  }
  if (m_cfg.bytes == 0) {
    return;
  }

  m_started = true;
  m_start_tick = Sys::boostedTick();

  MockNcclLog* log = MockNcclLog::getInstance();
  log->writeLog(
      NcclLogLevel::INFO,
      "[Overlay] start name=%s node=%d bytes=%llu collective=%d group=%d start_tick=%llu delay_ticks=%llu",
      m_cfg.name.c_str(),
      m_sys->id,
      (unsigned long long)m_cfg.bytes,
      (int)m_cfg.collective,
      (int)m_cfg.mock_nccl_group,
      (unsigned long long)m_start_tick,
      (unsigned long long)m_cfg.start_delay_ticks);

  // Optional per-overlay group override so MockNCCL can target refined
  // collectives (e.g., All_to_All + EP == ALLTOALL_EP).
  auto has_mock_comm = [&](MockNccl::GroupType group) {
    auto it = m_sys->mock_nccl_comms.find(static_cast<ParallelStrategy>(group));
    return it != m_sys->mock_nccl_comms.end() && it->second != nullptr;
  };
  auto pick_first_available_group = [&]() {
    const MockNccl::GroupType order[] = {
        MockNccl::GroupType::TP,
        MockNccl::GroupType::DP,
        MockNccl::GroupType::EP,
        MockNccl::GroupType::DP_EP};
    for (auto group : order) {
      if (has_mock_comm(group)) {
        return group;
      }
    }
    return MockNccl::GroupType::NONE;
  };

  bool group_overridden = false;
  Workload::LoopState overridden_state = Workload::LoopState::Forward_Pass;
  MockNccl::GroupType saved_group = MockNccl::GroupType::NONE;
  Layer* active_layer = nullptr;
  if (m_sys->workload != nullptr &&
      m_sys->workload->layers != nullptr &&
      m_sys->workload->index >= 0 &&
      m_sys->workload->index < m_sys->workload->SIZE &&
      m_sys->workload->layers[m_sys->workload->index] != nullptr) {
    active_layer = m_sys->workload->layers[m_sys->workload->index];
    overridden_state = m_sys->workload->current_state;

    auto read_state_group = [&]() {
      switch (overridden_state) {
        case Workload::LoopState::Forward_Pass:
          return active_layer->fwd_pass_group_type;
        case Workload::LoopState::Input_Gradient:
          return active_layer->input_grad_group_type;
        case Workload::LoopState::Weight_Gradient:
          return active_layer->weight_grad_group_type;
        default:
          return MockNccl::GroupType::NONE;
      }
    };
    auto write_state_group = [&](MockNccl::GroupType group) {
      switch (overridden_state) {
        case Workload::LoopState::Forward_Pass:
          active_layer->fwd_pass_group_type = group;
          break;
        case Workload::LoopState::Input_Gradient:
          active_layer->input_grad_group_type = group;
          break;
        case Workload::LoopState::Weight_Gradient:
          active_layer->weight_grad_group_type = group;
          break;
        default:
          break;
      }
    };

    MockNccl::GroupType current_group = read_state_group();
    MockNccl::GroupType selected_group = current_group;

    if (m_cfg.mock_nccl_group != MockNccl::GroupType::NONE) {
      if (has_mock_comm(m_cfg.mock_nccl_group)) {
        selected_group = m_cfg.mock_nccl_group;
      } else {
        log->writeLog(
            NcclLogLevel::WARNING,
            "[Overlay] requested group=%d is unavailable on node=%d, fallback "
            "to current group=%d",
            (int)m_cfg.mock_nccl_group,
            m_sys->id,
            (int)current_group);
      }
    }

    if (!has_mock_comm(selected_group)) {
      MockNccl::GroupType fallback_group = pick_first_available_group();
      if (fallback_group == MockNccl::GroupType::NONE) {
        log->writeLog(
            NcclLogLevel::ERROR,
            "[Overlay] no available MockNCCL communicator on node=%d, skip overlay",
            m_sys->id);
        m_started = false;
        return;
      }
      log->writeLog(
          NcclLogLevel::WARNING,
          "[Overlay] fallback group from %d to %d on node=%d",
          (int)selected_group,
          (int)fallback_group,
          m_sys->id);
      selected_group = fallback_group;
    }

    if (selected_group != current_group) {
      saved_group = current_group;
      write_state_group(selected_group);
      group_overridden = true;
    }
  } else if (m_cfg.mock_nccl_group != MockNccl::GroupType::NONE) {
    log->writeLog(
        NcclLogLevel::WARNING,
        "[Overlay] requested group override=%d but active workload layer is unavailable on node=%d",
        (int)m_cfg.mock_nccl_group,
        m_sys->id);
  }

  DataSet* ds = nullptr;
  if (m_mock_nccl_topology != nullptr && !m_mock_nccl_impl_per_dim.empty()) {
    ComType overlay_type = m_cfg.collective;
    switch (overlay_type) {
      case ComType::All_Reduce:
      case ComType::Reduce_Scatter:
      case ComType::All_Gather:
      case ComType::All_to_All:
      case ComType::All_Reduce_All_to_All:
      case ComType::All_Reduce_NVLS:
        break;
      default:
        overlay_type = ComType::All_to_All;
        break;
    }
    ds = m_sys->generate_collective(
        m_cfg.bytes,
        m_cfg.layer,
        m_mock_nccl_topology,
        m_mock_nccl_impl_per_dim,
        m_cfg.involved_dims,
        overlay_type,
        m_cfg.scheduling,
        EventType::NONE,
        nullptr);
  } else {
    // Fallback: keep legacy behavior if dedicated MockNCCL setup is missing.
    switch (m_cfg.collective) {
      case ComType::All_Reduce:
          ds = m_sys->generate_all_reduce(
              m_cfg.bytes,
              m_cfg.involved_dims,
              m_cfg.scheduling,
              m_cfg.layer,
              EventType::NONE,
              nullptr);
          break;

      case ComType::Reduce_Scatter:
          ds = m_sys->generate_reduce_scatter(
              m_cfg.bytes,
              m_cfg.involved_dims,
              m_cfg.scheduling,
              m_cfg.layer,
              EventType::NONE,
              nullptr);
          break;

      case ComType::All_Gather:
          ds = m_sys->generate_all_gather(
              m_cfg.bytes,
              m_cfg.involved_dims,
              m_cfg.scheduling,
              m_cfg.layer,
              EventType::NONE,
              nullptr);
          break;

      case ComType::All_to_All:
      default:
          ds = m_sys->generate_all_to_all(
              m_cfg.bytes,
              m_cfg.involved_dims,
              m_cfg.scheduling,
              m_cfg.layer,
              EventType::NONE,
              nullptr);
          break;
    }
  }

  if (group_overridden && active_layer != nullptr) {
    switch (overridden_state) {
      case Workload::LoopState::Forward_Pass:
        active_layer->fwd_pass_group_type = saved_group;
        break;
      case Workload::LoopState::Input_Gradient:
        active_layer->input_grad_group_type = saved_group;
        break;
      case Workload::LoopState::Weight_Gradient:
        active_layer->weight_grad_group_type = saved_group;
        break;
      default:
        break;
    }
  }


  if (ds == nullptr) {
    log->writeLog(NcclLogLevel::ERROR,
                  "[Overlay] failed to create dataset name=%s node=%d",
                  m_cfg.name.c_str(),
                  m_sys->id);
    return;
  }

  // Ensure we get a completion callback even in builds where Sys does not set
  // dataset notifier (some paths are guarded by PHY_MTP).
  ds->set_notifier(this, EventType::General);
  m_dataset = ds;
}

void OverlayCollectiveInjector::Finish_(int dataset_id) {
  Tick end_tick = Sys::boostedTick();
  Tick dur = (end_tick >= m_start_tick) ? (end_tick - m_start_tick) : 0;

  MockNcclLog* log = MockNcclLog::getInstance();
  log->writeLog(
      NcclLogLevel::INFO,
      "[Overlay] finish name=%s node=%d dataset=%d end_tick=%llu dur_ns=%llu",
      m_cfg.name.c_str(),
      m_sys ? m_sys->id : -1,
      dataset_id,
      (unsigned long long)end_tick,
      (unsigned long long)dur);

  // This dataset is owned by the injector (only used for overlay experiments).
  if (m_dataset != nullptr) {
    delete m_dataset;
    m_dataset = nullptr;
  }
}

} // namespace AstraSim
