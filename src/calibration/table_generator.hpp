#pragma once


#include <iostream>

#include <all_type_variant.hpp>
#include <storage/encoding_type.hpp>
#include <synthetic_table_generator.hpp>
#include "calibration_table_wrapper.hpp"

class Table;

namespace opossum {

struct TableGeneratorConfig{
  std::set<DataType> data_types;
  std::set<EncodingType> encoding_types;
  std::vector<ColumnDataDistribution> column_data_distribution;
  std::set<int> chunk_offsets;
  std::set<int> row_counts;
};

class TableGenerator {
 public:
  explicit TableGenerator(std::shared_ptr<TableGeneratorConfig> config);
  std::vector<std::shared_ptr<const CalibrationTableWrapper>> generate() const;

 private:
  std::vector<ColumnDataDistribution> _column_data_distribution_collection;
  std::vector<int> _chunk_offsets;
  std::vector<int> _row_counts;
  std::vector<ColumnSpecification> _column_specs;
};

}  // namespace opossum
