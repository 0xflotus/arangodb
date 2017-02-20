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

#ifndef ARANGODB_PREGEL_GRAPH_STRUCTURE_H
#define ARANGODB_PREGEL_GRAPH_STRUCTURE_H 1

#include <cstdint>
#include <string>
#include <functional>

namespace arangodb {
namespace pregel {

typedef uint16_t prgl_shard_t;
const prgl_shard_t invalid_prgl_shard = -1;
struct PregelID {
  prgl_shard_t shard;
  std::string key;

  PregelID() : shard(0), key("") {}
  PregelID(prgl_shard_t s, std::string const& k) : shard(s), key(k) {}

  inline bool operator==(const PregelID& rhs) {
    return shard == rhs.shard && key == rhs.key;
  }
  
  bool inline isValid() const {
    return shard != invalid_prgl_shard && !key.empty();
  }
};

template <typename V, typename E>
class GraphStore;

/// @brief header entry for the edge file
template <typename E>
class Edge {
  template <typename V, typename E2>
  friend class GraphStore;

  prgl_shard_t _sourceShard;
  prgl_shard_t _targetShard;
  std::string _toKey;
  E _data;

 public:
  // EdgeEntry() : _nextEntryOffset(0), _dataSize(0), _vertexIDSize(0) {}
  Edge() {}
  Edge(prgl_shard_t source, prgl_shard_t target, std::string const& key)
      : _sourceShard(source), _targetShard(target), _toKey(key) {}

  // size_t getSize() { return sizeof(EdgeEntry) + _vertexIDSize + _dataSize; }
  std::string const& toKey() const { return _toKey; }
  // size_t getDataSize() { return _dataSize; }
  inline E* data() {
    return &_data;  // static_cast<E>(this + sizeof(EdgeEntry) + _vertexIDSize);
  }
  inline prgl_shard_t sourceShard() const { return _sourceShard; }
  inline prgl_shard_t targetShard() const { return _targetShard; }
};

class VertexEntry {
  template <typename V, typename E>
  friend class GraphStore;

  prgl_shard_t _shard;
  std::string _key;
  size_t _vertexDataOffset = 0;
  size_t _edgeDataOffset = 0;
  size_t _edgeCount = 0;
  bool _active = true;

 public:
  VertexEntry() {}
  VertexEntry(prgl_shard_t shard, std::string const& key)
      : _shard(shard),
        _key(key),
        _vertexDataOffset(0),
        _edgeDataOffset(0),
        _edgeCount(0),
        _active(true) {}  //_vertexIDSize(0)

  inline size_t getVertexDataOffset() const { return _vertexDataOffset; }
  inline size_t getEdgeDataOffset() const { return _edgeDataOffset; }
  // inline size_t getSize() { return sizeof(VertexEntry) + _vertexIDSize; }
  inline size_t getSize() { return sizeof(VertexEntry); }
  inline bool active() const { return _active; }
  inline void setActive(bool bb) { _active = bb; }

  inline prgl_shard_t shard() const { return _shard; }
  inline std::string const& key() const { return _key; };
  PregelID pregelId() const { return PregelID(_shard, _key); }
  /*std::string const& key() const {
    return std::string(_key, _keySize);
  };*/
};

// unused right now
/*class LinkedListIterator {
 private:
  intptr_t _begin, _end, _current;

  VertexIterator(const VertexIterator&) = delete;
  VertexIterator& operator=(const FileInfo&) = delete;

 public:
  typedef VertexIterator iterator;
  typedef const VertexIterator const_iterator;

  VertexIterator(intptr_t beginPtr, intptr_t endPtr)
      : _begin(beginPtr), _end(endPtr), _current(beginPtr) {}

  iterator begin() { return VertexIterator(_begin, _end); }
  const_iterator begin() const { return VertexIterator(_begin, _end); }
  iterator end() {
    auto it = VertexIterator(_begin, _end);
    it._current = it._end;
    return it;
  }
  const_iterator end() const {
    auto it = VertexIterator(_begin, _end);
    it._current = it._end;
    return it;
  }

  // prefix ++
  VertexIterator& operator++() {
    VertexEntry* entry = (VertexEntry*)_current;
    _current += entry->getSize();
    return *this;
  }

  // postfix ++
  VertexIterator& operator++(int) {
    VertexEntry* entry = (VertexEntry*)_current;
    _current += entry->getSize();
    return *this;
  }

  VertexEntry* operator*() const {
    return _current != _end ? (VertexEntry*)_current : nullptr;
  }

  bool operator!=(VertexIterator const& other) const {
    return _current != other._current;
  }
};*/
}
}

namespace std {
  template <>
  struct hash<arangodb::pregel::PregelID> {
    std::size_t operator()(const arangodb::pregel::PregelID& k) const {
      using std::size_t;
      using std::hash;
      using std::string;
      
      // Compute individual hash values for first,
      // second and third and combine them using XOR
      // and bit shifting:
      std::size_t h1 = std::hash<string>()(k.key);
      std::size_t h2 = std::hash<int32_t>()(k.shard);
      return h1 ^ (h2 << 1);
    }
  };
}

#endif
