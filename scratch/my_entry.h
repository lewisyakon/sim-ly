/* 
*Copyright (c) 2024, Alibaba Group;
*Licensed under the Apache License, Version 2.0 (the "License");
*you may not use this file except in compliance with the License.
*You may obtain a copy of the License at

*   http://www.apache.org/licenses/LICENSE-2.0

*Unless required by applicable law or agreed to in writing, software
*distributed under the License is distributed on an "AS IS" BASIS,
*WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*See the License for the specific language governing permissions and
*limitations under the License.
*/

#ifndef __ENTRY_H__
#define __ENTRY_H__

#undef PGO_TRAINING
#define PATH_TO_PGO_CONFIG "path_to_pgo_config"
#define _QPS_PER_CONNECTION_  1
#include "my_common.h"
#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/error-model.h"
#include "ns3/global-route-manager.h"
#include "ns3/internet-module.h"
#include "ns3/ipv4-static-routing-helper.h"
#include "ns3/packet.h"
#include "ns3/point-to-point-helper.h"

#include <fstream>
#include <iostream>
#include <time.h>
#include <unordered_map>
#include <mutex>
#include <vector>
#ifdef NS3_MTP
#include "ns3/mtp-interface.h"
#endif
#include <map>
#include"astra-sim/system/MockNcclQps.h"
#include "ns3/flow-io.h"

using namespace ns3;
using namespace std;

std::mutex mtx;
struct task1 {
  int src;
  int dest;
  int type;
  uint64_t count;
  void *fun_arg;
  void (*msg_handler)(void *fun_arg);
  double schTime; 
};
map<std::pair<int, int>, int64_t> nodeHash;
extern uint32_t numHosts,gpus_per_server,Topology_model;


ns3::FlowSendMode check_out_or_iner(int src,int dst){//0表示在外部，1表示内部,3表示显示制定nvswitch,4表示无条件送原地址,5表示送本地交换机
  if (Topology_model==2 or Topology_model==3) return FlowSendMode::Internal;
  if (Topology_model==6) {
    return IsExternalTopologyInternalPair(src, dst) ? FlowSendMode::Internal
                                                   : FlowSendMode::External;
  }
  if (src < 0 || dst < 0) return FlowSendMode::External;//疑似错误源目，无条件外送
  const uint32_t srcRank = static_cast<uint32_t>(src);
  const uint32_t dstRank = static_cast<uint32_t>(dst);
  if (srcRank>numHosts or dstRank>numHosts) return FlowSendMode::External;//疑似错误源目，无条件外送
  else if (srcRank/gpus_per_server == dstRank/gpus_per_server) return FlowSendMode::Internal;
       else return FlowSendMode::External;
}

void SendFlow(int src, int dst, uint64_t maxPacketCount,
void (*msg_handler)(void *fun_arg), void *fun_arg, int tag, AstraSim::sim_request *request) {
  // NS_LOG_INFO("[SendFlow]: now_rank="<<MpiInterface::GetSystemId()<<" src_rank="<<(nodetable[src]->GetSystemId()>>16)<<" tag="<<tag<<" src="<<src<<" dst="<<dst<<" size="<<maxPacketCount);

  uint64_t real_PacketCount = maxPacketCount;
  // bool nvls_on = request->flowTag.nvls_on;

  if(real_PacketCount == 0) real_PacketCount = 1;
  if(real_PacketCount == 1023)
  {
    Ptr<sendflowclient> clientHelper=nodeapptable[src];
    clientHelper->set_sendfunc(tag,src,dst,{msg_handler,fun_arg});

    clientHelper->TriggerSendTo_now(nodeaddresstable[dst], real_PacketCount,
      tag, src, dst, check_out_or_iner(src, dst));
  }
  else
  {

    Ptr<sendflowclient> clientHelper=nodeapptable[src];
    AstraSim::SendPacketEventHandlerData* ehd = (AstraSim::SendPacketEventHandlerData*) fun_arg;
    // AstraSim::RecvPacketEventHadndlerData* ehd = (AstraSim::RecvPacketEventHadndlerData*) fun_arg;
    ehd->flowTag=request->flowTag;
    const int flowTagId = request->flowTag.tag_id;
    clientHelper->set_sendfunc(flowTagId,src,dst,{msg_handler,fun_arg});
    clientHelper->write_flowtag(flowTagId,src,dst,*request); 

    
    
    clientHelper = nodeapptable[src];
    clientHelper->TriggerSendTo_now(nodeaddresstable[dst], real_PacketCount,
      flowTagId, src, dst, check_out_or_iner(src,dst));

  }
}


int main1() {
  SetConfig();
  SetupNetwork();

  std::cout << "Running Simulation.\n";
  fflush(stdout);
  NS_LOG_INFO("Run Simulation.");

  return 0;
}
int main1(string network_conf) {
  if (!ReadConf(network_conf)){
    SetConfig();
  } 

  SetupNetwork();

  std::cout << "Running Simulation.\n";
  fflush(stdout);
  NS_LOG_INFO("Run Simulation.");

  return 0;
}
#endif
