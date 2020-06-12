#pragma once
#include <memory>
#include <queue>
#include <string>
#include <unordered_set>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include "src/carnot/planner/compiler_state/compiler_state.h"
#include "src/carnot/planner/compiler_state/registry_info.h"
#include "src/carnot/planner/distributed/distributed_plan.h"
#include "src/carnot/planner/distributed/partial_op_mgr.h"
#include "src/carnot/planner/ir/ir_nodes.h"
#include "src/carnot/planner/ir/pattern_match.h"
#include "src/carnot/planner/rules/rule_executor.h"
#include "src/carnot/planner/rules/rules.h"

namespace pl {
namespace carnot {
namespace planner {
namespace distributed {

/**
 * @brief A plan that is split around blocking nodes.
 * before_blocking: plan should have no blocking nodes and should end with nodes that feed into
 * GRPCSinks. No blocking nodes means there also should not be MemorySinks.
 *
 * after_blocking: plan should have no memory sources, feed data in from GRPCSources and sink data
 * into MemorySinks.
 *
 */
struct BlockingSplitPlan {
  // The plan that occcurs before blocking nodes.
  std::unique_ptr<IR> before_blocking;
  // The plan that occcurs after and including blocking nodes.
  std::unique_ptr<IR> after_blocking;
  // The that has both the before and after blocking nodes.
  std::unique_ptr<IR> original_plan;
};

/**
 * @brief Two sets of nodes that correspond to the nodes of the original plan for those
 * that occur before blocking nodes and those that occur after.
 */
struct BlockingSplitNodeIDGroups {
  absl::flat_hash_set<int64_t> before_blocking_nodes;
  absl::flat_hash_set<int64_t> after_blocking_nodes;
};

/**
 * @brief The DistributedSplitter splits apart the graph along Blocking Node lines. The result is
 * two new IR graphs -> one that is run on Carnot instances that pull up data from Stirling and the
 * other that is run on Carnot instances which accumulate data and run blocking operations.
 */
class DistributedSplitter : public NotCopyable {
 public:
  /**
   * @brief Inserts a GRPCBridge in front of blocking operators in a graph.
   * Inserts a GRPCBridge (GRPCSink -> GRPCSourceGroup) between the parent_op
   * and blocking ops. The returned SplitPlan should contain two IRs now:
   * 1. Where all sources are MemorySources and all sinks are GRPCSinks
   * 2. Where all sources are GRPCSourceGroups and all sinks are MemorySinks
   *
   * Graphically, we want to be able to convert the following logical plan:
   * MemSrc1
   *  |   \
   *  |    Agg
   *  |   /
   * Join
   *  |
   * Sink
   *
   * Into
   * MemSrc1
   *  |
   * GRPCSink(1)
   *
   * GRPCSource(1)
   *  |   \
   *  |    Agg
   *  |   /
   * Join
   *  |
   * Sink
   *
   * Where GRPCSink and GRPCSource are a bridge.
   *
   * @param logical_plan: the input logical_plan
   * @return StatusOr<std::unique_ptr<BlockingSplitPLan>>: the plan split along blocking lines.
   */
  StatusOr<std::unique_ptr<BlockingSplitPlan>> SplitKelvinAndAgents(const IR* logical_plan);

  static StatusOr<std::unique_ptr<DistributedSplitter>> Create(bool support_partial_agg) {
    std::unique_ptr<DistributedSplitter> splitter =
        std::unique_ptr<DistributedSplitter>(new DistributedSplitter());
    PL_RETURN_IF_ERROR(splitter->Init(support_partial_agg));
    return splitter;
  }

 private:
  DistributedSplitter() {}
  Status Init(bool support_partial_agg) {
    if (support_partial_agg) {
      partial_operator_mgrs_.push_back(std::make_unique<AggOperatorMgr>());
    }
    partial_operator_mgrs_.push_back(std::make_unique<LimitOperatorMgr>());
    return Status::OK();
  }
  /**
   * @brief Returns the list of operator ids from the graph that occur before the blocking node and
   * after the blocking node.
   *
   * Note: this does not include non Operator IDs. IR::Keep() with either set of ids
   * will not produce a working graph.
   *
   * @param logical_plan
   * @param on_kelvin
   * @return BlockingSplitNodeIDGroups
   */
  BlockingSplitNodeIDGroups GetSplitGroups(const IR* logical_plan,
                                           const absl::flat_hash_map<int64_t, bool>& on_kelvin);

  absl::flat_hash_map<int64_t, bool> GetKelvinNodes(const std::vector<OperatorIR*>& sources);
  absl::flat_hash_map<OperatorIR*, std::vector<OperatorIR*>> GetEdgesToBreak(
      const IR* logical_plan, const absl::flat_hash_map<int64_t, bool>& on_kelvin,
      const std::vector<int64_t>& sources);

  bool ExecutesOnDataStores(const udfspb::UDTFSourceExecutor& executor);
  bool ExecutesOnRemoteProcessors(const udfspb::UDTFSourceExecutor& executor);
  bool RunsOnDataStores(const std::vector<OperatorIR*> sources);
  bool RunsOnRemoteProcessors(const std::vector<OperatorIR*> sources);
  bool IsSourceOnKelvin(OperatorIR* source_op);
  bool IsChildOpOnKelvin(bool is_parent_on_kelvin, OperatorIR* source_op);
  StatusOr<std::unique_ptr<IR>> CreateGRPCBridgePlan(
      const IR* logical_plan, const absl::flat_hash_map<int64_t, bool>& on_kelvin,
      const std::vector<int64_t>& sources);
  StatusOr<GRPCSinkIR*> CreateGRPCSink(OperatorIR* parent_op, int64_t grpc_id);
  StatusOr<GRPCSourceGroupIR*> CreateGRPCSourceGroup(OperatorIR* parent_op, int64_t grpc_id);
  Status InsertGRPCBridge(IR* plan, OperatorIR* parent, const std::vector<OperatorIR*>& parents);
  PartialOperatorMgr* GetPartialOperatorMgr(OperatorIR* op) const;

  /**
   * @brief Returns true if each child has a partial operator version. If each child does, then we
   * can convert them to their partial aggregate forms which will reduce the data sent over the
   * network.
   *
   * If it's the case that any of the children lack a partial implementation, that means the parent
   * operator has to send over all of it's data. For the children that do have a partial
   * implementation that means we now not only send over the original full data but we also send
   * over the partial results so our network usage has acutally increased rather than decreased.
   *
   * Instead we opt to just perform the original operation after the GRPCBridge, assuming that the
   * operation is much cheaper than the network costs.
   *
   * @param children
   * @return true
   * @return false
   */
  bool AllHavePartialMgr(std::vector<OperatorIR*> children) const;

  int64_t grpc_id_counter_ = 0;
  std::vector<std::unique_ptr<PartialOperatorMgr>> partial_operator_mgrs_;
};
}  // namespace distributed
}  // namespace planner
}  // namespace carnot
}  // namespace pl
