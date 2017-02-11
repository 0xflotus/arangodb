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

#include "Pregel/Worker.h"
#include "Pregel/Aggregator.h"
#include "Pregel/CommonFormats.h"
#include "Pregel/GraphStore.h"
#include "Pregel/IncomingCache.h"
#include "Pregel/OutgoingCache.h"
#include "Pregel/PregelFeature.h"
#include "Pregel/ThreadPool.h"
#include "Pregel/Utils.h"
#include "Pregel/VertexComputation.h"
#include "Pregel/WorkerConfig.h"

#include "Basics/MutexLocker.h"
#include "Basics/ReadLocker.h"
#include "Basics/WriteLocker.h"
#include "Cluster/ClusterComm.h"
#include "Cluster/ClusterInfo.h"
#include "VocBase/ticks.h"
#include "VocBase/vocbase.h"

#include <velocypack/Iterator.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::pregel;

template <typename V, typename E, typename M>
Worker<V, E, M>::Worker(TRI_vocbase_t* vocbase, Algorithm<V, E, M>* algo,
                        VPackSlice initConfig)
    : _config(vocbase->name(), initConfig), _algorithm(algo) {
  MUTEX_LOCKER(guard, _commandMutex);

  VPackSlice userParams = initConfig.get(Utils::userParametersKey);
  _state = WorkerState::IDLE;
  _workerContext.reset(algo->workerContext(userParams));
  _messageFormat.reset(algo->messageFormat());
  _messageCombiner.reset(algo->messageCombiner());
  _conductorAggregators.reset(new AggregatorHandler(algo));
  _workerAggregators.reset(new AggregatorHandler(algo));
  _graphStore.reset(new GraphStore<V, E>(vocbase, _algorithm->inputFormat()));
  _nextGSSSendMessageCount = 0;
  if (_config.asynchronousMode()) {
    _messageBatchSize = _algorithm->messageBatchSize(_config, _messageStats, 0);
  } else {
    _messageBatchSize = 5000;
  }

  if (_messageCombiner) {
    _readCache = new CombiningInCache<M>(&_config, _messageFormat.get(),
                                         _messageCombiner.get());
    _writeCache = new CombiningInCache<M>(&_config, _messageFormat.get(),
                                          _messageCombiner.get());
    if (_config.asynchronousMode()) {
      _writeCacheNextGSS = new CombiningInCache<M>(
          &_config, _messageFormat.get(), _messageCombiner.get());
    }
  } else {
    _readCache = new ArrayInCache<M>(&_config, _messageFormat.get());
    _writeCache = new ArrayInCache<M>(&_config, _messageFormat.get());
    if (_config.asynchronousMode()) {
      _writeCacheNextGSS = new ArrayInCache<M>(&_config, _messageFormat.get());
    }
  }

  std::function<void()> callback = [this] {
    VPackBuilder package;
    package.openObject();
    package.add(Utils::senderKey, VPackValue(ServerState::instance()->getId()));
    package.add(Utils::executionNumberKey,
                VPackValue(_config.executionNumber()));
    package.add(Utils::vertexCountKey,
                VPackValue(_graphStore->localVertexCount()));
    package.add(Utils::edgeCountKey, VPackValue(_graphStore->localEdgeCount()));
    package.close();
    _callConductor(Utils::finishedStartupPath, package.slice());
  };

  if (_config.lazyLoading()) {
    // TODO maybe lazy loading needs to be performed on another thread too
    std::set<std::string> activeSet = _algorithm->initialActiveSet();
    if (activeSet.size() == 0) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                     "There needs to be one active vertice");
    }
    for (std::string const& documentID : activeSet) {
      _graphStore->loadDocument(&_config, documentID);
    }
    callback();
  } else {
    // initialization of the graphstore might take an undefined amount
    // of time. Therefore this is performed asynchronous
    ThreadPool* pool = PregelFeature::instance()->threadPool();
    pool->enqueue(
        [this, callback] { _graphStore->loadShards(&_config, callback); });
  }
}

