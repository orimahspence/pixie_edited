#include <gmock/gmock.h>
#include <google/protobuf/text_format.h>
#include <gtest/gtest.h>

#include <utility>
#include <vector>

#include <pypa/parser/parser.hh>

#include "src/carnot/planner/compiler/test_utils.h"
#include "src/carnot/planner/distributed/partial_op_mgr.h"
#include "src/carnot/udf_exporter/udf_exporter.h"

namespace pl {
namespace carnot {
namespace planner {
namespace distributed {
using ::testing::ElementsAre;
using ::testing::UnorderedElementsAre;
using ::testing::UnorderedElementsAreArray;

class PartialOpMgrTest : public OperatorTests {};

TEST_F(PartialOpMgrTest, limit_test) {
  auto mem_src = MakeMemSource(MakeRelation());
  auto limit = MakeLimit(mem_src, 10);
  MakeMemSink(limit, "out");

  LimitOperatorMgr mgr;
  EXPECT_TRUE(mgr.Matches(limit));
  auto prepare_limit_or_s = mgr.CreatePrepareOperator(graph.get(), limit);
  ASSERT_OK(prepare_limit_or_s);
  OperatorIR* prepare_limit_uncasted = prepare_limit_or_s.ConsumeValueOrDie();
  ASSERT_MATCH(prepare_limit_uncasted, Limit());
  LimitIR* prepare_limit = static_cast<LimitIR*>(prepare_limit_uncasted);
  EXPECT_EQ(prepare_limit->limit_value(), limit->limit_value());
  EXPECT_EQ(prepare_limit->parents(), limit->parents());
  EXPECT_NE(prepare_limit, limit);

  auto mem_src2 = MakeMemSource(MakeRelation());
  auto merge_limit_or_s = mgr.CreateMergeOperator(graph.get(), mem_src2, limit);
  ASSERT_OK(merge_limit_or_s);
  OperatorIR* merge_limit_uncasted = merge_limit_or_s.ConsumeValueOrDie();
  ASSERT_MATCH(merge_limit_uncasted, Limit());
  LimitIR* merge_limit = static_cast<LimitIR*>(merge_limit_uncasted);
  EXPECT_EQ(merge_limit->limit_value(), limit->limit_value());
  EXPECT_EQ(merge_limit->parents()[0], mem_src2);
  EXPECT_NE(merge_limit, limit);
}

TEST_F(PartialOpMgrTest, agg_test) {
  auto mem_src = MakeMemSource(MakeRelation());
  auto agg = MakeBlockingAgg(mem_src, {MakeColumn("count", 0)},
                             {{"mean", MakeMeanFunc(MakeColumn("count", 0))}});
  MakeMemSink(agg, "out");

  // Pre-checks to make sure things work.
  EXPECT_MATCH(agg, FullAgg());
  EXPECT_NOT_MATCH(agg, FinalizeAgg());
  EXPECT_NOT_MATCH(agg, PartialAgg());

  AggOperatorMgr mgr;
  EXPECT_TRUE(mgr.Matches(agg));
  auto prepare_agg_or_s = mgr.CreatePrepareOperator(graph.get(), agg);
  ASSERT_OK(prepare_agg_or_s);

  OperatorIR* prepare_agg_uncasted = prepare_agg_or_s.ConsumeValueOrDie();
  ASSERT_MATCH(prepare_agg_uncasted, PartialAgg());
  BlockingAggIR* prepare_agg = static_cast<BlockingAggIR*>(prepare_agg_uncasted);

  auto mem_src2 = MakeMemSource(MakeRelation());
  auto merge_agg_or_s = mgr.CreateMergeOperator(graph.get(), mem_src2, agg);
  ASSERT_OK(merge_agg_or_s);
  OperatorIR* merge_agg_uncasted = merge_agg_or_s.ConsumeValueOrDie();
  ASSERT_MATCH(merge_agg_uncasted, FinalizeAgg());
  BlockingAggIR* merge_agg = static_cast<BlockingAggIR*>(merge_agg_uncasted);

  ASSERT_EQ(prepare_agg->aggregate_expressions().size(), merge_agg->aggregate_expressions().size());
  for (int64_t i = 0; i < static_cast<int64_t>(prepare_agg->aggregate_expressions().size()); ++i) {
    auto prep_expr = prepare_agg->aggregate_expressions()[i];
    auto merge_expr = merge_agg->aggregate_expressions()[i];
    EXPECT_EQ(prep_expr.name, merge_expr.name);
    EXPECT_TRUE(prep_expr.node->Equals(merge_expr.node))
        << absl::Substitute("prep expr $0 merge expr $1", prep_expr.node->DebugString(),
                            merge_expr.node->DebugString());
  }
}
}  // namespace distributed
}  // namespace planner
}  // namespace carnot
}  // namespace pl
