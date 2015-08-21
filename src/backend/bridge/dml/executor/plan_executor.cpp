//===----------------------------------------------------------------------===//
//
//                         PelotonDB
//
// plan_executor.cpp
//
// Identification: src/backend/bridge/dml/executor/plan_executor.cpp
//
// Copyright (c) 2015, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "plan_executor.h"
#include <cassert>

#include "backend/bridge/dml/mapper/mapper.h"
#include "backend/bridge/dml/tuple/tuple_transformer.h"
#include "backend/common/logger.h"
#include "backend/concurrency/transaction_manager.h"
#include "backend/executor/executors.h"
#include "backend/storage/tuple_iterator.h"

#include "access/tupdesc.h"
#include "nodes/print.h"
#include "utils/memutils.h"

namespace peloton {
namespace bridge {

executor::AbstractExecutor *BuildExecutorTree(executor::AbstractExecutor *root,
                                              planner::AbstractPlan *plan,
                                              ParamListInfo param_list,
                                              concurrency::Transaction *txn);

void CleanExecutorTree(executor::AbstractExecutor *root);

/**
 * @brief Pretty print the plan tree.
 * @param The plan tree
 * @return none.
 */
void PlanExecutor::PrintPlan(const planner::AbstractPlan *plan,
                             std::string prefix) {
  if (plan == nullptr)
    return;

  prefix += "  ";

  LOG_TRACE("%s->Plan Type :: %d ", prefix.c_str(), plan->GetPlanNodeType());

  auto children = plan->GetChildren();

  for (auto child : children) {
    PrintPlan(child, prefix);
  }
}

/**
 * @brief Build the executor tree.
 * @param The current executor tree
 * @param The plan tree
 * @param Transation context
 * @return The updated executor tree.
 */
executor::AbstractExecutor *BuildExecutorTree(executor::AbstractExecutor *root,
                                              planner::AbstractPlan *plan,
                                              ParamListInfo param_list,
                                              concurrency::Transaction *txn) {
  // Base case
  if (plan == nullptr)
    return root;

  executor::AbstractExecutor *child_executor = nullptr;

  ValueArray params = PlanTransformer::BuildParams(param_list);

  executor::ExecutorContext *executor_context = new executor::ExecutorContext(
      txn, params);

  auto plan_node_type = plan->GetPlanNodeType();
  switch (plan_node_type) {
    case PLAN_NODE_TYPE_INVALID:
      LOG_ERROR("Invalid plan node type ")
      ;
      break;

    case PLAN_NODE_TYPE_SEQSCAN:
      child_executor = new executor::SeqScanExecutor(plan, executor_context);
      break;

    case PLAN_NODE_TYPE_INDEXSCAN:
      child_executor = new executor::IndexScanExecutor(plan, executor_context);
      break;

    case PLAN_NODE_TYPE_INSERT:
      child_executor = new executor::InsertExecutor(plan, executor_context);
      break;

    case PLAN_NODE_TYPE_DELETE:
      child_executor = new executor::DeleteExecutor(plan, executor_context);
      break;

    case PLAN_NODE_TYPE_UPDATE:
      child_executor = new executor::UpdateExecutor(plan, executor_context);
      break;

    case PLAN_NODE_TYPE_LIMIT:
      child_executor = new executor::LimitExecutor(plan, executor_context);
      break;

    case PLAN_NODE_TYPE_NESTLOOP:
      child_executor = new executor::NestedLoopJoinExecutor(plan,
                                                            executor_context);
      break;

    case PLAN_NODE_TYPE_MERGEJOIN:
      child_executor = new executor::MergeJoinExecutor(plan, executor_context);
      break;

    case PLAN_NODE_TYPE_PROJECTION:
      child_executor = new executor::ProjectionExecutor(plan, executor_context);
      break;

    case PLAN_NODE_TYPE_MATERIALIZE:
      child_executor = new executor::MaterializationExecutor(plan,
                                                             executor_context);
      break;

    case PLAN_NODE_TYPE_AGGREGATE_V2:
      child_executor = new executor::AggregateExecutor(plan, executor_context);
      break;

    case PLAN_NODE_TYPE_ORDERBY:
      child_executor = new executor::OrderByExecutor(plan, executor_context);
      break;

    default:
      LOG_ERROR("Unsupported plan node type : %d ", plan_node_type)
      ;
      break;
  }

  // Base case
  if (child_executor != nullptr) {
    if (root != nullptr)
      root->AddChild(child_executor);
    else
      root = child_executor;
  }

  // Recurse
  auto children = plan->GetChildren();
  for (auto child : children) {
    child_executor = BuildExecutorTree(child_executor, child, param_list, txn);
  }

  return root;
}

/**
 * @brief Clean up the executor tree.
 * @param The current executor tree
 * @return none.
 */
void CleanExecutorTree(executor::AbstractExecutor *root) {
  if (root == nullptr)
    return;

  // Recurse
  auto children = root->GetChildren();
  for (auto child : children) {
    CleanExecutorTree(child);
  }

  // Cleanup self
  delete root;
}

/**
 * @brief Add a Materialization node if the root of the executor tree is seqscan
 * or limit
 * @param the current executor tree
 * @return new root of the executor tree
 */
executor::AbstractExecutor *PlanExecutor::AddMaterialization(
    executor::AbstractExecutor *root) {
  if (root == nullptr)
    return root;
  auto type = root->GetRawNode()->GetPlanNodeType();
  executor::AbstractExecutor *new_root = root;

  switch (type) {
    case PLAN_NODE_TYPE_MERGEJOIN:
    case PLAN_NODE_TYPE_NESTLOOP:
    case PLAN_NODE_TYPE_SEQSCAN:
    case PLAN_NODE_TYPE_INDEXSCAN:
      /* FALL THRU */
    case PLAN_NODE_TYPE_LIMIT:
      new_root = new executor::MaterializationExecutor(nullptr, nullptr);
      new_root->AddChild(root);
      LOG_TRACE("Added materialization, the original root executor type is %d",
                type);
      break;
    default:
      break;
  }

  return new_root;
}

/**
 * @brief Build a executor tree and execute it.
 * @return status of execution.
 */
void PlanExecutor::ExecutePlan(planner::AbstractPlan *plan,
                               ParamListInfo param_list, TupleDesc tuple_desc,
                               Peloton_Status *pstatus, TransactionId txn_id) {
  if (plan == nullptr)
    return;

  LOG_TRACE("PlanExecutor Start \n");

  bool status;
  bool init_failure = false;
  bool single_statement_txn = false;
  MemoryContext oldContext;
  List *slots = NULL;

  auto &txn_manager = concurrency::TransactionManager::GetInstance();
  auto txn = txn_manager.GetPGTransaction(txn_id);
  // This happens for single statement queries in PG
  if (txn == nullptr) {
    single_statement_txn = true;
    txn = txn_manager.StartPGTransaction(txn_id);
  }
  assert(txn);

  LOG_TRACE("Txn ID = %lu ", txn->GetTransactionId());
  LOG_TRACE("Building the executor tree");

  // Build the executor tree
  executor::AbstractExecutor *executor_tree = BuildExecutorTree(nullptr, plan,
                                                                param_list,
                                                                txn);

  // Add materialization if the root if seqscan or limit
  executor_tree = AddMaterialization(executor_tree);

  LOG_TRACE("Initializing the executor tree");

  // Initialize the executor tree
  status = executor_tree->Init();

  // Abort and cleanup
  if (status == false) {
    init_failure = true;
    txn->SetResult(Result::RESULT_FAILURE);
    goto cleanup;
  }

  LOG_TRACE("Running the executor tree");

  // Execute the tree until we get result tiles from root node
  for (;;) {
    status = executor_tree->Execute();

    // Stop
    if (status == false) {
      break;
    }

    std::unique_ptr<executor::LogicalTile> tile(executor_tree->GetOutput());

    // Some executor just doesn't return tiles (e.g., Update).
    if (tile.get() == nullptr) {
      continue;
    }

    // Get result base tile and iterate over it
    auto base_tile = tile.get()->GetBaseTile(0);
    assert(base_tile);
    storage::TupleIterator tile_itr(base_tile);
    storage::Tuple tuple(base_tile->GetSchema());

    // Switch to query context to construct list and slots
    oldContext = MemoryContextSwitchTo(SHMQueryContext);

    // Go over tile and get result slots
    while (tile_itr.Next(tuple)) {
      auto slot = TupleTransformer::GetPostgresTuple(&tuple, tuple_desc);

      if (slot != nullptr) {
        slots = lappend(slots, slot);
        //print_slot(slot);
      }
    }

    // Go back to previous context
    MemoryContextSwitchTo(oldContext);
  }

  // Set the result
  pstatus->m_result_slots = slots;

// final cleanup
  cleanup:

  // should we commit or abort ?
  if (single_statement_txn == true || init_failure == true) {
    auto status = txn->GetResult();
    switch (status) {
      case Result::RESULT_SUCCESS:
        LOG_TRACE("Committing txn_id : %lu , cid : %lu\n",
                  txn->GetTransactionId(), txn->GetCommitId());
        // Commit
        txn_manager.CommitTransaction(txn);

        break;

      case Result::RESULT_FAILURE:
      default:
        LOG_TRACE("Aborting txn : %lu , cid : %lu \n", txn->GetTransactionId(),
                  txn->GetCommitId());
        // Abort
        txn_manager.AbortTransaction(txn);
    }
  }

  // clean up
  CleanExecutorTree(executor_tree);
  pstatus->m_result = txn->GetResult();
  return;
}

}  // namespace bridge
}  // namespace peloton