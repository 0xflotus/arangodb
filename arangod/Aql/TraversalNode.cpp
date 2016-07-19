/// @brief Implementation of Traversal Execution Node
///
/// @file arangod/Aql/TraversalNode.cpp
///
/// DISCLAIMER
///
/// Copyright 2010-2014 triagens GmbH, Cologne, Germany
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
/// @author Copyright 2015, ArangoDB GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#include "Aql/TraversalNode.h"
#include "Aql/ExecutionPlan.h"
#include "Aql/Ast.h"
#include "Aql/SortCondition.h"
#include "Aql/TraversalOptions.h"
#include "Indexes/Index.h"

#include <iostream>

using namespace arangodb::basics;
using namespace arangodb::aql;

static uint64_t checkTraversalDepthValue(AstNode const* node) {
  if (!node->isNumericValue()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_PARSE,
                                   "invalid traversal depth");
  }
  double v = node->getDoubleValue();
  double intpart;
  if (modf(v, &intpart) != 0.0 || v < 0.0) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_PARSE,
                                   "invalid traversal depth");
  }
  return static_cast<uint64_t>(v);
}

TraversalNode::EdgeConditionBuilder::EdgeConditionBuilder(
    TraversalNode const* tn)
    : _tn(tn), _containsCondition(false) {
      _modCondition = _tn->_ast->createNodeNaryOperator(NODE_TYPE_OPERATOR_NARY_AND);
    }

void TraversalNode::EdgeConditionBuilder::addConditionPart(AstNode const* part) {
  _modCondition->addMember(part);
}

AstNode* TraversalNode::EdgeConditionBuilder::getOutboundCondition() {
  if (_containsCondition) {
    _modCondition->changeMember(_modCondition->numMembers() - 1, _tn->_fromCondition);
  } else {
    if (_tn->_globalEdgeCondition != nullptr) {
      _modCondition->addMember(_tn->_globalEdgeCondition);
    }
    _modCondition->addMember(_tn->_fromCondition);
    _containsCondition = true;
  }
  return _modCondition;
};

AstNode* TraversalNode::EdgeConditionBuilder::getInboundCondition() {
  if (_containsCondition) {
    _modCondition->changeMember(_modCondition->numMembers() - 1, _tn->_toCondition);
  } else {
    if (_tn->_globalEdgeCondition != nullptr) {
      _modCondition->addMember(_tn->_globalEdgeCondition);
    }
    _modCondition->addMember(_tn->_toCondition);
    _containsCondition = true;
  }
  return _modCondition;
};

static TRI_edge_direction_e parseDirection (AstNode const* node) {
  TRI_ASSERT(node->isIntValue());
  auto dirNum = node->getIntValue();

  switch (dirNum) {
    case 0:
      return TRI_EDGE_ANY;
    case 1:
      return TRI_EDGE_IN;
    case 2:
      return TRI_EDGE_OUT;
    default:
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_QUERY_PARSE,
          "direction can only be INBOUND, OUTBOUND or ANY");
  }
}

