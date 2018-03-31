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

#ifndef ARANGODB_HYDRA_FEATURE_H
#define ARANGODB_HYDRA_FEATURE_H 1

#include <cstdint>
#include "ApplicationFeatures/ApplicationFeature.h"
#include "Basics/Common.h"
#include "Basics/Mutex.h"
#include "Hydra/JobContext.h"

namespace arangodb {
namespace hydra {


class HydraFeature final : public application_features::ApplicationFeature {
 public:
  explicit HydraFeature(application_features::ApplicationServer* server);
  HydraFeature();

  static HydraFeature* instance();
  static size_t availableParallelism();

  void start() override final;
  void beginShutdown() override final;

  void addJob(std::unique_ptr<JobContext>&&);
  JobBase* job(uint64_t);

  void cleanupJob(uint64_t executionNumber);
  void cleanupAll();

 private:
  Mutex _mutex;
  std::unordered_map<uint64_t, std::unique_ptr<JobContext>> _jobs;
};
}
}

#endif
