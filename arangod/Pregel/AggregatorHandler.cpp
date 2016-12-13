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

#include "Pregel/AggregatorHandler.h"
#include "Pregel/Aggregator.h"
#include "Pregel/Algorithm.h"

using namespace arangodb;
using namespace arangodb::pregel;

AggregatorHandler::~AggregatorHandler() {
  for (auto const& it : _values) {
    delete it.second;
  }
  _values.clear();
}

void AggregatorHandler::aggregate(std::string const& name,
                                  const void* valuePtr) {
  auto it = _values.find(name);
  if (it != _values.end()) {
    it->second->aggregate(valuePtr);
  } else {
    std::unique_ptr<Aggregator> agg(_create->aggregator(name));
    if (agg) {
      agg->aggregate(valuePtr);
      _values[name] = agg.get();
      agg.release();
    }
  }
}

const void* AggregatorHandler::getAggregatedValue(
    std::string const& name) const {
  auto const& it = _values.find(name);
  if (it != _values.end()) {
    return it->second->getValue();
  }
  return nullptr;
}

void AggregatorHandler::resetValues() {
  for (auto& it : _values) {
    if (!it.second->isPermanent()) {
      it.second->reset();
    }
  }
}

void AggregatorHandler::aggregateValues(AggregatorHandler const& workerValues) {
  for (auto const& pair : workerValues._values) {
    std::string const& name = pair.first;
    auto my = _values.find(name);
    if (my != _values.end()) {
      my->second->aggregate(pair.second->getValue());
    } else {
      std::unique_ptr<Aggregator> agg(_create->aggregator(name));
      if (agg) {
        agg->aggregate(pair.second->getValue());
        _values[name] = agg.get();
        agg.release();
      }
    }
  }
}

void AggregatorHandler::aggregateValues(VPackSlice workerValues) {
  for (auto const& keyValue : VPackObjectIterator(workerValues)) {
    std::string name = keyValue.key.copyString();
    auto const& it = _values.find(name);
    if (it != _values.end()) {
      it->second->aggregate(keyValue.value);
    } else {
      std::unique_ptr<Aggregator> agg(_create->aggregator(name));
      if (agg) {
        agg->aggregate(keyValue.value);
        _values[name] = agg.get();
        agg.release();
      }
    }
  }
}

void AggregatorHandler::serializeValues(VPackBuilder& b) const {
  for (auto const& pair : _values) {
    b.add(pair.first, pair.second->vpackValue());
  }
}

size_t AggregatorHandler::size() { return _values.size(); }
