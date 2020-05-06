#include <iostream>
#include <fstream>

#include "expression/expression_functional.hpp"
#include "hyrise.hpp"
#include "operators/print.hpp"
#include "operators/sort.hpp"
#include "operators/table_wrapper.hpp"
#include "optimizer/optimizer.hpp"
#include "optimizer/strategy/chunk_pruning_rule.hpp"
#include "optimizer/strategy/column_pruning_rule.hpp"
#include "logical_query_plan/logical_plan_root_node.hpp"
#include "logical_query_plan/lqp_translator.hpp"
#include "logical_query_plan/predicate_node.hpp"
#include "logical_query_plan/stored_table_node.hpp"
#include "sql/sql_pipeline_builder.hpp"
#include "sql/sql_pipeline_statement.hpp"
#include "storage/chunk_encoder.hpp"
#include "storage/index/group_key/group_key_index.hpp"
#include "utils/load_table.hpp"
#include "utils/format_duration.hpp"
#include "utils/timer.hpp"

using namespace opossum::expression_functional;  // NOLINT

using namespace opossum;  // NOLINT

// Import

constexpr auto TBL_FILE = "../../data/10mio_pings_no_id_int.tbl";
constexpr auto WORKLOAD_FILE = "../../data/workload.csv";
constexpr auto CONFIG_PATH = "../../data/config";
constexpr auto CHUNK_SIZE = size_t{10};
//constexpr auto CHUNK_SIZE = size_t{100'000};
constexpr auto TABLE_NAME = "PING";
constexpr auto ORDER_BY_MODE = OrderByMode::Ascending;

//Chunk encodings copied from ping data micro benchmark 
const auto CHUNK_ENCODINGS = std::vector{
  SegmentEncodingSpec{EncodingType::Dictionary},
  SegmentEncodingSpec{EncodingType::Unencoded},
  SegmentEncodingSpec{EncodingType::LZ4},
  SegmentEncodingSpec{EncodingType::RunLength},
  SegmentEncodingSpec{EncodingType::FrameOfReference, VectorCompressionType::SimdBp128}
};

// Export 

constexpr auto MEMORY_CONSUMPTION_FILE = "../../out/config_results/memory_consumption.csv";
constexpr auto PERFORMANCE_FILE = "../../out/config_results/performance.csv";

// returns a vector with all lines of the file
std::vector<std::vector<std::string>> read_file(const std::string file) {
  std::ifstream f(file);
  std::string line;
  std::vector<std::vector<std::string>> file_values;

  std::string header;
  std::getline(f, header);

  while (std::getline(f, line)){
    std::vector<std::string> line_values;
    std::istringstream linestream(line);
    std::string value;

    while (std::getline(linestream, value, ',')){
     line_values.push_back(value);
    }

    file_values.push_back(line_values);
  }

  return file_values;  
} 

// returns all segmenst of a chunk 
Segments get_segments_of_chunk(const std::shared_ptr<const Table>& input_table, ChunkID chunk_id){
  Segments segments{};
  for (auto column_id = ColumnID{0}; column_id < input_table->column_count(); ++column_id) {
    segments.emplace_back(input_table->get_chunk(chunk_id)->get_segment(column_id));
  }
  return segments;
} 

/**
 * This function takes a file path to a CSV file containing the workload and returns the queries in form of LQP-based
 * queries together with their frequency. The reason to use LQPs is that we can later selectively apply optimizer rules
 * (most importantly the chunk pruning rule).
 * Alternatives are SQL-based queries, which either do not use the optimizer (no chunk pruning) or the complete
 * optimizer (we lose control over the predicate order), or creating PQPs (we would need to manually prune chunks).
 */
std::vector<std::pair<std::shared_ptr<AbstractLQPNode>, size_t>> load_queries_from_csv(const std::string workload_file) {
  std::vector<std::pair<std::shared_ptr<AbstractLQPNode>, size_t>> output_queries;

  const auto csv_lines = read_file(workload_file);

  auto previous_query_id = int64_t{-1};
  auto previous_predicate_selectivity = 17.0;
  std::shared_ptr<AbstractLQPNode> current_node;
  
  const auto& table = Hyrise::get().storage_manager.get_table(TABLE_NAME);
  const auto column_names = table->column_names();
  auto stored_table_node = StoredTableNode::make(TABLE_NAME);
  std::shared_ptr<AbstractLQPNode> previous_node = stored_table_node;
  auto query_frequency = size_t{0};
  for (const auto& csv_line : csv_lines) {
    const auto query_id = std::stol(csv_line[0]);
    const auto predicate_str = csv_line[1];
    const auto column_id = ColumnID{static_cast<uint16_t>(std::stoi(csv_line[2]))};
    const auto predicate_selectivity = stof(csv_line[3]);
    query_frequency = stoul(csv_line[5]);
    const auto search_value_0 = stoi(csv_line[6]);
    const auto search_value_1 = stoi(csv_line[7]);

    Assert(query_id >= previous_query_id,
           "Queries are expected to be sorted ascendingly by: query ID ascendingly.");
    Assert(query_id != previous_query_id || predicate_selectivity >= previous_predicate_selectivity,
           "Queries are expected to be sorted ascendingly by: query ID ascendingly & selectivity descendingly.");

    if (query_id > 0 && query_id != previous_query_id) {
      output_queries.emplace_back(current_node, query_frequency);
      stored_table_node = StoredTableNode::make(TABLE_NAME);
      previous_node = stored_table_node;
    }

    const auto lqp_column = stored_table_node->get_column(column_names[column_id]);
    if (predicate_str == "Between") {
      Assert(search_value_1 > -1, "Between predicate with missing second search value.");
      current_node = PredicateNode::make(between_inclusive_(lqp_column, search_value_0, search_value_1), previous_node);
    } else if (predicate_str == "LessThanEquals") {
      current_node = PredicateNode::make(less_than_equals_(lqp_column, search_value_0), previous_node);
    }

    previous_query_id = query_id;
    previous_predicate_selectivity = predicate_selectivity;
    previous_node = current_node;
  }
  output_queries.emplace_back(current_node, query_frequency);  // Store last query

  return output_queries;
}