/*template <typename M>
GSSContext::~GSSContext() {}*/

template <typename V, typename E, typename M>
Worker<V, E, M>::~Worker() {
  LOG_TOPIC(INFO, Logger::PREGEL) << "Called ~Worker()";
  _state = WorkerState::DONE;
  usleep(50000);  // 50ms wait for threads to die
  delete _readCache;
  delete _writeCache;
  delete _writeCacheNextGSS;
  _writeCache = nullptr;
}

template <typename V, typename E, typename M>
VPackBuilder Worker<V, E, M>::prepareGlobalStep(VPackSlice const& data) {
  // Only expect serial calls from the conductor.
  // Lock to prevent malicous activity
  MUTEX_LOCKER(guard, _commandMutex);
  if (_state != WorkerState::IDLE) {
    LOG_TOPIC(ERR, Logger::PREGEL) << "Expected GSS " << _expectedGSS;
    LOG_TOPIC(ERR, Logger::PREGEL) << "Cannot prepare a gss when the worker is not idle";
    THROW_ARANGO_EXCEPTION(TRI_ERROR_INTERNAL);
  }
  _state = WorkerState::PREPARING;  // stop any running step
  LOG_TOPIC(INFO, Logger::PREGEL) << "Received prepare GSS: " << data.toJson();
  VPackSlice gssSlice = data.get(Utils::globalSuperstepKey);
  if (!gssSlice.isInteger()) {
    THROW_ARANGO_EXCEPTION_FORMAT(TRI_ERROR_BAD_PARAMETER,
                                  "Invalid gss in %s:%d", __FILE__, __LINE__);
  }
  const uint64_t gss = (uint64_t)gssSlice.getUInt();
  if (_expectedGSS != gss) {
    THROW_ARANGO_EXCEPTION_FORMAT(
        TRI_ERROR_BAD_PARAMETER,
        "Seems like this worker missed a gss, expected %u. Data = %s ",
        _expectedGSS, data.toJson().c_str());
  }

  // initialize worker context
  if (_workerContext && gss == 0 && _config.localSuperstep() == 0) {
    _workerContext->_conductorAggregators = _conductorAggregators.get();
    _workerContext->_workerAggregators = _workerAggregators.get();
    _workerContext->_vertexCount = data.get(Utils::vertexCountKey).getUInt();
    _workerContext->_edgeCount = data.get(Utils::edgeCountKey).getUInt();
    _workerContext->preApplication();
  }

  // make us ready to receive messages
  _config._globalSuperstep = gss;
  // write cache becomes the readable cache
  if (_config.asynchronousMode()) {
    TRI_ASSERT(_readCache->containedMessageCount() == 0);
    TRI_ASSERT(_writeCache->containedMessageCount() == 0);
    WriteLocker<ReadWriteLock> wguard(&_cacheRWLock);
    std::swap(_readCache, _writeCacheNextGSS);
    _writeCache->clear();
    _requestedNextGSS = false;  // only relevant for async
    _messageStats.sendCount = _nextGSSSendMessageCount;
    _nextGSSSendMessageCount = 0;
  } else {
    TRI_ASSERT(_readCache->containedMessageCount() == 0);
    WriteLocker<ReadWriteLock> wguard(&_cacheRWLock);
    std::swap(_readCache, _writeCache);
    _config._localSuperstep = gss;
  }

  // only place where is makes sense to call this, since startGlobalSuperstep
  // might not be called again
  if (_workerContext && gss > 0) {
    _workerContext->postGlobalSuperstep(gss - 1);
  }

  // responds with info which allows the conductor to decide whether
  // to start the next GSS or end the execution
  VPackBuilder response;
  response.openObject();
  response.add(Utils::senderKey, VPackValue(ServerState::instance()->getId()));
  response.add(Utils::activeCountKey, VPackValue(_activeCount));
  response.add(Utils::vertexCountKey,
               VPackValue(_graphStore->localVertexCount()));
  response.add(Utils::edgeCountKey, VPackValue(_graphStore->localEdgeCount()));
  _workerAggregators->serializeValues(response);
  response.close();

  LOG_TOPIC(INFO, Logger::PREGEL) << "Responded: " << response.toJson();
  return response;
}

