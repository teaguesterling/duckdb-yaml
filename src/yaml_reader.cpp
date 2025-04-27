#include "yaml_reader.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

// Bind data structure for read_yaml
struct YAMLReadBindData : public TableFunctionData {
    YAMLReadBindData(string file_path, YAMLReader::YAMLReadOptions options) 
        : file_path(std::move(file_path)), options(options) {}
    
    string file_path;
    YAMLReader::YAMLReadOptions options;
    vector<YAML::Node> yaml_nodes;
    vector<string> names;
    vector<LogicalType> types;
    idx_t current_row = 0;
};

LogicalType YAMLReader::DetectYAMLType(YAML::Node node) {
    if (!node) {
        return LogicalType::VARCHAR;
    }
    
    switch (node.Type()) {
        case YAML::NodeType::Scalar: {
            std::string scalar_value = node.Scalar();
            
            // Basic type detection
            if (scalar_value == "true" || scalar_value == "false" || 
                scalar_value == "yes" || scalar_value == "no") {
                return LogicalType::BOOLEAN;
            }
            
            try {
                // Try integer
                size_t pos;
                std::stoll(scalar_value, &pos);
                if (pos == scalar_value.size()) {
                    return LogicalType::BIGINT;
                }
                
                // Try double
                std::stod(scalar_value, &pos);
                if (pos == scalar_value.size()) {
                    return LogicalType::DOUBLE;
                }
            } catch (...) {
                // Not a number
            }
            
            return LogicalType::VARCHAR;
        }
        case YAML::NodeType::Sequence:
            return LogicalType::LIST(LogicalType::VARCHAR);
        case YAML::NodeType::Map: {
            child_list_t<LogicalType> struct_children;
            for (auto it = node.begin(); it != node.end(); ++it) {
                std::string key = it->first.Scalar();
                LogicalType value_type = DetectYAMLType(it->second);
                struct_children.push_back(make_pair(key, value_type));
            }
            return LogicalType::STRUCT(struct_children);
        }
        default:
            return LogicalType::VARCHAR;
    }
}

Value YAMLReader::YAMLNodeToValue(YAML::Node node, LogicalType target_type) {
    if (!node) {
        return Value(target_type); // NULL value
    }
    
    // Handle based on YAML node type
    switch (node.Type()) {
        case YAML::NodeType::Scalar: {
            std::string scalar_value = node.Scalar();
            
            if (target_type.id() == LogicalTypeId::VARCHAR) {
                return Value(scalar_value);
            } else if (target_type.id() == LogicalTypeId::BOOLEAN) {
                if (scalar_value == "true" || scalar_value == "yes" || scalar_value == "on") {
                    return Value::BOOLEAN(true);
                } else if (scalar_value == "false" || scalar_value == "no" || scalar_value == "off") {
                    return Value::BOOLEAN(false);
                }
                return Value(target_type); // NULL if not valid boolean
            } else if (target_type.id() == LogicalTypeId::BIGINT) {
                try {
                    return Value::BIGINT(std::stoll(scalar_value));
                } catch (...) {
                    return Value(target_type); // NULL if conversion fails
                }
            } else if (target_type.id() == LogicalTypeId::DOUBLE) {
                try {
                    return Value::DOUBLE(std::stod(scalar_value));
                } catch (...) {
                    return Value(target_type); // NULL if conversion fails
                }
            }
            return Value(scalar_value); // Default to string
        }
        case YAML::NodeType::Sequence: {
            if (target_type.id() != LogicalTypeId::LIST) {
                return Value(target_type); // NULL if not expecting a list
            }
            
            // Get child type for list
            auto child_type = ListType::GetChildType(target_type);
            
            // Create list of values
            vector<Value> values;
            for (size_t i = 0; i < node.size(); i++) {
                values.push_back(YAMLNodeToValue(node[i], child_type));
            }
            
            return Value::LIST(values);
        }
        case YAML::NodeType::Map: {
            if (target_type.id() != LogicalTypeId::STRUCT) {
                return Value(target_type); // NULL if not expecting a struct
            }
            
            // Get struct children
            auto &struct_children = StructType::GetChildTypes(target_type);
            
            // Create struct values
            child_list_t<Value> struct_values;
            for (auto &entry : struct_children) {
                if (node[entry.first]) {
                    struct_values.push_back(make_pair(
                        entry.first, 
                        YAMLNodeToValue(node[entry.first], entry.second)
                    ));
                } else {
                    struct_values.push_back(make_pair(
                        entry.first, 
                        Value(entry.second) // NULL value
                    ));
                }
            }
            
            return Value::STRUCT(struct_values);
        }
        default:
            return Value(target_type); // NULL for unknown type
    }
}

