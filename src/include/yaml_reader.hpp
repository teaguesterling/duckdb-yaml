#pragma once

#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/function/table_function.hpp"
#include "yaml-cpp/yaml.h"
#include <vector>

namespace duckdb {

class YAMLReader {
public:
    // Structure to hold YAML read options
    struct YAMLReadOptions {
        bool auto_detect_types = true;
        bool ignore_errors = false;
        bool allow_unquoted_strings = true;
        bool allow_duplicate_keys = true;
        size_t maximum_object_size = 16777216;  // 16MB default
        bool convert_to_json = false;           // Whether to convert YAML to JSON
        bool multi_document = true;             // Whether to handle multi-document YAML files
    };
    
    // Register the read_yaml table function
    static void RegisterFunction(DatabaseInstance &db);

    // Read a YAML file and create a vector of YAML nodes (for multi-document support)
    static std::vector<YAML::Node> ReadYAMLFromFile(ClientContext &context, const string &file_path, 
                                                  YAMLReadOptions options);
    
    // Convert a YAML node to a DuckDB value
    static Value YAMLNodeToValue(ClientContext &context, YAML::Node node, LogicalType target_type);

private:
    // Helper functions for converting YAML to DuckDB values
    static Value YAMLScalarToValue(ClientContext &context, YAML::Node node, LogicalType target_type);
    static Value YAMLSequenceToValue(ClientContext &context, YAML::Node node, LogicalType target_type);
    static Value YAMLMappingToValue(ClientContext &context, YAML::Node node, LogicalType target_type);
    
    // Helper to detect logical type from YAML node
    static LogicalType DetectYAMLType(YAML::Node node);
    
    // Helper to extract column definitions from YAML node
    static void ExtractColumnDefinitions(YAML::Node node, vector<string> &names, vector<LogicalType> &types);
    
    // Helper to fill a DataChunk from YAML nodes
    static void FillDataChunk(ClientContext &context, vector<YAML::Node> &nodes, 
                             const vector<string> &names, const vector<LogicalType> &types,
                             DataChunk &output, idx_t &row_count, const YAMLReadOptions &options);
};

} // namespace duckdb