/**
 * Takes a pair of an LQP-based query and the frequency, partially optimizes the query (only chunk and column pruning
 * for now), translates the query, and executes the query (single-threaded).
 */
float partially_optimize_translate_and_execute_query(const std::pair<std::shared_ptr<AbstractLQPNode>, size_t>& workoad_item) {
  constexpr auto EXECUTION_COUNT = 17;
  const auto lqp_query = workoad_item.first;
  //const auto frequency = workoad_item.second;

  // Run chunk and column pruning rules. Kept it quite simple for now. Take a look at Optimizer::optimize() in case
  // problems occur. The following code is taken from optimizer.cpp. In case the new root is confusing to you, take a
  // look there.
  const auto root_node = LogicalPlanRootNode::make(std::move(lqp_query));

  const auto chunk_pruning_rule = ChunkPruningRule();
  chunk_pruning_rule.apply_to(root_node);

  const auto column_pruning_rule = ColumnPruningRule();
  column_pruning_rule.apply_to(root_node);

  // Remove LogicalPlanRootNode
  const auto optimized_node = root_node->left_input();
  root_node->set_left_input(nullptr);

  //std::cout << "Executing the following query " << EXECUTION_COUNT << " times (workload frequency " << frequency << "):\n" << *optimized_node << std::endl;
  std::vector<size_t> runtimes;
  for (auto count = size_t{0}; count < EXECUTION_COUNT; ++count) {
    Timer timer;
    const auto pqp = LQPTranslator{}.translate_node(optimized_node);
    const auto tasks = OperatorTask::make_tasks_from_operator(pqp);
    Hyrise::get().scheduler()->schedule_and_wait_for_tasks(tasks);
    const auto runtime = static_cast<size_t>(timer.lap().count());
    runtimes.push_back(runtime);
  }

  auto runtime_accumulated = size_t{0};
  for (const auto runtime : runtimes) {
    runtime_accumulated += runtime;
  }

  const auto avg_accumulated_runtime = static_cast<float>(runtime_accumulated) / static_cast<float>(runtimes.size());
  //std::cout << std::fixed << "Execution took in average " << avg_accumulated_runtime << " ns" << std::endl;

  return avg_accumulated_runtime;
}

