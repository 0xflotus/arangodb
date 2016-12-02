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

#include "OutgoingCache.h"
#include "IncomingCache.h"
#include "Utils.h"
#include "WorkerState.h"

#include "Basics/MutexLocker.h"
#include "Basics/StaticStrings.h"
#include "Cluster/ClusterComm.h"
#include "VocBase/LogicalCollection.h"

#include <velocypack/Iterator.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;
using namespace arangodb::pregel;

template <typename M>
OutgoingCache<M>::OutgoingCache(WorkerState* state, MessageFormat<M>* format,
                                MessageCombiner<M>* combiner,
                                IncomingCache<M>* cache)
    : _state(state), _format(format), _combiner(combiner), _localCache(cache) {
  _baseUrl = Utils::baseUrl(_state->database());
}

template <typename M>
OutgoingCache<M>::~OutgoingCache() {
  clear();
}

template <typename M>
void OutgoingCache<M>::clear() {
  _shardMap.clear();
  _containedMessages = 0;
}

template <typename M>
void OutgoingCache<M>::appendMessage(prgl_shard_t shard, std::string const& key, M const& data) {

  if (_state->isLocalVertexShard(shard)) {
    _localCache->setDirect(shard, key, data);
    LOG(INFO) << "Worker: Got messages for myself " << key << " <- "
              << data;
    _sendMessages++;
  } else {
    // std::unordered_shardMap<std::string, VPackBuilder*> vertexMap =;
    std::unordered_map<std::string, M>& vertexMap = _shardMap[shard];
    auto it = vertexMap.find(key);
    if (it != vertexMap.end()) {  // more than one message
      vertexMap[key] = _combiner->combine(vertexMap[key], data);
    } else {  // first message for this vertex
      vertexMap.emplace(key, data);
    }
    _containedMessages++;
    
    if (_containedMessages > 1000) {
      flushMessages();
    }
  }
}

template <typename M>
void OutgoingCache<M>::flushMessages() {
  LOG(INFO) << "Beginning to send messages to other machines";

  std::vector<ClusterCommRequest> requests;
  for (auto const& it : _shardMap) {
    prgl_shard_t shard = it.first;
    std::unordered_map<std::string, M> const& vertexMessageMap = it.second;
    if (vertexMessageMap.size() == 0) {
      continue;
    }

    VPackBuilder package;
    package.openObject();
    package.add(Utils::messagesKey, VPackValue(VPackValueType::Array));
    for (auto const& vertexMessagePair : vertexMessageMap) {
      package.add(VPackValue(vertexMessagePair.first));
      _format->addValue(package, vertexMessagePair.second);
      _sendMessages++;
    }
    package.close();
    package.add(Utils::senderKey, VPackValue(ServerState::instance()->getId()));
    package.add(Utils::executionNumberKey,
                VPackValue(_state->executionNumber()));
    package.add(Utils::globalSuperstepKey,
                VPackValue(_state->globalSuperstep()));
    package.close();
    // add a request
    ShardID const& shardId = _state->globalShardIDs()[shard];
    auto body = std::make_shared<std::string>(package.toJson());
    requests.emplace_back("shard:" + shardId, rest::RequestType::POST,
                          _baseUrl + Utils::messagesPath, body);

    LOG(INFO) << "Worker: Sending data to other Shard: " << shardId
              << ". Message: " << package.toJson();
  }
  size_t nrDone = 0;
  ClusterComm::instance()->performRequests(requests, 120, nrDone,
                                           LogTopic("Pregel message transfer"));
  // readResults(requests);
  for (auto const& req : requests) {
    auto& res = req.result;
    if (res.status == CL_COMM_RECEIVED) {
      LOG(INFO) << res.answer->payload().toJson();
    }
  }
  this->clear();
}

// template types to create
template class arangodb::pregel::OutgoingCache<int64_t>;
template class arangodb::pregel::OutgoingCache<float>;