template <typename V, typename E, typename M>
void Worker<V, E, M>::receivedMessages(VPackSlice const& data) {
  VPackSlice gssSlice = data.get(Utils::globalSuperstepKey);
  uint64_t gss = gssSlice.getUInt();
  if (gss == _config._globalSuperstep) {
    {  // make sure the pointer is not changed while
       // parsing messages
      ReadLocker<ReadWriteLock> guard(&_cacheRWLock);
      // handles locking for us
      _writeCache->parseMessages(data);
    }

    // Trigger the processing of vertices
    if (_config.asynchronousMode() && _state == WorkerState::IDLE) {
      MUTEX_LOCKER(guard, _commandMutex);
      _continueAsync();
    }
  } else if (_config.asynchronousMode() &&
             gss == _config._globalSuperstep + 1) {
    ReadLocker<ReadWriteLock> guard(&_cacheRWLock);
    _writeCacheNextGSS->parseMessages(data);
  } else {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                   "Superstep out of sync");
    LOG_TOPIC(ERR, Logger::PREGEL) << "Expected: " << _config._globalSuperstep << "Got: " << gss;
  }
}

/// @brief Setup next superstep
template <typename V, typename E, typename M>
void Worker<V, E, M>::startGlobalStep(VPackSlice const& data) {
  // Only expect serial calls from the conductor.
  // Lock to prevent malicous activity
  MUTEX_LOCKER(guard, _commandMutex);
  if (_state != WorkerState::PREPARING) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_INTERNAL,
        "Cannot start a gss when the worker is not prepared");
  }
  LOG_TOPIC(INFO, Logger::PREGEL) << "Starting GSS: " << data.toJson();
  VPackSlice gssSlice = data.get(Utils::globalSuperstepKey);
  const uint64_t gss = (uint64_t)gssSlice.getUInt();
  if (gss != _config.globalSuperstep()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER, "Wrong GSS");
  }

  _workerAggregators->resetValues(true);
  _conductorAggregators->resetValues(true);
  _conductorAggregators->parseValues(data);
  // execute context
  if (_workerContext) {
    _workerContext->_vertexCount = data.get(Utils::vertexCountKey).getUInt();
    _workerContext->_edgeCount = data.get(Utils::edgeCountKey).getUInt();
    _workerContext->preGlobalSuperstep(gss);
  }

  LOG_TOPIC(INFO, Logger::PREGEL) << "Worker starts new gss: " << gss;
  _startProcessing();  // sets _state = COMPUTING;
}

template <typename V, typename E, typename M>
void Worker<V, E, M>::cancelGlobalStep(VPackSlice const& data) {
  MUTEX_LOCKER(guard, _commandMutex);
  _state = WorkerState::DONE;
}

/// WARNING only call this while holding the _commandMutex
template <typename V, typename E, typename M>
void Worker<V, E, M>::_startProcessing() {
  _state = WorkerState::COMPUTING;
  _activeCount = 0;  // active count is only valid after the run

  ThreadPool* pool = PregelFeature::instance()->threadPool();
  size_t total = _graphStore->localVertexCount();
  size_t delta = total / pool->numThreads();
  size_t start = 0, end = delta;
  if (delta >= 100 && total >= 100) {
    _runningThreads = total / delta;  // rounds-up unsigned integers
  } else {
    _runningThreads = 1;
    end = total;
  }
  do {
    pool->enqueue([this, start, end] {
      if (_state != WorkerState::COMPUTING) {
        LOG_TOPIC(INFO, Logger::PREGEL) << "Execution aborted prematurely.";
        return;
      }
      auto vertices = _graphStore->vertexIterator(start, end);
      // should work like a join operation
      if (_processVertices(vertices) && _state == WorkerState::COMPUTING) {
        _finishedProcessing();  // last thread turns the lights out
      }
    });
    start = end; end = end + delta;
    if (total < end + delta) {  // swallow the rest
      end = total;
    }
  } while (start != total);
}

