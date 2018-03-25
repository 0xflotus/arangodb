////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2018 ArangoDB GmbH, Cologne, Germany
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

#ifndef ARANGODB_HYDRA_SHARDING_H
#define ARANGODB_HYDRA_SHARDING_H 1

#include <string>

namespace husky {

/// Sharding interface to handle sharding in a transparent way
class ShardingBase {
 public:
  
  template<typename KeyT>
  std::string lookupTarget(KeyT const& key) const {
    lookupTargetInternal(&key, sizeof(KeyT));
  }
  
protected:
  virtual std::string lookupTargetInternal(void const* ptr, size_t len) const = 0;
};
  
class CollectionSharding : public ShardingBase {
   CollectionSharding(std::string const& cname);
   
   std::string lookupTargetInternal(void const* ptr, size_t len) const;
 private:
   std::string const _collection;
 }
  
class SimpleSharding : public ShardingBase {
  SimpleSharding(uint64_t seed) : _seed(seed) {}
  
  std::string lookupTargetInternal(void const* ptr, size_t len) const;
private:
  uint64_t const _seed;
}

}  // namespace husky