int main() {
  auto& storage_manager = Hyrise::get().storage_manager;

  std::ofstream memory_consumption_csv_file(MEMORY_CONSUMPTION_FILE);
  std::ofstream performance_csv_file(PERFORMANCE_FILE);

  memory_consumption_csv_file << "CONFIG_NAME, MEMORY_CONSUMPTION,INDEX,TABLE\n";
  performance_csv_file << "CONFIG_NAME,QUERY_ID,EXECUTION_TIME\n";

  for (const auto& entry : std::filesystem::directory_iterator(CONFIG_PATH)) {
    const auto conf_path = entry.path();
    const auto conf_name = conf_path.stem();
    const auto filename = conf_path.filename().string();

    // check that file name is csv file
    if (filename.find(".csv") == std::string::npos) {
      std::cout << "Skipping " << conf_path << std::endl;
      continue;
    }

    // Create a new PING table that is used for the actual benchmark configuration
    std::cout << "Loading PING table ... ";
    Timer load_timer;
    const auto table = load_table(TBL_FILE, CHUNK_SIZE);
    std::cout << "done (" << format_duration(load_timer.lap()) << ")" << std::endl;
    
    // Load configuration from csv file
    const auto conf = read_file(conf_path);

    // Apply specified configuration schema
    std::cout << "Preparing table (encoding, sorting, ...) with given configuration: " << conf_name << " ... ";
    Timer preparation_timer;
    
    const auto sorted_table = std::make_shared<Table>(table->column_definitions(), 
      TableType::Data, CHUNK_SIZE, UseMvcc::No);
    const auto chunk_count = table->chunk_count();

    auto index_memory_consumption = size_t{0};
    auto conf_line_count = 0;
    for (auto chunk_id = ChunkID{0}; chunk_id < chunk_count; ++chunk_id) {
      const auto conf_chunk_id = ChunkID{static_cast<uint16_t>(std::stoi(conf[conf_line_count][0]))};
      const auto conf_chunk_sort_column_id = ColumnID{static_cast<uint16_t>(std::stoi(conf[conf_line_count][3]))};
      Assert(chunk_id == conf_chunk_id,
           "Expected chunk id does not match to chunk id in configuration file");
      
      // Sort 

      // Create single chunk table
      auto chunk = std::make_shared<Chunk>(get_segments_of_chunk(table, chunk_id));
      std::vector<std::shared_ptr<Chunk>> single_chunk_vector = {chunk};
      auto single_chunk_table = std::make_shared<Table>(table->column_definitions(), TableType::Data, std::move(single_chunk_vector), UseMvcc::No);

      // Sort single chunk table
      auto table_wrapper = std::make_shared<TableWrapper>(single_chunk_table);
      table_wrapper->execute();
      auto sort = std::make_shared<Sort>(
        table_wrapper, std::vector<SortColumnDefinition>{
          SortColumnDefinition{conf_chunk_sort_column_id, ORDER_BY_MODE}},CHUNK_SIZE, Sort::ForceMaterialization::Yes);
      sort->execute();
      const auto sorted_single_chunk_table = sort->get_output();

      // Add sorted chunk to sorted table
      // Note: we do not care about MVCC at all at the moment
      sorted_table->append_chunk(get_segments_of_chunk(sorted_single_chunk_table, ChunkID{0}));
      const auto& added_chunk = sorted_table->get_chunk(chunk_id);
      // Set order by for chunk 
      added_chunk->set_ordered_by({conf_chunk_sort_column_id, ORDER_BY_MODE});
      added_chunk->finalize();
     
      // Encode segments of sorted single chunk table

      for (ColumnID column_id = ColumnID{0}; column_id < added_chunk->column_count(); ++column_id) {
        const auto conf_column_id = ColumnID{static_cast<uint16_t>(std::stoi(conf[conf_line_count][1]))};
        Assert(column_id == conf_column_id,
           "Expected column id does not match column id in configuration file");

        const auto conf_segment_sort_column_id = ColumnID{static_cast<uint16_t>(std::stoi(conf[conf_line_count][3]))};
        Assert(conf_chunk_sort_column_id == conf_segment_sort_column_id,
           "Different sort configurations for a single chunk in configuration file");

        //Encode segment with specified encoding 
        const auto encoding_id = static_cast<uint16_t>(std::stoi(conf[conf_line_count][2]));
        const auto encoding = CHUNK_ENCODINGS[encoding_id];
        const auto segment = added_chunk->get_segment(column_id);
        
        Assert(encoding_id < CHUNK_ENCODINGS.size(), 
          "Undefined encoding specified in configuration file");

        const auto encoded_segment = ChunkEncoder::encode_segment(segment, segment->data_type(), encoding);
        added_chunk->replace_segment(column_id, encoded_segment);

        //Store index columns 

        const auto index_conf = static_cast<uint16_t>(std::stoi(conf[conf_line_count][4]));
        if (index_conf == 1) {
          Assert(encoding_id == 0, "Tried to set index on a not dictionary encoded segment");
          const auto added_index = added_chunk->create_index<GroupKeyIndex>(std::vector<ColumnID>{column_id});
          index_memory_consumption += added_index->memory_consumption();
        }

        ++conf_line_count;
      }
    }

    //Print::print(sorted_table);

    std::cout << " done (" << format_duration(preparation_timer.lap()) << ")" << std::endl;

    storage_manager.add_table(TABLE_NAME, sorted_table);

    // Write memory usage of indexes and table to memory consumption csv file 
    auto mem_usage = sorted_table->memory_usage(MemoryUsageCalculationMode::Full) + index_memory_consumption;
    memory_consumption_csv_file << conf_name << "," << mem_usage << "," << index_memory_consumption << "," << sorted_table->memory_usage(MemoryUsageCalculationMode::Full) <<"\n";

    // We load queries here, as the construction of the queries needs the existing actual table
    const auto queries = load_queries_from_csv(WORKLOAD_FILE);

    std::cout << "Execute benchmark queries ... ";
    auto query_id = size_t{0};
    for (auto const& query : queries) {
      const auto query_runtime = partially_optimize_translate_and_execute_query(query);
      performance_csv_file << conf_name << "," << query_id << "," << query_runtime << "\n";
      query_id += 1;
    }
    std::cout << " done" << std::endl;
    
    storage_manager.drop_table(TABLE_NAME);
  }

  memory_consumption_csv_file.close();
  performance_csv_file.close();

}