TraversalNode::TraversalNode(ExecutionPlan* plan, size_t id,
                             TRI_vocbase_t* vocbase, AstNode const* direction,
                             AstNode const* start, AstNode const* graph,
                             TraversalOptions const& options)
    : ExecutionNode(plan, id),
      _vocbase(vocbase),
      _vertexOutVariable(nullptr),
      _edgeOutVariable(nullptr),
      _pathOutVariable(nullptr),
      _inVariable(nullptr),
      _graphObj(nullptr),
      _condition(nullptr),
      _options(options),
      _specializedNeighborsSearch(false),
      _ast(plan->getAst()),
      _tmpObjVariable(_ast->variables()->createTemporaryVariable()),
      _tmpObjVarNode(_ast->createNodeReference(_tmpObjVariable)),
      _tmpIdNode(_ast->createNodeValueString("", 0)),
      _globalEdgeCondition(nullptr),
      _globalVertexCondition(nullptr) {
  TRI_ASSERT(_vocbase != nullptr);
  TRI_ASSERT(direction != nullptr);
  TRI_ASSERT(start != nullptr);
  TRI_ASSERT(graph != nullptr);

  // Let us build the conditions on _from and _to. Just in case we need them.
  {
    auto const* access = _ast->createNodeAttributeAccess(
        _tmpObjVarNode, StaticStrings::FromString.c_str(),
        StaticStrings::FromString.length());
    _fromCondition = _ast->createNodeBinaryOperator(
        NODE_TYPE_OPERATOR_BINARY_EQ, access, _tmpIdNode);
  }
  TRI_ASSERT(_fromCondition != nullptr);

  {
    auto const* access = _ast->createNodeAttributeAccess(
        _tmpObjVarNode, StaticStrings::ToString.c_str(),
        StaticStrings::ToString.length());
    _toCondition = _ast->createNodeBinaryOperator(NODE_TYPE_OPERATOR_BINARY_EQ,
                                                  access, _tmpIdNode);
  }
  TRI_ASSERT(_toCondition != nullptr);

  auto resolver = std::make_unique<CollectionNameResolver>(vocbase);

  // Parse Steps and direction
  TRI_ASSERT(direction->type == NODE_TYPE_DIRECTION);
  TRI_ASSERT(direction->numMembers() == 2);
  // Member 0 is the direction. Already the correct Integer.
  // Is not inserted by user but by enum.
  TRI_edge_direction_e baseDirection = parseDirection(direction->getMember(0));

  auto steps = direction->getMember(1);

  if (steps->isNumericValue()) {
    // Check if a double value is integer
    _minDepth = checkTraversalDepthValue(steps);
    _maxDepth = _minDepth;
  } else if (steps->type == NODE_TYPE_RANGE) {
    // Range depth
    _minDepth = checkTraversalDepthValue(steps->getMember(0));
    _maxDepth = checkTraversalDepthValue(steps->getMember(1));

    if (_maxDepth < _minDepth) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_PARSE,
                                     "invalid traversal depth");
    }
  } else {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_PARSE,
                                   "invalid traversal depth");
  }

  std::unordered_map<std::string, TRI_edge_direction_e> seenCollections;

  if (graph->type == NODE_TYPE_COLLECTION_LIST) {
    size_t edgeCollectionCount = graph->numMembers();
    _graphJson = arangodb::basics::Json(arangodb::basics::Json::Array,
                                        edgeCollectionCount);
    _edgeColls.reserve(edgeCollectionCount);
    _directions.reserve(edgeCollectionCount);
    // List of edge collection names
    for (size_t i = 0; i < edgeCollectionCount; ++i) {
      auto col = graph->getMember(i);
      TRI_edge_direction_e dir = TRI_EDGE_ANY;
      
      if (col->type == NODE_TYPE_DIRECTION) {
        // We have a collection with special direction.
        dir = parseDirection(col->getMember(0));
        col = col->getMember(1);
      } else {
        dir = baseDirection;
      }
        
      std::string eColName = col->getString();
      
      // now do some uniqueness checks for the specified collections
      auto it = seenCollections.find(eColName);
      if (it != seenCollections.end()) {
        if ((*it).second != dir) {
          std::string msg("conflicting directions specified for collection '" +
                          std::string(eColName));
          THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_ARANGO_COLLECTION_TYPE_INVALID,
                                         msg);
        }
        // do not re-add the same collection!
        continue;
      }
      seenCollections.emplace(eColName, dir);
      
      auto eColType = resolver->getCollectionTypeCluster(eColName);
      if (eColType != TRI_COL_TYPE_EDGE) {
        std::string msg("collection type invalid for collection '" +
                        std::string(eColName) +
                        ": expecting collection type 'edge'");
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_ARANGO_COLLECTION_TYPE_INVALID,
                                       msg);
      }
      
      _directions.emplace_back(dir);
      _graphJson.add(arangodb::basics::Json(eColName));
      _edgeColls.emplace_back(eColName);
    }
  } else {
    if (_edgeColls.empty()) {
      if (graph->isStringValue()) {
        std::string graphName = graph->getString();
        _graphJson = arangodb::basics::Json(graphName);
        _graphObj = plan->getAst()->query()->lookupGraphByName(graphName);

        if (_graphObj == nullptr) {
          THROW_ARANGO_EXCEPTION(TRI_ERROR_GRAPH_NOT_FOUND);
        }

        auto eColls = _graphObj->edgeCollections();
        size_t length = eColls.size();
        if (length == 0) {
          THROW_ARANGO_EXCEPTION(TRI_ERROR_GRAPH_EMPTY);
        }
        _edgeColls.reserve(length);
        _directions.reserve(length);

        for (const auto& n : eColls) {
          _edgeColls.push_back(n);
          _directions.emplace_back(baseDirection);
        }
      }
    }
  }

  // Parse start node
  switch (start->type) {
    case NODE_TYPE_REFERENCE:
      _inVariable = static_cast<Variable*>(start->getData());
      _vertexId = "";
      break;
    case NODE_TYPE_VALUE:
      if (start->value.type != VALUE_TYPE_STRING) {
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_PARSE,
                                       "invalid start vertex. Must either be "
                                       "an _id string or an object with _id.");
      }
      _inVariable = nullptr;
      _vertexId = start->getString();
      break;
    default:
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_PARSE,
                                     "invalid start vertex. Must either be an "
                                     "_id string or an object with _id.");
  }

  // Parse options node
}