template <typename V, typename E, typename M>
void Worker<V, E, M>::_initializeVertexContext(VertexContext<V, E, M>* ctx) {
  ctx->_gss = _config.globalSuperstep();
  ctx->_lss = _config.localSuperstep();
  ctx->_context = _workerContext.get();
  ctx->_graphStore = _graphStore.get();
  ctx->_conductorAggregators = _conductorAggregators.get();
}

// internally called in a WORKER THREAD!!
template <typename V, typename E, typename M>
bool Worker<V, E, M>::_processVertices(
    RangeIterator<VertexEntry>& vertexIterator) {
  double start = TRI_microtime();

  // thread local caches
  std::unique_ptr<InCache<M>> inCache;
  std::unique_ptr<OutCache<M>> outCache;
  if (_messageCombiner) {
    inCache.reset(new CombiningInCache<M>(nullptr, _messageFormat.get(),
                                          _messageCombiner.get()));
    if (_config.asynchronousMode()) {
      outCache.reset(new CombiningOutCache<M>(
          &_config, (CombiningInCache<M>*)inCache.get(), _writeCacheNextGSS));
    } else {
      outCache.reset(new CombiningOutCache<M>(
          &_config, (CombiningInCache<M>*)inCache.get()));
    }
  } else {
    inCache.reset(new ArrayInCache<M>(nullptr, _messageFormat.get()));
    if (_config.asynchronousMode()) {
      outCache.reset(
          new ArrayOutCache<M>(&_config, inCache.get(), _writeCacheNextGSS));
    } else {
      outCache.reset(new ArrayOutCache<M>(&_config, inCache.get()));
    }
  }
  outCache->setBatchSize(_messageBatchSize);
  if (_config.asynchronousMode()) {
    outCache->sendToNextGSS(_requestedNextGSS);
  }

  AggregatorHandler workerAggregator(_algorithm.get());
  // TODO look if we can avoid instantiating this
  std::unique_ptr<VertexComputation<V, E, M>> vertexComputation(
      _algorithm->createComputation(&_config));
  _initializeVertexContext(vertexComputation.get());
  vertexComputation->_workerAggregators = &workerAggregator;
  vertexComputation->_cache = outCache.get();
  if (!_config.asynchronousMode()) {
    // Should cause enterNextGlobalSuperstep to do nothing
    vertexComputation->_enterNextGSS = true;
  }

  size_t activeCount = 0;
  for (VertexEntry* vertexEntry : vertexIterator) {
    MessageIterator<M> messages =
        _readCache->getMessages(vertexEntry->shard(), vertexEntry->key());

    if (messages.size() > 0 || vertexEntry->active()) {
      vertexComputation->_vertexEntry = vertexEntry;
      vertexComputation->compute(messages);
      if (vertexEntry->active()) {
        activeCount++;
      }
    }
    if (_state != WorkerState::COMPUTING) {
      LOG_TOPIC(INFO, Logger::PREGEL) << "Execution aborted prematurely.";
      break;
    }
  }
  // ==================== send messages to other shards ====================
  outCache->flushMessages();
  if (!_writeCache) {  // ~Worker was called
    return false;
  }
  if (vertexComputation->_enterNextGSS) {
    _requestedNextGSS = true;
    _nextGSSSendMessageCount += outCache->sendCountNextGSS();
  }

  // merge thread local messages, _writeCache does locking
  _writeCache->mergeCache(_config, inCache.get());
  // TODO ask how to implement message sending without waiting for a response

  MessageStats stats;
  stats.sendCount = outCache->sendCount();
  stats.superstepRuntimeSecs = TRI_microtime() - start;

  bool lastThread = false;
  {  // only one thread at a time
    MUTEX_LOCKER(guard, _threadMutex);
    // merge the thread local stats and aggregators
    _workerAggregators->aggregateValues(workerAggregator);
    _messageStats.accumulate(stats);
    _activeCount += activeCount;
    _runningThreads--;
    lastThread = _runningThreads == 0;  // should work like a join operation
  }
  return lastThread;
}

