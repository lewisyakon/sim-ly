/*
 * OverlayCollectiveInjector.hh
 *
 * Minimal overlay collective injector for Astra-Sim.
 *
 * Usage:
 *   - Create one injector per Sys.
 *   - Call Arm() at the moment you consider "t=0" for the overlay.
 *   - The injector schedules a start event after start_delay_ticks.
 */

#ifndef __OVERLAY_COLLECTIVE_INJECTOR_HH__
#define __OVERLAY_COLLECTIVE_INJECTOR_HH__

#include <cstdint>
#include <string>
#include <vector>

#include "Callable.hh"
#include "Common.hh"
#include "MockNcclGroup.h"

namespace AstraSim {

class Sys;
class DataSet;
class LogicalTopology;

class OverlayCollectiveInjector : public Callable {
 public:
  struct Config {
    bool enable = false;

    // Delay from when Arm() is called, in Astra-Sim ticks.
    // In this project CLOCK_PERIOD=1, so 1 tick == 1 ns.
    Tick start_delay_ticks = 0;

    // Which collective to inject.
    ComType collective = ComType::All_to_All;

    // Payload size in bytes.
    uint64_t bytes = 0;

    // Dimensions involved for the collective.
    // If empty, defaults to "all dims involved".
    std::vector<bool> involved_dims;
    // std::vector<bool> involved_dims={1, 0, 0, 0, 0, 0, 0, 0, 0, 0};


    // Scheduling policy hint.
    SchedulingPolicy scheduling = SchedulingPolicy::None;

    // Optional MockNCCL communicator group hint.
    // Example:
    //   collective=All_to_All + mock_nccl_group=EP  ~= ALLTOALL_EP
    // If NONE, keep current workload-selected group behavior.
    MockNccl::GroupType mock_nccl_group = MockNccl::GroupType::NONE;

    // Layer id used only for logging / tracing.
    int layer = -777;

    std::string name = "overlay";
  };

  OverlayCollectiveInjector(Sys* sys, const Config& cfg);
  ~OverlayCollectiveInjector() override;

  // Register the "start overlay" event.
  // Call this at the moment you want overlay's relative time origin.
  void Arm();

  // Callable interface: invoked at start time (data == nullptr) and on
  // dataset completion (data != nullptr, expected IntData).
  void call(EventType event, CallData* data) override;

 private:
  Sys* m_sys = nullptr;
  Config m_cfg;

  DataSet* m_dataset = nullptr;
  bool m_started = false;
  Tick m_start_tick = 0;

  // Dedicated MockNCCL path for overlay collectives. This bypasses legacy
  // Sys collective configuration and always issues NcclFlowModel phases.
  CollectiveImplementation m_mock_nccl_impl{
      CollectiveImplementationType::NcclFlowModel};
  std::vector<CollectiveImplementation*> m_mock_nccl_impl_per_dim;
  LogicalTopology* m_mock_nccl_topology = nullptr;

  void StartNow_();
  void Finish_(int dataset_id);
};

} // namespace AstraSim

#endif // __OVERLAY_COLLECTIVE_INJECTOR_HH__
