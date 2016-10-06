////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2016 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_PREGEL_WORKER_H
#define ARANGODB_PREGEL_WORKER_H 1

#include <vector>
#include "Basics/Common.h"
#include "Cluster/ClusterInfo.h"
#include "VocBase/vocbase.h"

namespace arangodb {
namespace pregel {
  class Vertex;
  
  class Worker {
  public:
    Worker(int executionNumber, TRI_vocbase_t *vocbase, VPackSlice &s);
      
    void nextGlobalStep(VPackSlice &data);// called by coordinator
    
  private:
    std::string _coordinatorId;
    std::vector<Vertex> _vertices;
  };
}
}
#endif