// called at the end of a worker thread, needs mutex
template <typename V, typename E, typename M>
void Worker<V, E, M>::_finishedProcessing() {
  {
    MUTEX_LOCKER(guard, _threadMutex);
    if (_runningThreads != 0) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_INTERNAL, "only one thread should ever enter this region");
    }
  }

  VPackBuilder package;
  {  // only lock after there are no more processing threads
    MUTEX_LOCKER(guard, _commandMutex);
    if (_state != WorkerState::COMPUTING) {
      return;  // probably canceled
    }

    // count all received messages
    _messageStats.receivedCount = _readCache->containedMessageCount();

    // lazy loading and async mode are a little tricky
    // the correct halting requires us to accurately track the number
    // of messages send or received, and to report them to the coordinator
    if (_config.lazyLoading()) {  // TODO how to improve this?
      // hack to determine newly added vertices
      size_t currentAVCount = _graphStore->localVertexCount();
      auto currentVertices = _graphStore->vertexIterator();
      for (VertexEntry* vertexEntry : currentVertices) {
        // reduces the containedMessageCount
        _readCache->erase(vertexEntry->shard(), vertexEntry->key());
      }

      _readCache->forEach(
          [this](prgl_shard_t shard, std::string const& key, M const&) {
            _graphStore->loadDocument(&_config, shard, key);
          });

      // only do this expensive merge operation if there are new vertices
      size_t total = _graphStore->localVertexCount();
      if (total > currentAVCount) {
        if (_config.asynchronousMode()) {
          // just process these vertices in the next superstep
          ReadLocker<ReadWriteLock> rguard(&_cacheRWLock);
          _writeCache->mergeCache(_config, _readCache);  // compute in next superstep
          _messageStats.sendCount += _readCache->containedMessageCount();
        } else {
          // TODO call _startProcessing ???
          _runningThreads = 1;
          auto addedVertices =
              _graphStore->vertexIterator(currentAVCount, total);
          _processVertices(addedVertices);
        }
      }
    }
    // no need to keep old messages around
    _readCache->clear();

    // only set the state here, because _processVertices checks for it
    _state = WorkerState::IDLE;
    _expectedGSS = _config._globalSuperstep + 1;
    _config._localSuperstep++;

    package.openObject();
    package.add(Utils::senderKey, VPackValue(ServerState::instance()->getId()));
    package.add(Utils::executionNumberKey,
                VPackValue(_config.executionNumber()));
    package.add(Utils::globalSuperstepKey,
                VPackValue(_config.globalSuperstep()));
    _messageStats.serializeValues(package);
    if (_config.asynchronousMode()) {
      _workerAggregators->serializeValues(package, true);
    }
    package.close();

    size_t tn = PregelFeature::instance()->threadPool()->numThreads();
    if (_config.asynchronousMode()) {
      // async adaptive message buffering
      _messageBatchSize = _algorithm->messageBatchSize(_config, _messageStats, tn);
    } else {
      uint32_t s = _messageStats.sendCount / tn / 2UL;
      _messageBatchSize = s > 1000 ? s : 1000;
    }
    _messageStats.resetTracking();
    LOG_TOPIC(INFO, Logger::PREGEL) << "Batch size: " << _messageBatchSize;
  }

  if (_config.asynchronousMode()) {
    bool proceed = true;
    // if the conductor is unreachable or has send data (try to) proceed
    std::unique_ptr<ClusterCommResult> result = _callConductorWithResponse(
        Utils::finishedWorkerStepPath, package.slice());
    if (result->status == CL_COMM_RECEIVED) {
      VPackSlice data = result->answer->payload();
      if ((proceed = _conductorAggregators->parseValues(data))) {
        VPackSlice nextGSS = data.get(Utils::enterNextGSSKey);
        if (nextGSS.isBool()) {
          _requestedNextGSS = nextGSS.getBool();
        }
      }
    }
    if (proceed) {
      MUTEX_LOCKER(guard, _commandMutex);
      _continueAsync();
    }
  } else {  // no answer expected
    _callConductor(Utils::finishedWorkerStepPath, package.slice());
    LOG_TOPIC(INFO, Logger::PREGEL) << "Finished GSS: " << package.toJson();
  }
}

