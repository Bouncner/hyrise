#include <memory>
#include "base_test.hpp"
#include "utils/assert.hpp"
#include "concurrency/transaction_context.hpp"
#include "hyrise.hpp"
#include "operators/maintenance/create_index.hpp"
#include "storage/table.hpp"

namespace opossum {

using namespace opossum::expression_functional;  // NOLINT

class CreateIndexTest : public BaseTest {
 public:
  void SetUp() override {
    test_table = load_table("resources/test_data/tbl/string_int_index.tbl", 3);
    Hyrise::get().storage_manager.add_table(table_name, test_table);
    column_ids->emplace_back(test_table->column_id_by_name("b"));
    create_index = std::make_shared<CreateIndex>(index_name, true, table_name, column_ids);
  }

  std::shared_ptr<Table> test_table;
  std::shared_ptr<CreateIndex> create_index;
  std::string index_name = "TestIndex";
  std::shared_ptr<std::vector<ColumnID>> column_ids = std::make_shared<std::vector<ColumnID>>();
  SegmentIndexType index_type;
  std::string table_name = "TestTable";
};

void check_index_exists_correctly(std::shared_ptr<CreateIndex> created_index, std::shared_ptr<Table> table) {
  auto chunk_count = table->chunk_count();
  for(ChunkID id=ChunkID{0}; id < chunk_count; id+=1) {
    auto current_chunk = table->get_chunk(id);
    auto applied_indices = current_chunk->get_indexes(*created_index->column_ids);
    EXPECT_TRUE(applied_indices.size() == 1);
  }
}

TEST_F(CreateIndexTest, NameAndDescription) { EXPECT_EQ(create_index->name(), "CreateIndex"); }

TEST_F(CreateIndexTest, Execute) {
  ChunkEncoder::encode_all_chunks(test_table);

  const auto context = Hyrise::get().transaction_manager.new_transaction_context(AutoCommit::No);
  create_index->set_transaction_context(context);

  create_index->execute();
  context->commit();


  auto actual_index = test_table->indexes_statistics().at(0);

  EXPECT_EQ(actual_index.name, index_name);
  EXPECT_EQ(actual_index.column_ids, *column_ids);

  check_index_exists_correctly(create_index, test_table);
}

TEST_F(CreateIndexTest, TableExists) {
  // TODO: is this test really doing what we think it does?
  create_index = std::make_shared<CreateIndex>(index_name, true, table_name, column_ids);

  const auto context = Hyrise::get().transaction_manager.new_transaction_context(AutoCommit::No);
  create_index->set_transaction_context(context);

  EXPECT_THROW(create_index->execute(), std::logic_error);
  context->rollback(RollbackReason::Conflict);
}

TEST_F(CreateIndexTest, ExecuteWithIfNotExists) {
  ChunkEncoder::encode_all_chunks(test_table);

  const auto context = Hyrise::get().transaction_manager.new_transaction_context(AutoCommit::No);
  create_index->set_transaction_context(context);

  create_index->execute();
  context->commit();

  // let name stay the same, but alter column ids
  column_ids->emplace_back(test_table->column_id_by_name("a"));
  auto another_index = std::make_shared<CreateIndex>(index_name, true, table_name, column_ids);
  const auto another_context = Hyrise::get().transaction_manager.new_transaction_context(AutoCommit::No);

  another_index->set_transaction_context(another_context);

  // make sure that initially created index still exists
  check_index_exists_correctly(create_index, test_table);
}

TEST_F(CreateIndexTest, ExecuteMultipleColumns) {
  ChunkEncoder::encode_all_chunks(test_table);

  // overwrite and extend setup method to enable multiple columns
  column_ids->emplace_back(test_table->column_id_by_name("a"));
  create_index = std::make_shared<CreateIndex>(index_name, true, table_name, column_ids);

  const auto context = Hyrise::get().transaction_manager.new_transaction_context(AutoCommit::No);
  create_index->set_transaction_context(context);

  create_index->execute();
  context->commit();

  auto actual_index = test_table->indexes_statistics().at(0);

  EXPECT_EQ(actual_index.name, index_name);
  EXPECT_EQ(actual_index.column_ids, *column_ids);

  check_index_exists_correctly(create_index, test_table);
}

TEST_F(CreateIndexTest, ExecuteWithIfNotExistsWithoutName) {}
  // TODO implement test case
}  // namespace opossum