/// @brief Internal constructor to clone the node.
TraversalNode::TraversalNode(
    ExecutionPlan* plan, size_t id, TRI_vocbase_t* vocbase,
    std::vector<std::string> const& edgeColls, Variable const* inVariable,
    std::string const& vertexId, std::vector<TRI_edge_direction_e> directions,
    uint64_t minDepth, uint64_t maxDepth, TraversalOptions const& options)
    : ExecutionNode(plan, id),
      _vocbase(vocbase),
      _vertexOutVariable(nullptr),
      _edgeOutVariable(nullptr),
      _pathOutVariable(nullptr),
      _inVariable(inVariable),
      _vertexId(vertexId),
      _minDepth(minDepth),
      _maxDepth(maxDepth),
      _directions(directions),
      _graphObj(nullptr),
      _condition(nullptr),
      _options(options),
      _specializedNeighborsSearch(false) {
  _graphJson = arangodb::basics::Json(arangodb::basics::Json::Array, edgeColls.size());

  for (auto& it : edgeColls) {
    _edgeColls.emplace_back(it);
    _graphJson.add(arangodb::basics::Json(it));
  }
}

TraversalNode::TraversalNode(ExecutionPlan* plan,
                             arangodb::basics::Json const& base)
    : ExecutionNode(plan, base),
      _vocbase(plan->getAst()->query()->vocbase()),
      _vertexOutVariable(nullptr),
      _edgeOutVariable(nullptr),
      _pathOutVariable(nullptr),
      _inVariable(nullptr),
      _graphObj(nullptr),
      _condition(nullptr),
      _specializedNeighborsSearch(false) {
  _minDepth =
      arangodb::basics::JsonHelper::stringUInt64(base.json(), "minDepth");
  _maxDepth =
      arangodb::basics::JsonHelper::stringUInt64(base.json(), "maxDepth");
  auto dirList = base.get("directions");
  TRI_ASSERT(dirList.json() != nullptr);
  for (size_t i = 0; i < dirList.size(); ++i) {
    auto dirJson = dirList.at(i);
    uint64_t dir = arangodb::basics::JsonHelper::stringUInt64(dirJson.json());
    TRI_edge_direction_e d;
    switch (dir) {
      case 0:
        d = TRI_EDGE_ANY;
        break;
      case 1:
        d = TRI_EDGE_IN;
        break;
      case 2:
        d = TRI_EDGE_OUT;
        break;
      default:
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                       "Invalid direction value");
        break;
    }
    _directions.emplace_back(d);
  }

  // In Vertex
  if (base.has("inVariable")) {
    _inVariable = varFromJson(plan->getAst(), base, "inVariable");
  } else {
    _vertexId = arangodb::basics::JsonHelper::getStringValue(base.json(),
                                                             "vertexId", "");
    if (_vertexId.empty()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_BAD_JSON_PLAN,
                                     "start vertex mustn't be empty.");
    }
  }

  if (base.has("condition")) {
    TRI_json_t const* condition =
        JsonHelper::checkAndGetObjectValue(base.json(), "condition");

    if (condition != nullptr) {
      arangodb::basics::Json conditionJson(TRI_UNKNOWN_MEM_ZONE, condition,
                                           arangodb::basics::Json::NOFREE);
      _condition = Condition::fromJson(plan, conditionJson);
    }
  }

  std::string graphName;
  if (base.has("graph") && (base.get("graph").isString())) {
    graphName = JsonHelper::checkAndGetStringValue(base.json(), "graph");
    if (base.has("graphDefinition")) {
      _graphObj = plan->getAst()->query()->lookupGraphByName(graphName);

      if (_graphObj == nullptr) {
        THROW_ARANGO_EXCEPTION(TRI_ERROR_GRAPH_NOT_FOUND);
      }

      auto eColls = _graphObj->edgeCollections();
      for (auto const& n : eColls) {
        _edgeColls.push_back(n);
      }
    } else {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_BAD_JSON_PLAN,
                                     "missing graphDefinition.");
    }
  } else {
    _graphJson = base.get("graph").copy();
    if (!_graphJson.isArray()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_BAD_JSON_PLAN,
                                     "graph has to be an array.");
    }
    size_t edgeCollectionCount = _graphJson.size();
    // List of edge collection names
    for (size_t i = 0; i < edgeCollectionCount; ++i) {
      auto at = _graphJson.at(i);
      if (!at.isString()) {
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_QUERY_BAD_JSON_PLAN,
                                       "graph has to be an array of strings.");
      }
      _edgeColls.push_back(at.json()->_value._string.data);
    }
    if (_edgeColls.empty()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_QUERY_BAD_JSON_PLAN,
          "graph has to be a non empty array of strings.");
    }
  }

  // Out variables
  if (base.has("vertexOutVariable")) {
    _vertexOutVariable = varFromJson(plan->getAst(), base, "vertexOutVariable");
  }
  if (base.has("edgeOutVariable")) {
    _edgeOutVariable = varFromJson(plan->getAst(), base, "edgeOutVariable");
  }
  if (base.has("pathOutVariable")) {
    _pathOutVariable = varFromJson(plan->getAst(), base, "pathOutVariable");
  }

  // Flags
  if (base.has("traversalFlags")) {
    _options = TraversalOptions(base);
  }

  // TODO PARSE CONDITIONS
  
  _specializedNeighborsSearch = arangodb::basics::JsonHelper::getBooleanValue(base.json(), "specializedNeighborsSearch", false);
}

