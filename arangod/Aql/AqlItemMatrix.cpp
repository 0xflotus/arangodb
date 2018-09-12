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
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#include "AqlItemMatrix.h"

#include "Aql/AqlItemBlock.h"
#include "Aql/AqlItemRow.h"

using namespace arangodb;
using namespace arangodb::aql;

AqlItemMatrix::AqlItemMatrix() : _size(0) {}

AqlItemMatrix::~AqlItemMatrix() {}

void AqlItemMatrix::addBlock(std::shared_ptr<AqlItemBlock> block) {
  _blocks.emplace_back(block);
  _size += block->size();
}

size_t AqlItemMatrix::size() const {
  return _size;
}

bool AqlItemMatrix::empty() const {
  return _blocks.empty();
}

AqlItemRow const* AqlItemMatrix::getRow(size_t index) const {
  TRI_ASSERT(index < _size);

  for (auto it = _blocks.begin(); it != _blocks.end() ; ++it) {
    auto& block = *it;
    if (index < block->size()) {
      // We are in this row
      RegInfo info;
      info.numRegs = block->getNrRegs();

      _lastRow = std::make_unique<AqlItemRow const>(block.get(), index, info);
      return _lastRow.get();
    }

    // Jump over this block
    index -= (*it)->size();
  }
  // We have asked for a row outside of this Vector
  THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, "Internal Aql Logic Error: An exector block is reading out of bounds.");
}
