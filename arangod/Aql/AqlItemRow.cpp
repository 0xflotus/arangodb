////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2018-2018 ArangoDB GmbH, Cologne, Germany
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

#include "Aql/AqlItemRow.h"
#include "Aql/AqlItemBlock.h"
#include "Aql/AqlValue.h"

using namespace arangodb;
using namespace arangodb::aql;

AqlItemRow::AqlItemRow(AqlItemBlock &block, size_t baseIndex, RegInfo info)
    : _block(block)
    , _baseIndex(baseIndex)
    , _registerInfo(std::move(info))
    , _produced(false)
    {}

const AqlValue& AqlItemRow::getValue(RegisterId variableNr) const {
  TRI_ASSERT(variableNr < getNrRegisters());
  return _block.getValueReference(_baseIndex, variableNr);
}

void AqlItemRow::setValue(RegisterId variableNr, AqlItemRow const& sourceRow, AqlValue const& value) {
  TRI_ASSERT(variableNr < getNrRegisters());
  _block.emplaceValue(_baseIndex, variableNr, value);
  copyRow(sourceRow);
}

void AqlItemRow::copyRow(AqlItemRow const& sourceRow) {
  if (_produced) {
    return;
  }

  // copy entries to keep
  for (auto itemId : _registerInfo.toKeep) {
    _block.emplaceValue(_baseIndex, itemId, sourceRow.getValue(itemId));
  }

  _produced = true;
}