int TraversalNode::checkIsOutVariable(size_t variableId) const {
  if (_vertexOutVariable != nullptr && _vertexOutVariable->id == variableId) {
    return 0;
  }
  if (_edgeOutVariable != nullptr && _edgeOutVariable->id == variableId) {
    return 1;
  }
  if (_pathOutVariable != nullptr && _pathOutVariable->id == variableId) {
    return 2;
  }
  return -1;
}

/// @brief check if all directions are equal
bool TraversalNode::allDirectionsEqual() const {
  if (_directions.empty()) {
    // no directions!
    return false;
  }
  size_t const n = _directions.size();
  TRI_edge_direction_e const expected = _directions[0];

  for (size_t i = 1; i < n; ++i) {
    if (_directions[i] != expected) {
      return false;
    }
  }
  return true;
}

void TraversalNode::specializeToNeighborsSearch() {
  TRI_ASSERT(allDirectionsEqual());
  TRI_ASSERT(!_directions.empty());

  _specializedNeighborsSearch = true;
}

/// @brief toVelocyPack, for TraversalNode
void TraversalNode::toVelocyPackHelper(arangodb::velocypack::Builder& nodes,
                                       bool verbose) const {
  ExecutionNode::toVelocyPackHelperGeneric(nodes,
                                           verbose);  // call base class method

  nodes.add("database", VPackValue(_vocbase->_name));
  nodes.add("minDepth", VPackValue(_minDepth));
  nodes.add("maxDepth", VPackValue(_maxDepth));

  {
    // TODO Remove _graphJson
    auto tmp = arangodb::basics::JsonHelper::toVelocyPack(_graphJson.json());
    nodes.add("graph", tmp->slice());
  }
  nodes.add(VPackValue("directions"));
  {
    VPackArrayBuilder guard(&nodes);
    for (auto const& d : _directions) {
      nodes.add(VPackValue(d));
    }
  }

  // In variable
  if (usesInVariable()) {
    nodes.add(VPackValue("inVariable"));
    inVariable()->toVelocyPack(nodes);
  } else {
    nodes.add("vertexId", VPackValue(_vertexId));
  }

  if (_condition != nullptr) {
    nodes.add(VPackValue("condition"));
    _condition->toVelocyPack(nodes, verbose);
  }

  if (_graphObj != nullptr) {
    nodes.add(VPackValue("graphDefinition"));
    _graphObj->toVelocyPack(nodes, verbose);
  }

  // Out variables
  if (usesVertexOutVariable()) {
    nodes.add(VPackValue("vertexOutVariable"));
    vertexOutVariable()->toVelocyPack(nodes);
  }
  if (usesEdgeOutVariable()) {
    nodes.add(VPackValue("edgeOutVariable"));
    edgeOutVariable()->toVelocyPack(nodes);
  }
  if (usesPathOutVariable()) {
    nodes.add(VPackValue("pathOutVariable"));
    pathOutVariable()->toVelocyPack(nodes);
  }

  nodes.add(VPackValue("traversalFlags"));
  _options.toVelocyPack(nodes);

  // And close it:
  nodes.close();
}