unique_ptr<FunctionData> YAMLReader::YAMLReadBind(ClientContext &context, 
                                               TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types, 
                                               vector<string> &names) {
    auto &fs = FileSystem::GetFileSystem(context);
    
    // Extract parameters
    auto file_path = input.inputs[0].GetValue<string>();
    YAMLReadOptions options;
    
    // Parse optional parameters
    if (input.named_parameters.count("auto_detect")) {
        options.auto_detect_types = input.named_parameters["auto_detect"].GetValue<bool>();
    }
    if (input.named_parameters.count("ignore_errors")) {
        options.ignore_errors = input.named_parameters["ignore_errors"].GetValue<bool>();
    }
    if (input.named_parameters.count("maximum_object_size")) {
        options.maximum_object_size = input.named_parameters["maximum_object_size"].GetValue<int64_t>();
    }
    if (input.named_parameters.count("multi_document")) {
        options.multi_document = input.named_parameters["multi_document"].GetValue<bool>();
    }
    
    // Create bind data
    auto result = make_uniq<YAMLReadBindData>(file_path, options);
    
    // Read YAML file
    try {
        auto handle = fs.OpenFile(file_path, FileFlags::FILE_FLAGS_READ);
        idx_t file_size = fs.GetFileSize(*handle);
        
        if (file_size > options.maximum_object_size) {
            throw IOException("YAML file size exceeds maximum allowed size");
        }
        
        auto buffer = unique_ptr<char[]>(new char[file_size + 1]);
        fs.Read(*handle, buffer.get(), file_size);
        buffer[file_size] = '\0';
        
        if (options.multi_document) {
            // Parse as multi-document YAML
            std::stringstream yaml_stream(buffer.get());
            try {
                result->yaml_nodes = YAML::LoadAll(yaml_stream);
            } catch (const YAML::Exception &e) {
                if (!options.ignore_errors) {
                    throw IOException("Error parsing YAML file: " + string(e.what()));
                }
            }
        } else {
            // Parse as single-document YAML
            try {
                YAML::Node yaml_node = YAML::Load(buffer.get());
                result->yaml_nodes.push_back(yaml_node);
            } catch (const YAML::Exception &e) {
                if (!options.ignore_errors) {
                    throw IOException("Error parsing YAML file: " + string(e.what()));
                }
            }
        }
    } catch (const std::exception &e) {
        throw IOException("Error reading YAML file: " + string(e.what()));
    }
    
    if (result->yaml_nodes.empty()) {
        // Empty result
        names.emplace_back("yaml");
        return_types.emplace_back(LogicalType::VARCHAR);
        return std::move(result);
    }
    
    // Auto-detect columns from first document
    if (options.auto_detect_types) {
        // For each document, we create a column with the document structure
        names.emplace_back("yaml");
        auto doc_type = DetectYAMLType(result->yaml_nodes[0]);
        return_types.emplace_back(doc_type);
    } else {
        // If not auto-detecting, just use VARCHAR for the whole document
        names.emplace_back("yaml");
        return_types.emplace_back(LogicalType::VARCHAR);
    }
    
    // Save column info
    result->names = names;
    result->types = return_types;
    
    return std::move(result);
}

void YAMLReader::YAMLReadFunction(ClientContext &context, TableFunctionInput &data_p, 
                               DataChunk &output) {
    auto &bind_data = (YAMLReadBindData &)*data_p.bind_data;
    
    // If we've processed all rows, we're done
    if (bind_data.current_row >= bind_data.yaml_nodes.size()) {
        output.SetCardinality(0);
        return;
    }
    
    // Process up to STANDARD_VECTOR_SIZE rows at a time
    idx_t count = 0;
    idx_t max_count = std::min((idx_t)STANDARD_VECTOR_SIZE, bind_data.yaml_nodes.size() - bind_data.current_row);
    
    // Set up the output chunk
    output.Reset();
    
    // Fill the output
    for (idx_t i = 0; i < max_count; i++) {
        // Get the current YAML node
        YAML::Node node = bind_data.yaml_nodes[bind_data.current_row + i];
        
        // Convert to DuckDB value
        Value val = YAMLNodeToValue(node, bind_data.types[0]);
        
        // Add to output
        output.SetValue(0, count, val);
        count++;
    }
    
    // Update current row
    bind_data.current_row += count;
    
    // Set the cardinality
    output.SetCardinality(count);
}

void YAMLReader::RegisterFunction(DatabaseInstance &db) {
    // Create read_yaml table function
    TableFunction read_yaml("read_yaml", {LogicalType::VARCHAR}, YAMLReadFunction, YAMLReadBind);
    
    // Add optional named parameters
    read_yaml.named_parameters["auto_detect"] = LogicalType::BOOLEAN;
    read_yaml.named_parameters["ignore_errors"] = LogicalType::BOOLEAN;
    read_yaml.named_parameters["maximum_object_size"] = LogicalType::BIGINT;
    read_yaml.named_parameters["multi_document"] = LogicalType::BOOLEAN;
    
    // Register the function
    CreateTableFunctionInfo read_yaml_info(read_yaml);
    db.GetCatalog().CreateTableFunction(db.GetCatalog().GetSystemTransaction(), &read_yaml_info);
}

} // namespace duckdb