/// WARNING only call this while holding the _commandMutex
/// in async mode checks if there are messages to process
template <typename V, typename E, typename M>
void Worker<V, E, M>::_continueAsync() {
  if (_state == WorkerState::IDLE && _writeCache->containedMessageCount() > 0) {
    {  // swap these pointers atomically
      WriteLocker<ReadWriteLock> guard(&_cacheRWLock);
      std::swap(_readCache, _writeCache);
    }
    // overwrite conductor values with local values
    _conductorAggregators->resetValues();
    _conductorAggregators->aggregateValues(*_workerAggregators.get());
    _workerAggregators->resetValues();
    _startProcessing();
  }
}

template <typename V, typename E, typename M>
void Worker<V, E, M>::finalizeExecution(VPackSlice const& body) {
  // Only expect serial calls from the conductor.
  // Lock to prevent malicous activity
  MUTEX_LOCKER(guard, _commandMutex);
  _state = WorkerState::DONE;

  VPackSlice store = body.get(Utils::storeResultsKey);
  if (store.isBool() && store.getBool() == true) {
    LOG_TOPIC(INFO, Logger::PREGEL) << "Storing results";
    // tell graphstore to remove read locks
    _graphStore->storeResults(_config);
  } else {
    LOG_TOPIC(WARN, Logger::PREGEL) << "Discarding results";
  }
  _graphStore.reset();
}

template <typename V, typename E, typename M>
void Worker<V, E, M>::aqlResult(VPackBuilder *b) const {
  MUTEX_LOCKER(guard, _commandMutex);
  
  b->openArray();
  auto it = _graphStore->vertexIterator();
  for (VertexEntry const* vertexEntry : it) {
    
    V* data = _graphStore->mutableVertexData(vertexEntry);
    b->openObject();
    b->add(StaticStrings::KeyString, VPackValue(vertexEntry->key()));
    // bool store =
    _graphStore->graphFormat()->buildVertexDocument(*b, data, sizeof(V));
    b->close();
  }
  b->close();
}

template <typename V, typename E, typename M>
void Worker<V, E, M>::startRecovery(VPackSlice const& data) {
  // other methods might lock _commandMutex
  MUTEX_LOCKER(guard, _commandMutex);
  VPackSlice method = data.get(Utils::recoveryMethodKey);
  if (method.compareString(Utils::compensate) != 0) {
    LOG_TOPIC(INFO, Logger::PREGEL) << "Unsupported operation";
    return;
  }
  // else if (method.compareString(Utils::rollback) == 0)

  _state = WorkerState::RECOVERING;
  _writeCache->clear();
  _readCache->clear();
  if (_writeCacheNextGSS) {
    _writeCacheNextGSS->clear();
  }

  VPackBuilder copy(data);
  // hack to determine newly added vertices
  _preRecoveryTotal = _graphStore->localVertexCount();
  WorkerConfig nextState(_config.database(), data);
  _graphStore->loadShards(&nextState, [this, nextState, copy] {
    _config = nextState;
    compensateStep(copy.slice());
  });
}

