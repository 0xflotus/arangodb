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
/// @author Tobias Goedderz
/// @author Michael Hackstein
/// @author Heiko Kernbach
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#include "catch.hpp"
#include "fakeit.hpp"

#include "BlockFetcherHelper.h"

#include "Aql/AllRowsFetcher.h"
#include "Aql/AqlItemBlock.h"
#include "Aql/AqlItemRow.h"
#include "Aql/ExecutorInfos.h"
#include "Aql/ExecutionNode.h"
#include "Aql/SortExecutor.h"
#include "Aql/SortRegister.h"
#include "Aql/ResourceUsage.h"
#include "Aql/Variable.h"
#include "Transaction/Context.h"
#include "Transaction/Methods.h"

#include "search/sort.hpp"

#include <velocypack/Builder.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;
using namespace arangodb::aql;

namespace arangodb {
namespace tests {
namespace aql {

int compareAqlValues(
    irs::sort::prepared const*,
    arangodb::transaction::Methods* trx,
    arangodb::aql::AqlValue const& lhs,
    arangodb::aql::AqlValue const& rhs) {
  return arangodb::aql::AqlValue::Compare(trx, lhs, rhs, true);
}

SCENARIO("SortExecutor", "[AQL][EXECUTOR]") {
  ExecutionState state;

  ResourceMonitor monitor;
  AqlItemBlock block(&monitor, 1000, 1);

  // Mock of the Transaction
  // Enough for this test, will only be passed through and accessed
  // on documents alone.
  fakeit::Mock<transaction::Methods> mockTrx;
  transaction::Methods& trx = mockTrx.get();

  fakeit::Mock<transaction::Context> mockContext;
  transaction::Context& ctxt = mockContext.get();

  fakeit::When(Method(mockTrx, transactionContextPtr)).AlwaysReturn(&ctxt);
  fakeit::When(Method(mockContext, getVPackOptions)).AlwaysReturn(&arangodb::velocypack::Options::Defaults);

  Variable sortVar("mySortVar", 0);
  std::vector<SortRegister> sortRegisters;
  SortElement sl{&sortVar, true};
  SortRegister sortReg(0, sl, &compareAqlValues);
  sortRegisters.emplace_back(std::move(sortReg));
  SortExecutorInfos infos(0, 0, &trx, std::move(sortRegisters), false);

  RegInfo regInfo{};
  regInfo.numRegs = 1;
  regInfo.toKeep = {0};
  regInfo.toClear = {};

  GIVEN("there are no rows upstream") {
    VPackBuilder input;

    WHEN("the producer does not wait") {
      AllRowsFetcherHelper fetcher(input.steal(), false);
      SortExecutor testee(fetcher, infos);

      THEN("the executor should return DONE with nullptr") {
        AqlItemRow result(block, 0, regInfo);
        state = testee.produceRow(result);
        REQUIRE(state == ExecutionState::DONE);
        REQUIRE(!result.produced());
      }
    }

    WHEN("the producer waits") {
      AllRowsFetcherHelper fetcher(input.steal(), true);
      SortExecutor testee(fetcher, infos);

      THEN("the executor should first return WAIT with nullptr") {
        AqlItemRow result(block, 0, regInfo);
        state = testee.produceRow(result);
        REQUIRE(state == ExecutionState::WAITING);
        REQUIRE(!result.produced());

        AND_THEN("the executor should return DONE with nullptr") {
          state = testee.produceRow(result);
          REQUIRE(state == ExecutionState::DONE);
          REQUIRE(!result.produced());
        }
      }

    }
  }

  GIVEN("there are rows from upstream, and we are waiting") {
    std::shared_ptr<VPackBuilder> input;

    WHEN("it is a simple list of numbers") {
      input = VPackParser::fromJson("[[5],[3],[1],[2],[4]]");
      AllRowsFetcherHelper fetcher(input->steal(), true);
      SortExecutor testee(fetcher, infos);

      THEN("we will hit waiting 5 times") {
        AqlItemRow firstResult(block, 0, regInfo);
        // Wait, 5, Wait, 3, Wait, 1, Wait, 2, Wait, 4, HASMORE
        for (size_t i = 0; i < 5; ++i) {
          state = testee.produceRow(firstResult);
          REQUIRE(state == ExecutionState::WAITING);
          REQUIRE(!firstResult.produced());
        }

        AND_THEN("we procude the rows in order") {
          state = testee.produceRow(firstResult);
          REQUIRE(state == ExecutionState::HASMORE);
          REQUIRE(firstResult.produced());

          AqlItemRow secondResult(block, 1, regInfo);
          state = testee.produceRow(secondResult);
          REQUIRE(state == ExecutionState::HASMORE);
          REQUIRE(secondResult.produced());

          AqlItemRow thirdResult(block, 2, regInfo);
          state = testee.produceRow(thirdResult);
          REQUIRE(state == ExecutionState::HASMORE);
          REQUIRE(thirdResult.produced());

          AqlItemRow fourthResult(block, 3, regInfo);
          state = testee.produceRow(fourthResult);
          REQUIRE(state == ExecutionState::HASMORE);
          REQUIRE(fourthResult.produced());

          AqlItemRow fifthResult(block, 4, regInfo);
          state = testee.produceRow(fifthResult);
          REQUIRE(state == ExecutionState::DONE);
          REQUIRE(fifthResult.produced());

          AqlValue v = firstResult.getValue(0);
          REQUIRE(v.isNumber());
          int64_t number = v.toInt64(nullptr);
          REQUIRE(number == 1);

          v = secondResult.getValue(0);
          REQUIRE(v.isNumber());
          number = v.toInt64(nullptr);
          REQUIRE(number == 2);

          v = thirdResult.getValue(0);
          REQUIRE(v.isNumber());
          number = v.toInt64(nullptr);
          REQUIRE(number == 3);

          v = fourthResult.getValue(0);
          REQUIRE(v.isNumber());
          number = v.toInt64(nullptr);
          REQUIRE(number == 4);

          v = fifthResult.getValue(0);
          REQUIRE(v.isNumber());
          number = v.toInt64(nullptr);
          REQUIRE(number == 5);
        }
      }
    }
  }
}
} // aql
} // tests
} // arangodb
