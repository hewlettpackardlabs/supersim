/*
 * Copyright 2016 Hewlett Packard Enterprise Development LP
 *
 * Licensed under the Apache License, Version 2.0 (the 'License');
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "network/uno/RoutingFunctionFactory.h"

#include <cassert>

#include "network/uno/DirectRoutingFunction.h"
#include "network/RoutingFunction.h"

namespace Uno {

RoutingFunctionFactory::RoutingFunctionFactory(u32 _numVcs, u32 _concentration)
    : ::RoutingFunctionFactory(), numVcs_(_numVcs),
      concentration_(_concentration) {}

RoutingFunctionFactory::~RoutingFunctionFactory() {}

RoutingFunction* RoutingFunctionFactory::createRoutingFunction(
    const std::string& _name, const Component* _parent, Router* _router,
    u32 _inputPort, Json::Value _settings) {
  std::string algorithm = _settings["algorithm"].asString();
  u32 latency = _settings["latency"].asUInt();
  bool allVcs = _settings["all_vcs"].asBool();

  if (algorithm == "direct") {
    return new Uno::DirectRoutingFunction(
        _name, _parent, latency, _router, numVcs_, concentration_, allVcs);
  } else {
    fprintf(stderr, "Unknown routing algorithm: '%s'\n", algorithm.c_str());
    assert(false);
  }
}

}  // namespace Uno