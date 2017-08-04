/*
 * Licensed under the Apache License, Version 2.0 (the 'License');
 * you may not use this file except in compliance with the License.
 * See the NOTICE file distributed with this work for additional information
 * regarding copyright ownership. You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "router/outputqueued/Router.h"

#include <factory/Factory.h>

#include <cassert>
#include <cmath>

#include "congestion/CongestionStatus.h"
#include "network/Network.h"
#include "router/outputqueued/Ejector.h"
#include "router/outputqueued/InputQueue.h"
#include "router/outputqueued/OutputQueue.h"
#include "types/Packet.h"

namespace OutputQueued {

Router::Router(
    const std::string& _name, const Component* _parent, Network* _network,
    u32 _id, const std::vector<u32>& _address, u32 _numPorts, u32 _numVcs,
    MetadataHandler* _metadataHandler, Json::Value _settings)
    : ::Router(_name, _parent, _network, _id, _address, _numPorts, _numVcs,
               _metadataHandler, _settings),
      transferLatency_(_settings["transfer_latency"].asUInt()) {
  assert(!_settings["transfer_latency"].isNull());
  assert(transferLatency_ > 0);

  // determine the size of credits
  creditSize_ = numVcs_ * (u32)std::ceil(
      (f64)gSim->cycleTime(Simulator::Clock::CHANNEL) /
      (f64)gSim->cycleTime(Simulator::Clock::CORE));

  // initialize the port VCs trackers
  portVcs_.resize(numPorts_, U32_MAX);

  // initialize the expectation arrays
  expTimes_.resize(numPorts_, U64_MAX);
  expPackets_.resize(numPorts_, nullptr);

  // queue depths and pipeline control
  u32 inputQueueDepth = _settings["input_queue_depth"].asUInt();
  assert(inputQueueDepth > 0);

  // create a congestion status device
  congestionStatus_ = CongestionStatus::create(
      "CongestionStatus", this, this, _settings["congestion_status"]);

  // ensure the congestion status module is operating in absolute mode because
  //  the output queues are infinite and relative occupancy will always be zero
  assert(congestionStatus_->style() == CongestionStatus::Style::kAbsolute);

  // create routing algorithms, input queues, link to routing algorithm,
  //  crossbar, and schedulers
  routingAlgorithms_.resize(numPorts_ * numVcs_);
  inputQueues_.resize(numPorts_ * numVcs_, nullptr);
  for (u32 port = 0; port < numPorts_; port++) {
    for (u32 vc = 0; vc < numVcs_; vc++) {
      u32 vcIdx = vcIndex(port, vc);

      // create the name suffix
      std::string nameSuffix = "_" + std::to_string(port) + "_" +
          std::to_string(vc);

      // routing algorithm
      std::string rfname = "RoutingAlgorithm" + nameSuffix;
      RoutingAlgorithm* rf = network_->createRoutingAlgorithm(
          port, vc, rfname, this, this);
      routingAlgorithms_.at(vcIdx) = rf;

      // input queue
      std::string iqName = "InputQueue" + nameSuffix;
      InputQueue* iq = new InputQueue(
          iqName, this, this, inputQueueDepth, port, numVcs_, vc, rf);
      inputQueues_.at(vcIdx) = iq;
    }
  }

  // output queues, schedulers, and crossbar
  outputQueues_.resize(numPorts_ * numVcs_, nullptr);
  outputCrossbarSchedulers_.resize(numPorts_, nullptr);
  outputCrossbars_.resize(numPorts_, nullptr);
  ejectors_.resize(numPorts_, nullptr);
  for (u32 port = 0; port < numPorts_; port++) {
    // output port switch allocator
    std::string outputCrossbarSchedulerName =
        "OutputCrossbarScheduler_" + std::to_string(port);
    outputCrossbarSchedulers_.at(port) = new CrossbarScheduler(
        outputCrossbarSchedulerName, this, numVcs_, numVcs_, 1, port * numVcs_,
        Simulator::Clock::CHANNEL, _settings["output_crossbar_scheduler"]);

    // set the congestion status to watch the credits of the output scheduler
    // outputCrossbarSchedulers_.at(port)->addCreditWatcher(congestionStatus_);

    // output crossbar
    std::string outputCrossbarName = "OutputCrossbar_" + std::to_string(port);
    outputCrossbars_.at(port) = new Crossbar(
        outputCrossbarName, this, numVcs_, 1, Simulator::Clock::CHANNEL,
        _settings["output_crossbar"]);

    // ejector
    std::string ejName = "Ejector_" + std::to_string(port);
    ejectors_.at(port) = new Ejector(ejName, this, port);
    outputCrossbars_.at(port)->setReceiver(0, ejectors_.at(port), 0);

    // queues
    for (u32 vc = 0; vc < numVcs_; vc++) {
      // initialize the credit count in the OutputCrossbarScheduler
      outputCrossbarSchedulers_.at(port)->initCredits(vc, inputQueueDepth);

      // create the name suffix
      std::string nameSuffix = "_" + std::to_string(port) + "_" +
          std::to_string(vc);

      // compute the client indexes
      u32 clientIndexOut = vc;  // sw alloc and output crossbar
      u32 clientIndexMain = vcIndex(port, vc);  // main crossbar

      // output queue
      std::string oqName = "OutputQueue" + nameSuffix;
      OutputQueue* oq = new OutputQueue(
          oqName, this, port, vc,
          outputCrossbarSchedulers_.at(port), clientIndexOut,
          outputCrossbars_.at(port), clientIndexOut,
          congestionStatus_, clientIndexMain);
      outputQueues_.at(clientIndexMain) = oq;

      // register the output queue with switch allocator
      outputCrossbarSchedulers_.at(port)->setClient(clientIndexOut, oq);

      // tell the congestion status module of the number of credits (infinite)
      congestionStatus_->initCredits(clientIndexMain, U32_MAX);
    }
  }

  // allocate slots for I/O channels
  inputChannels_.resize(numPorts_, nullptr);
  outputChannels_.resize(numPorts_, nullptr);
}

Router::~Router() {
  delete congestionStatus_;
  for (u32 vc = 0; vc < (numPorts_ * numVcs_); vc++) {
    delete routingAlgorithms_.at(vc);
    delete inputQueues_.at(vc);
    delete outputQueues_.at(vc);
  }
  for (u32 port = 0; port < numPorts_; port++) {
    delete outputCrossbarSchedulers_.at(port);
    delete outputCrossbars_.at(port);
    delete ejectors_.at(port);
  }
}

void Router::setInputChannel(u32 _port, Channel* _channel) {
  assert(inputChannels_.at(_port) == nullptr);
  inputChannels_.at(_port) = _channel;
  _channel->setSink(this, _port);
}

Channel* Router::getInputChannel(u32 _port) const {
  return inputChannels_.at(_port);
}

void Router::setOutputChannel(u32 _port, Channel* _channel) {
  assert(outputChannels_.at(_port) == nullptr);
  outputChannels_.at(_port) = _channel;
  _channel->setSource(this, _port);
}

Channel* Router::getOutputChannel(u32 _port) const {
  return outputChannels_.at(_port);
}

void Router::receiveFlit(u32 _port, Flit* _flit) {
  u64& expTime = expTimes_.at(_port);
  Packet*& expPacket = expPackets_.at(_port);

  // ensure back-to-back flit transmission
  u64 now = gSim->time();
  assert((expTime == U64_MAX) || (now == expTime));
  if (_flit->isTail()) {
    expTime = U64_MAX;
  } else {
    expTime = gSim->futureCycle(Simulator::Clock::CHANNEL, 1);
  }

  // ensure packet buffer flow control
  if (_flit->isHead()) {
    assert(expPacket == nullptr);
    expPacket = _flit->packet();
  } else {
    assert(expPacket == _flit->packet());
  }
  if (_flit->isTail()) {
    expPacket = nullptr;
  }

  // figure out the proper VC to use
  //  this fixes the symptoms of hyperwarping packets
  u32 vc = _flit->getVc();
  if (_flit->isHead()) {
    portVcs_.at(_port) = vc;
  } else {
    vc = portVcs_.at(_port);
  }

  // give the flit to the input queue
  InputQueue* iq = inputQueues_.at(vcIndex(_port, vc));
  iq->receiveFlit(0, _flit);

  // inform base class of arrival
  if (_flit->isHead()) {
    packetArrival(_port, _flit->packet());
  }
}

void Router::receiveCredit(u32 _port, Credit* _credit) {
  while (_credit->more()) {
    u32 vc = _credit->getNum();
    outputCrossbarSchedulers_.at(_port)->incrementCredit(vc);
  }
  delete _credit;
}

void Router::sendCredit(u32 _port, u32 _vc) {
  // ensure there is an outgoing credit for the next time slot
  assert(_vc < numVcs_);
  Credit* credit = inputChannels_.at(_port)->getNextCredit();
  if (credit == nullptr) {
    credit = new Credit(creditSize_);
    inputChannels_.at(_port)->setNextCredit(credit);
  }

  // mark the credit with the specified VC
  credit->putNum(_vc);
}

void Router::sendFlit(u32 _port, Flit* _flit) {
  assert(outputChannels_.at(_port)->getNextFlit() == nullptr);
  outputChannels_.at(_port)->setNextFlit(_flit);

  // inform base class of departure
  if (_flit->isHead()) {
    packetDeparture(_port, _flit->packet());
  }
}

void Router::transferPacket(Flit* _headFlit, u32 _outputPort, u32 _outputVc) {
  assert(gSim->epsilon() == 2);

  // get the output queue
  u32 vcIdx = vcIndex(_outputPort, _outputVc);

  Packet* packet = _headFlit->packet();
  for (u32 f = 0; f < packet->numFlits(); f++) {
    // change VCs
    Flit* flit = packet->getFlit(f);
    flit->setVc(_outputVc);

    // decrement credit in the congestion status module
    congestionStatus_->decrementCredit(vcIdx);
  }

  // determine the time of arrival at the output queue
  u32 time = gSim->futureCycle(Simulator::Clock::CORE, transferLatency_);
  addEvent(time, 1, packet, static_cast<s32>(vcIdx));
}

f64 Router::congestionStatus(u32 _inputPort, u32 _inputVc,
                             u32 _outputPort, u32 _outputVc) const {
  return congestionStatus_->status(_inputPort, _inputVc, _outputPort,
                                   _outputVc);
}

void Router::processEvent(void* _event, s32 _type) {
  u32 vcIdx = static_cast<u32>(_type);
  OutputQueue* outputQueue = outputQueues_.at(vcIdx);
  Packet* packet = reinterpret_cast<Packet*>(_event);
  outputQueue->receivePacket(packet);
}

}  // namespace OutputQueued

registerWithFactory("output_queued", ::Router,
                    OutputQueued::Router, ROUTER_ARGS);
