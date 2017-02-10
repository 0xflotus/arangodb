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

#include "PregelFeature.h"
#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/MutexLocker.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#include "Pregel/Conductor.h"
#include "Pregel/Recovery.h"
#include "Pregel/ThreadPool.h"
#include "Pregel/Worker.h"

using namespace arangodb::pregel;

static PregelFeature* Instance = nullptr;

uint64_t PregelFeature::createExecutionNumber() {
  return ClusterInfo::instance()->uniqid();
}

PregelFeature::PregelFeature(application_features::ApplicationServer* server)
    : ApplicationFeature(server, "Pregel") {
  setOptional(true);
  requiresElevatedPrivileges(false);
  startsAfter("Logger");
  startsAfter("Database");
  startsAfter("Endpoint");
  startsAfter("Cluster");
  startsAfter("Server");
  startsAfter("V8Dealer");
}

PregelFeature::~PregelFeature() {
  if (_recoveryManager) {
    _recoveryManager.reset();
  }
  cleanupAll();
}

PregelFeature* PregelFeature::instance() { return Instance; }

static size_t _approxThreadNumber() {
  const size_t procNum = TRI_numberProcessors();
  if (procNum <= 1)
    return 1;
  else return procNum - 1;// use full performance on cluster
}

void PregelFeature::start() {
  Instance = this;
  if (ServerState::instance()->isAgent()) {
    return;
  }

  const size_t threadNum = _approxThreadNumber();
  LOG_TOPIC(INFO, Logger::PREGEL) << "Pregel uses " << threadNum << " threads";
  _threadPool.reset(new ThreadPool(threadNum, "Pregel"));

  if (ServerState::instance()->isCoordinator()) {
    _recoveryManager.reset(new RecoveryManager());
    //    ClusterFeature* cluster =
    //    application_features::ApplicationServer::getFeature<ClusterFeature>(
    //                                                                        "Cluster");
    //    if (cluster != nullptr) {
    //      AgencyCallbackRegistry* registry =
    //      cluster->agencyCallbackRegistry();
    //      if (registry != nullptr) {
    //        _recoveryManager.reset(new RecoveryManager(registry));
    //      }
    //    }
  }
}

void PregelFeature::beginShutdown() { cleanupAll(); }

void PregelFeature::addExecution(Conductor* const exec,
                                 uint64_t executionNumber) {
  MUTEX_LOCKER(guard, _mutex);
  //_executions.
  _conductors[executionNumber] = exec;
}

Conductor* PregelFeature::conductor(uint64_t executionNumber) {
  MUTEX_LOCKER(guard, _mutex);
  auto it = _conductors.find(executionNumber);
  return it != _conductors.end() ? it->second : nullptr;
}

void PregelFeature::addWorker(IWorker* const worker, uint64_t executionNumber) {
  MUTEX_LOCKER(guard, _mutex);
  _workers[executionNumber] = worker;
}

IWorker* PregelFeature::worker(uint64_t executionNumber) {
  MUTEX_LOCKER(guard, _mutex);
  auto it = _workers.find(executionNumber);
  return it != _workers.end() ? it->second : nullptr;
}

void PregelFeature::cleanup(uint64_t executionNumber) {
  MUTEX_LOCKER(guard, _mutex);
  auto cit = _conductors.find(executionNumber);
  if (cit != _conductors.end()) {
    delete (cit->second);
    _conductors.erase(executionNumber);
  }
  auto wit = _workers.find(executionNumber);
  if (wit != _workers.end()) {
    delete (wit->second);
    _workers.erase(executionNumber);
  }
}

void PregelFeature::cleanupAll() {
  MUTEX_LOCKER(guard, _mutex);
  for (auto it : _conductors) {
    delete (it.second);
  }
  _conductors.clear();
  for (auto it : _workers) {
    it.second->cancelGlobalStep(VPackSlice());
    usleep(1000 * 25);
    delete (it.second);
  }
  _workers.clear();
}