template <typename V, typename E, typename M>
void Worker<V, E, M>::compensateStep(VPackSlice const& data) {
  MUTEX_LOCKER(guard, _commandMutex);

  _workerAggregators->resetValues();
  _conductorAggregators->resetValues();
  _conductorAggregators->parseValues(data);

  ThreadPool* pool = PregelFeature::instance()->threadPool();
  pool->enqueue([this] {
    if (_state != WorkerState::RECOVERING) {
      LOG_TOPIC(INFO, Logger::PREGEL) << "Compensation aborted prematurely.";
      return;
    }

    auto vertexIterator = _graphStore->vertexIterator();
    std::unique_ptr<VertexCompensation<V, E, M>> vCompensate(
        _algorithm->createCompensation(&_config));
    _initializeVertexContext(vCompensate.get());
    vCompensate->_workerAggregators = _workerAggregators.get();

    size_t i = 0;
    for (VertexEntry* vertexEntry : vertexIterator) {
      vCompensate->_vertexEntry = vertexEntry;
      vCompensate->compensate(i > _preRecoveryTotal);
      i++;
      if (_state != WorkerState::RECOVERING) {
        LOG_TOPIC(INFO, Logger::PREGEL) << "Execution aborted prematurely.";
        break;
      }
    }
    VPackBuilder package;
    package.openObject();
    package.add(Utils::senderKey, VPackValue(ServerState::instance()->getId()));
    package.add(Utils::executionNumberKey,
                VPackValue(_config.executionNumber()));
    package.add(Utils::globalSuperstepKey,
                VPackValue(_config.globalSuperstep()));
    _workerAggregators->serializeValues(package);
    package.close();
    _callConductor(Utils::finishedRecoveryPath, package.slice());
  });
}

template <typename V, typename E, typename M>
void Worker<V, E, M>::finalizeRecovery(VPackSlice const& data) {
  MUTEX_LOCKER(guard, _commandMutex);
  if (_state != WorkerState::RECOVERING) {
    LOG_TOPIC(INFO, Logger::PREGEL) << "Compensation aborted prematurely.";
    return;
  }

  _expectedGSS = data.get(Utils::globalSuperstepKey).getUInt();
  _writeCache->clear();
  _readCache->clear();
  if (_writeCacheNextGSS) {
    _writeCacheNextGSS->clear();
  }
  _messageStats.resetTracking();
  _state = WorkerState::IDLE;
  LOG_TOPIC(INFO, Logger::PREGEL) << "Recovery finished";
}

template <typename V, typename E, typename M>
void Worker<V, E, M>::_callConductor(std::string const& path,
                                     VPackSlice const& message) {
  std::shared_ptr<ClusterComm> cc = ClusterComm::instance();
  std::string baseUrl = Utils::baseUrl(_config.database());
  CoordTransactionID coordinatorTransactionID = TRI_NewTickServer();
  auto headers =
      std::make_unique<std::unordered_map<std::string, std::string>>();
  auto body = std::make_shared<std::string const>(message.toJson());
  cc->asyncRequest("", coordinatorTransactionID,
                   "server:" + _config.coordinatorId(), rest::RequestType::POST,
                   baseUrl + path, body, headers, nullptr,
                   120.0,  // timeout
                   true);  // single request, no answer expected
}

template <typename V, typename E, typename M>
std::unique_ptr<ClusterCommResult> Worker<V, E, M>::_callConductorWithResponse(
    std::string const& path, VPackSlice const& message) {
  LOG_TOPIC(INFO, Logger::PREGEL) << "Calling the conductor";
  std::shared_ptr<ClusterComm> cc = ClusterComm::instance();
  std::string baseUrl = Utils::baseUrl(_config.database());
  CoordTransactionID coordinatorTransactionID = TRI_NewTickServer();
  std::unordered_map<std::string, std::string> headers;
  return cc->syncRequest("", coordinatorTransactionID,
                         "server:" + _config.coordinatorId(),
                         rest::RequestType::POST, baseUrl + path,
                         message.toJson(), headers, 120.0);
}

// template types to create
template class arangodb::pregel::Worker<int64_t, int64_t, int64_t>;
template class arangodb::pregel::Worker<float, float, float>;
template class arangodb::pregel::Worker<double, float, double>;
// custom algorihm types
template class arangodb::pregel::Worker<SCCValue, int32_t,
                                        SenderMessage<uint64_t>>;
