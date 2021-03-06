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
#include "architecture/util.h"

#include <cassert>
#include <cmath>

#include <algorithm>

u32 computeTailoredBufferLength(f64 _inputQueueMult, u32 _inputQueueMin,
                                u32 _inputQueueMax, u32 _channelLatency) {
  // compute tailored input queue depth based on input channel latency
  u32 depth = static_cast<u32>(std::ceil(_channelLatency * _inputQueueMult));
  depth = std::max(_inputQueueMin, std::min(depth, _inputQueueMax));
  return depth;
}