/// @brief clone ExecutionNode recursively
ExecutionNode* TraversalNode::clone(ExecutionPlan* plan, bool withDependencies,
                                    bool withProperties) const {
  auto c =
      new TraversalNode(plan, _id, _vocbase, _edgeColls, _inVariable, _vertexId,
                        _directions, _minDepth, _maxDepth, _options);

  if (usesVertexOutVariable()) {
    auto vertexOutVariable = _vertexOutVariable;
    if (withProperties) {
      vertexOutVariable =
          plan->getAst()->variables()->createVariable(vertexOutVariable);
    }
    TRI_ASSERT(vertexOutVariable != nullptr);
    c->setVertexOutput(vertexOutVariable);
  }

  if (usesEdgeOutVariable()) {
    auto edgeOutVariable = _edgeOutVariable;
    if (withProperties) {
      edgeOutVariable =
          plan->getAst()->variables()->createVariable(edgeOutVariable);
    }
    TRI_ASSERT(edgeOutVariable != nullptr);
    c->setEdgeOutput(edgeOutVariable);
  }

  if (usesPathOutVariable()) {
    auto pathOutVariable = _pathOutVariable;
    if (withProperties) {
      pathOutVariable =
          plan->getAst()->variables()->createVariable(pathOutVariable);
    }
    TRI_ASSERT(pathOutVariable != nullptr);
    c->setPathOutput(pathOutVariable);
  }

  if (_specializedNeighborsSearch) {
    c->specializeToNeighborsSearch();
  }

  cloneHelper(c, plan, withDependencies, withProperties);

  return static_cast<ExecutionNode*>(c);
}

