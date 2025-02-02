/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "velox/exec/PlanNodeStats.h"
#include "velox/exec/tests/utils/AssertQueryBuilder.h"
#include "velox/exec/tests/utils/PlanBuilder.h"
#include "velox/functions/lib/aggregates/tests/AggregationTestBase.h"

using namespace facebook::velox::exec::test;
using namespace facebook::velox::functions::aggregate::test;

namespace facebook::velox::aggregate::test {

namespace {

class CountAggregationTest : public AggregationTestBase {
 protected:
  void SetUp() override {
    AggregationTestBase::SetUp();
    allowInputShuffle();
  }

  RowTypePtr rowType_{
      ROW({"c0", "c1", "c2", "c3", "c4", "c5", "c6", "c7"},
          {BIGINT(),
           SMALLINT(),
           INTEGER(),
           BIGINT(),
           REAL(),
           DOUBLE(),
           VARCHAR(),
           TINYINT()})};
};

TEST_F(CountAggregationTest, count) {
  auto vectors = makeVectors(rowType_, 10, 100);
  createDuckDbTable(vectors);

  testAggregations(vectors, {}, {"count()"}, "SELECT count(1) FROM tmp");

  testAggregations(vectors, {}, {"count(1)"}, "SELECT count(1) FROM tmp");

  // count over column with nulls; only non-null rows should be counted
  testAggregations(vectors, {}, {"count(c1)"}, "SELECT count(c1) FROM tmp");

  // count over zero rows; the result should be 0, not null
  testAggregations(
      [&](PlanBuilder& builder) {
        builder.values(vectors).filter("c0 % 3 > 5");
      },
      {},
      {"count(c1)"},
      "SELECT count(c1) FROM tmp WHERE c0 % 3 > 5");

  testAggregations(
      [&](PlanBuilder& builder) {
        builder.values(vectors).project({"c0 % 10 AS c0_mod_10", "c1"});
      },
      {"c0_mod_10"},
      {"count(1)"},
      "SELECT c0 % 10, count(1) FROM tmp GROUP BY 1");

  testAggregations(
      [&](PlanBuilder& builder) {
        builder.values(vectors).project({"c0 % 10 AS c0_mod_10", "c7"});
      },
      {"c0_mod_10"},
      {"count(c7)"},
      "SELECT c0 % 10, count(c7) FROM tmp GROUP BY 1");
}

TEST_F(CountAggregationTest, mask) {
  std::vector<RowVectorPtr> data;
  // Make batches where some batches have mask all true, some half and half and
  // some all false. All batches repeat the same grouping keys. The data to
  // count is null for 1/3 of the rows.
  constexpr int32_t kNumBatches = 10;
  constexpr int32_t kRowsInBatch = 100;
  for (auto counter = 0; counter < kNumBatches; ++counter) {
    data.push_back(makeRowVector(
        {"k", "c", "m"},
        {makeFlatVector<int64_t>(
             kRowsInBatch, [](auto row) { return row / 10; }),
         makeFlatVector<int64_t>(
             kRowsInBatch,
             [](auto row) { return row; },
             [](auto row) { return row % 3 == 0; }),
         makeFlatVector<bool>(kRowsInBatch, [&](auto row) {
           return counter % 3 == 0 ? false
               : counter % 3 == 1  ? false
                                   : row % 2 == 0;
         })}));
  }

  createDuckDbTable(data);

  // count(c)
  auto plan = PlanBuilder()
                  .values(data)
                  .singleAggregation({}, {"count(c)"}, {"m"})
                  .planNode();
  assertQuery(plan, "SELECT count(c) FILTER (where m) FROM tmp");

  plan = PlanBuilder()
             .values(data)
             .partialAggregation({}, {"count(c)"}, {"m"})
             .finalAggregation()
             .planNode();
  assertQuery(plan, "SELECT count(c) FILTER (where m) FROM tmp");

  // count(1)
  plan = PlanBuilder()
             .values(data)
             .singleAggregation({}, {"count()"}, {"m"})
             .planNode();
  assertQuery(plan, "SELECT count(1) FILTER (where m) FROM tmp");

  plan = PlanBuilder()
             .values(data)
             .partialAggregation({}, {"count()"}, {"m"})
             .finalAggregation()
             .planNode();
  assertQuery(plan, "SELECT count(1) FILTER (where m) FROM tmp");

  core::PlanNodeId partialNodeId;
  plan = PlanBuilder()
             .values(data)
             .partialAggregation({"k"}, {"count(c)"}, {"m"})
             .capturePlanNodeId(partialNodeId)
             .finalAggregation()
             .planNode();
  auto task =
      AssertQueryBuilder(plan, duckDbQueryRunner_)
          .maxDrivers(1)
          .config(core::QueryConfig::kAbandonPartialAggregationMinRows, "1")
          .config(core::QueryConfig::kAbandonPartialAggregationMinPct, "0")
          .assertResults(
              "SELECT k, count(c) FILTER (where m) FROM tmp GROUP BY k");
  auto taskStats = toPlanStats(task->taskStats());
  auto partialStats = taskStats.at(partialNodeId).customStats;
  EXPECT_LT(0, partialStats.at("abandonedPartialAggregation").count);
}

} // namespace
} // namespace facebook::velox::aggregate::test
