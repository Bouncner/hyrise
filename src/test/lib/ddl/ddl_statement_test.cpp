#include <memory>
#include <string>
#include <utility>

#include "base_test.hpp"
#include "tasks/chunk_compression_task.hpp"

#include "SQLParser.h"
#include "SQLParserResult.h"

#include "types.hpp"
#include "hyrise.hpp"
#include "sql/sql_pipeline.hpp"
#include "sql/sql_pipeline_builder.hpp"
#include "sql/sql_plan_cache.hpp"

namespace opossum {

class DDLStatementTest : public BaseTest {
 protected:
  void SetUp() override {
    Hyrise::reset();

    // We reload table_a every time since it is modified during the test case.
    _table_a = load_table("resources/test_data/tbl/int_float_create_index_test.tbl", 2);
    ChunkEncoder::encode_all_chunks(_table_a);
    Hyrise::get().storage_manager.add_table("table_a", _table_a);
  }

  // Tables modified during test case
  std::shared_ptr<Table> _table_a;

  const std::string _create_index_single_column = "CREATE INDEX myindex ON table_a (a)";
  const std::string _create_index_multi_column = "CREATE INDEX myindex ON table_a (a, b)";
};

void check_if_index_exists_correctly(std::shared_ptr<std::vector<ColumnID>> column_ids, std::shared_ptr<Table> table) {
  auto chunk_count = table->chunk_count();
  for(ChunkID id=ChunkID{0}; id < chunk_count; id+=1) {
    auto current_chunk = table->get_chunk(id);
    auto actual_indices = current_chunk->get_indexes(*column_ids);
    EXPECT_TRUE(actual_indices.size() == 1);
  }
}

TEST_F(DDLStatementTest, CreateIndexSingleColumn) {
  auto sql_pipeline = SQLPipelineBuilder{_create_index_single_column}.create_pipeline();

  const auto& [pipeline_status, table] = sql_pipeline.get_result_table();
  EXPECT_EQ(pipeline_status, SQLPipelineStatus::Success);

  auto column_ids = std::make_shared<std::vector<ColumnID>>();
  column_ids->emplace_back(ColumnID{0});

  auto actual_index = _table_a->indexes_statistics().at(0);

  EXPECT_TRUE(actual_index.name == "myindex");
  EXPECT_TRUE(actual_index.column_ids == *column_ids);

  check_if_index_exists_correctly(column_ids, _table_a);
}

TEST_F(DDLStatementTest, CreateIndexMultiColumn) {
  auto sql_pipeline = SQLPipelineBuilder{_create_index_multi_column}.create_pipeline();

  const auto& [pipeline_status, table] = sql_pipeline.get_result_table();
  EXPECT_EQ(pipeline_status, SQLPipelineStatus::Success);

  auto column_ids = std::make_shared<std::vector<ColumnID>>();
  column_ids->emplace_back(ColumnID{0});
  column_ids->emplace_back(ColumnID{1});

  auto actual_index = _table_a->indexes_statistics().at(0);

  EXPECT_TRUE(actual_index.name == "myindex");
  EXPECT_TRUE(actual_index.column_ids == *column_ids);

  check_if_index_exists_correctly(column_ids, _table_a);
}

TEST_F(DDLStatementTest, CreateIndexWithoutName) {
  auto sql_pipeline = SQLPipelineBuilder{"CREATE INDEX ON table_a (a)"}.create_pipeline();

  const auto& [pipeline_status, table] = sql_pipeline.get_result_table();
  EXPECT_EQ(pipeline_status, SQLPipelineStatus::Success);

  auto column_ids = std::make_shared<std::vector<ColumnID>>();
  column_ids->emplace_back(ColumnID{0});

  auto actual_index = _table_a->indexes_statistics().at(0);

  EXPECT_TRUE(actual_index.name == "table_a_a");
  EXPECT_TRUE(actual_index.column_ids == *column_ids);

  check_if_index_exists_correctly(column_ids, _table_a);
}

TEST_F(DDLStatementTest, CreateIndexIfNotExistsFirstTime) {
  auto sql_pipeline = SQLPipelineBuilder{"CREATE INDEX IF NOT EXISTS myindex ON table_a (a)"}.create_pipeline();

  const auto& [pipeline_status, table] = sql_pipeline.get_result_table();
  EXPECT_EQ(pipeline_status, SQLPipelineStatus::Success);

  auto column_ids = std::make_shared<std::vector<ColumnID>>();
  column_ids->emplace_back(ColumnID{0});

  auto actual_index = _table_a->indexes_statistics().at(0);

  EXPECT_TRUE(actual_index.name == "myindex");
  EXPECT_TRUE(actual_index.column_ids == *column_ids);

  check_if_index_exists_correctly(column_ids, _table_a);
}

TEST_F(DDLStatementTest, CreateIndexIfNotExistsSecondTime) {
  auto sql_pipeline = SQLPipelineBuilder{_create_index_single_column}.create_pipeline();

  const auto& [pipeline_status, table] = sql_pipeline.get_result_table();
  EXPECT_EQ(pipeline_status, SQLPipelineStatus::Success);

  auto second_sql_pipeline = SQLPipelineBuilder{"CREATE INDEX IF NOT EXISTS myindex ON table_a (a, b)"}.create_pipeline();

  const auto& [second_pipeline_status, second_table] = second_sql_pipeline.get_result_table();
  EXPECT_EQ(second_pipeline_status, SQLPipelineStatus::Success);

  auto single_column_col_ids = std::make_shared<std::vector<ColumnID>>();
  single_column_col_ids->emplace_back(ColumnID{0});

  check_if_index_exists_correctly(single_column_col_ids, _table_a);
}

TEST_F(DDLStatementTest, CreateIndexIfNotExistsWithoutName) {
  auto sql_pipeline = SQLPipelineBuilder{"CREATE INDEX IF NOT EXISTS ON table_a (a, b)"}.create_pipeline();

  EXPECT_THROW(sql_pipeline.get_result_table(), std::exception);
}

}