/// @brief the cost of a traversal node
double TraversalNode::estimateCost(size_t& nrItems) const {
  size_t incoming = 0;
  double depCost = _dependencies.at(0)->getCost(incoming);
  double expectedEdgesPerDepth = 0.0;
  auto trx = _plan->getAst()->query()->trx();
  auto collections = _plan->getAst()->query()->collections();

  TRI_ASSERT(collections != nullptr);

  for (auto const& it : _edgeColls) {
    auto collection = collections->get(it);

    if (collection == nullptr) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                     "unexpected pointer for collection");
    }

    TRI_ASSERT(collection != nullptr);

    auto indexes = trx->indexesForCollection(collection->name);
    for (auto const& index : indexes) {
      if (index->type() == arangodb::Index::IndexType::TRI_IDX_TYPE_EDGE_INDEX) {
        // We can only use Edge Index
        if (index->hasSelectivityEstimate()) {
          expectedEdgesPerDepth += 1 / index->selectivityEstimate();
        } else {
          expectedEdgesPerDepth += 1000;  // Hard-coded
        }
        break;
      }
    }
  }
  nrItems =
      static_cast<size_t>(incoming * std::pow(expectedEdgesPerDepth, static_cast<double>(_maxDepth)));
  if (nrItems == 0 && incoming > 0) {
    nrItems = 1;  // min value
  }
  return depCost + nrItems;
}

void TraversalNode::fillTraversalOptions(
    arangodb::traverser::TraverserOptions& opts) const {
  opts.minDepth = _minDepth;
  opts.maxDepth = _maxDepth;
  opts._tmpVar = _tmpObjVariable;

  // This is required by trx api.
  // But we do not use it here.
  SortCondition sort;

  size_t numEdgeColls = _edgeColls.size();
  AstNode* condition = nullptr;
  Transaction* trx = _ast->query()->trx();
  bool res = false;
  EdgeConditionBuilder globalEdgeConditionBuilder(this);

  opts._baseIndexHandles.reserve(numEdgeColls);
  opts._baseConditions.reserve(numEdgeColls);
  // Compute Edge Indexes. First default indexes:
  for (size_t i = 0; i < numEdgeColls; ++i) {
    auto dir = _directions[i];
    switch (dir) {
      case TRI_EDGE_IN:
        condition = globalEdgeConditionBuilder.getInboundCondition();
        break;
      case TRI_EDGE_OUT:
        condition = globalEdgeConditionBuilder.getOutboundCondition();
        break;
      case TRI_EDGE_ANY:
        condition = globalEdgeConditionBuilder.getInboundCondition();
        res = trx->getBestIndexHandleForFilterCondition(
            _edgeColls[i], condition, _tmpObjVariable, &sort, 1000,
            opts._baseIndexHandles);
        TRI_ASSERT(res);  // Right now we have an enforced edge index which wil
                          // always fit.
        opts._baseConditions.emplace_back(condition->clone(_ast));
        condition = globalEdgeConditionBuilder.getOutboundCondition();
        break;
    }
#warning hard-coded nrItems.
    res = trx->getBestIndexHandleForFilterCondition(
        _edgeColls[i], condition, _tmpObjVariable, &sort, 1000,
        opts._baseIndexHandles);
    TRI_ASSERT(res);  // We have an enforced edge index which wil always fit.
    opts._baseConditions.emplace_back(condition->clone(_ast));
  }

  for (std::pair<size_t, EdgeConditionBuilder> it : _edgeConditions) {
    auto ins = opts._depthIndexHandles.emplace(
        it.first, std::make_pair(std::vector<Transaction::IndexHandle>(),
                                 std::vector<AstNode*>()));
    TRI_ASSERT(ins.second);

    auto& idxList = ins.first->second.first;
    auto& condList = ins.first->second.second;
    idxList.reserve(numEdgeColls);
    condList.reserve(numEdgeColls);
    // Compute Edge Indexes. First default indexes:
    for (size_t i = 0; i < numEdgeColls; ++i) {
      auto dir = _directions[i];
      switch (dir) {
        case TRI_EDGE_IN:
          condition = it.second.getInboundCondition();
          break;
        case TRI_EDGE_OUT:
          condition = it.second.getOutboundCondition();
          break;
        case TRI_EDGE_ANY:
          condition = it.second.getInboundCondition();
          res = trx->getBestIndexHandleForFilterCondition(
              _edgeColls[i], condition, _tmpObjVariable, &sort, 1000, idxList);
          TRI_ASSERT(res);  // Right now we have an enforced edge index which wil
                            // always fit.
          condList.emplace_back(condition);
          condition = it.second.getOutboundCondition();
          break;
      }
#warning hard-coded nrItems.
      res = trx->getBestIndexHandleForFilterCondition(
          _edgeColls[i], condition, _tmpObjVariable, &sort, 1000, idxList);
      TRI_ASSERT(res);  // We have an enforced edge index which wil always fit.
      condList.emplace_back(condition);
    }
  }

  opts.useBreadthFirst = _options.useBreadthFirst;
  opts.uniqueVertices = _options.uniqueVertices;
  opts.uniqueEdges = _options.uniqueEdges;
}

/// @brief remember the condition to execute for early traversal abortion.
void TraversalNode::setCondition(arangodb::aql::Condition* condition) {
  std::unordered_set<Variable const*> varsUsedByCondition;

  Ast::getReferencedVariables(condition->root(), varsUsedByCondition);

  for (auto const& oneVar : varsUsedByCondition) {
    if ((_vertexOutVariable != nullptr &&
         oneVar->id != _vertexOutVariable->id) &&
        (_edgeOutVariable != nullptr && oneVar->id != _edgeOutVariable->id) &&
        (_pathOutVariable != nullptr && oneVar->id != _pathOutVariable->id) &&
        (_inVariable != nullptr && oneVar->id != _inVariable->id)) {
      _conditionVariables.emplace_back(oneVar);
    }
  }

  _condition = condition;
}

void TraversalNode::registerCondition(bool isConditionOnEdge,
                                      size_t conditionLevel,
                                      AstNode const* condition) {

  if (isConditionOnEdge) {
    auto const& it = _edgeConditions.find(conditionLevel);
    if (it == _edgeConditions.end()) {
      EdgeConditionBuilder builder(this);
      builder.addConditionPart(condition);
      _edgeConditions.emplace(conditionLevel, builder);
    } else {
      it->second.addConditionPart(condition);
    }
  } else {
    auto const& it = _vertexConditions.find(conditionLevel);
    if (it == _vertexConditions.end()) {
      auto cond = _ast->createNodeNaryOperator(NODE_TYPE_OPERATOR_NARY_AND);
      if (_globalVertexCondition != nullptr) {
        cond->addMember(_globalVertexCondition);
      }
      cond->addMember(condition);
      _vertexConditions.emplace(conditionLevel, cond);
    } else {
      it->second->addMember(condition);
    }
  }
}

void TraversalNode::registerGlobalCondition(bool isConditionOnEdge,
                                            AstNode const* condition) {
  std::cout << "Registering global condition for edges: " << isConditionOnEdge << std::endl;
  if (isConditionOnEdge) {
    _globalEdgeCondition = condition;
  } else {
    _globalVertexCondition = condition;
  }

  condition->dump(0);
}

AstNode* TraversalNode::getTemporaryRefNode() const {
  return _tmpObjVarNode;
}
