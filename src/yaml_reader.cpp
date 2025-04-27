#include "yaml_types.hpp"
#include "yaml_reader.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

std::vector<YAML::Node> YAMLReader::ReadYAMLFromFile(ClientContext &context, const string &file_path, 
                                                   YAMLReadOptions options) {
    auto &fs = FileSystem::GetFileSystem(context);
    auto handle = fs.OpenFile(file_path, FileFlags::FILE_FLAGS_READ);
    
    // Read the file into memory
    idx_t file_size = fs.GetFileSize(*handle);
    if (file_size > options.maximum_object_size) {
        throw IOException("YAML file size exceeds maximum object size setting");
    }
    
    auto buffer = unique_ptr<char[]>(new char[file_size + 1]);
    fs.Read(*handle, buffer.get(), file_size);
    buffer[file_size] = '\0';  // Ensure null termination
    
    try {
        std::vector<YAML::Node> yaml_nodes;
        
        if (options.multi_document) {
            // Parse as multi-document YAML
            std::stringstream yaml_stream(buffer.get());
            YAML::Node doc;
            YAML::Parser parser(yaml_stream);
            
            while (parser.HandleNextDocument(doc)) {
                yaml_nodes.push_back(doc);
            }
            
            if (yaml_nodes.empty() && !options.ignore_errors) {
                throw IOException("No valid YAML documents found in file");
            }
        } else {
            // Parse as single-document YAML
            YAML::Node yaml_node = YAML::Load(buffer.get());
            yaml_nodes.push_back(yaml_node);
        }
        
        return yaml_nodes;
    } catch (YAML::Exception &e) {
        if (options.ignore_errors) {
            // Return an empty node if errors should be ignored
            return std::vector<YAML::Node>();
        } else {
            throw IOException("Error parsing YAML file: " + string(e.what()));
        }
    }
}

Value YAMLReader::YAMLNodeToValue(ClientContext &context, YAML::Node node, LogicalType target_type) {
    if (!node) {
        return Value(target_type);  // NULL value of target type
    }
    
    // If converting to JSON, use YAML to JSON conversion
    if (target_type.id() == LogicalTypeId::JSON) {
        string yaml_str;
        {
            std::stringstream ss;
            ss << node;
            yaml_str = ss.str();
        }
        string_t yaml_string_t = StringVector::AddString(yaml_str);
        string_t json_string_t = YAMLTypes::CastYAMLToJSON(context, yaml_string_t);
        return Value::JSON(json_string_t);
    }
    
    switch (node.Type()) {
        case YAML::NodeType::Scalar:
            return YAMLScalarToValue(context, node, target_type);
        case YAML::NodeType::Sequence:
            return YAMLSequenceToValue(context, node, target_type);
        case YAML::NodeType::Map:
            return YAMLMappingToValue(context, node, target_type);
        case YAML::NodeType::Null:
            return Value(target_type);  // NULL value of target type
        default:
            throw NotImplementedException("Unsupported YAML node type");
    }
}

Value YAMLReader::YAMLScalarToValue(ClientContext &context, YAML::Node node, LogicalType target_type) {
    if (!node.IsScalar()) {
        throw InvalidInputException("Expected YAML scalar, got a different type");
    }
    
    std::string scalar_value = node.as<std::string>();
    
    // Handle YAML scalar conversions based on target type
    switch (target_type.id()) {
        case LogicalTypeId::VARCHAR:
            return Value(scalar_value);
            
        case LogicalTypeId::BOOLEAN: {
            // Handle various boolean representations in YAML
            if (scalar_value == "true" || scalar_value == "yes" || 
                scalar_value == "on" || scalar_value == "1") {
                return Value::BOOLEAN(true);
            } else if (scalar_value == "false" || scalar_value == "no" || 
                      scalar_value == "off" || scalar_value == "0") {
                return Value::BOOLEAN(false);
            } else {
                throw InvalidInputException("Cannot convert YAML value '%s' to BOOLEAN", scalar_value);
            }
        }
            
        case LogicalTypeId::TINYINT:
        case LogicalTypeId::SMALLINT:
        case LogicalTypeId::INTEGER:
        case LogicalTypeId::BIGINT: {
            try {
                int64_t int_val = std::stoll(scalar_value);
                return Value::BIGINT(int_val);
            } catch (...) {
                throw InvalidInputException("Cannot convert YAML value '%s' to INTEGER type", scalar_value);
            }
        }
            
        case LogicalTypeId::FLOAT:
        case LogicalTypeId::DOUBLE: {
            try {
                double double_val = std::stod(scalar_value);
                return Value::DOUBLE(double_val);
            } catch (...) {
                throw InvalidInputException("Cannot convert YAML value '%s' to FLOAT type", scalar_value);
            }
        }
            
        case LogicalTypeId::DATE: {
            try {
                // Try to parse the date in ISO format
                date_t date_val = Date::FromString(scalar_value);
                return Value::DATE(date_val);
            } catch (...) {
                throw InvalidInputException("Cannot convert YAML value '%s' to DATE type", scalar_value);
            }
        }
            
        case LogicalTypeId::TIMESTAMP: {
            try {
                // Try to parse the timestamp in ISO format
                timestamp_t ts_val = Timestamp::FromString(scalar_value);
                return Value::TIMESTAMP(ts_val);
            } catch (...) {
                throw InvalidInputException("Cannot convert YAML value '%s' to TIMESTAMP type", scalar_value);
            }
        }
            
        case LogicalTypeId::JSON: {
            // Convert scalar to JSON string
            if (scalar_value == "null" || scalar_value == "~") {
                return Value::JSON("null");
            } else if (scalar_value == "true" || scalar_value == "yes" || scalar_value == "on") {
                return Value::JSON("true");
            } else if (scalar_value == "false" || scalar_value == "no" || scalar_value == "off") {
                return Value::JSON("false");
            }
            
            // Try to parse as number
            try {
                size_t pos;
                std::stod(scalar_value, &pos);
                if (pos == scalar_value.size()) {
                    return Value::JSON(scalar_value); // It's a number, use as is
                }
            } catch (...) {
                // Not a number, treat as string below
            }
            
            // Escape for JSON string
            std::string json_str = "\"";
            for (char c : scalar_value) {
                switch (c) {
                    case '\"': json_str += "\\\""; break;
                    case '\\': json_str += "\\\\"; break;
                    case '/':  json_str += "\\/"; break;
                    case '\b': json_str += "\\b"; break;
                    case '\f': json_str += "\\f"; break;
                    case '\n': json_str += "\\n"; break;
                    case '\r': json_str += "\\r"; break;
                    case '\t': json_str += "\\t"; break;
                    default:
                        if ('\x00' <= c && c <= '\x1f') {
                            char buf[8];
                            snprintf(buf, sizeof(buf), "\\u%04x", c);
                            json_str += buf;
                        } else {
                            json_str += c;
                        }
                }
            }
            json_str += "\"";
            return Value::JSON(json_str);
        }
            
        default:
            throw InvalidInputException("Unsupported target type for YAML scalar conversion");
    }
}

Value YAMLReader::YAMLSequenceToValue(ClientContext &context, YAML::Node node, LogicalType target_type) {
    if (!node.IsSequence()) {
        throw InvalidInputException("Expected YAML sequence, got a different type");
    }
    
    if (target_type.id() != LogicalTypeId::LIST && target_type.id() != LogicalTypeId::JSON) {
        throw TypeMismatchException(target_type, LogicalType::LIST(LogicalType::ANY));
    }
    
    if (target_type.id() == LogicalTypeId::JSON) {
        // Convert sequence to JSON array
        std::string json_str = "[";
        for (size_t i = 0; i < node.size(); i++) {
            if (i > 0) {
                json_str += ",";
            }
            
            // Recursively convert each element to JSON
            Value json_val = YAMLNodeToValue(context, node[i], LogicalType::JSON());
            json_str += json_val.GetValue<string>();
        }
        json_str += "]";
        
        return Value::JSON(json_str);
    }
    
    // Extract the child type from the list type
    auto &list_type = ListType::GetChildType(target_type);
    
    vector<Value> values;
    for (auto it = node.begin(); it != node.end(); ++it) {
        values.push_back(YAMLNodeToValue(context, *it, list_type));
    }
    
    return Value::LIST(values);
}

Value YAMLReader::YAMLMappingToValue(ClientContext &context, YAML::Node node, LogicalType target_type) {
    if (!node.IsMap()) {
        throw InvalidInputException("Expected YAML map, got a different type");
    }
    
    if (target_type.id() == LogicalTypeId::JSON) {
        // Convert map to JSON object
        std::string json_str = "{";
        bool first = true;
        
        for (auto it = node.begin(); it != node.end(); ++it) {
            if (!first) {
                json_str += ",";
            }
            first = false;
            
            // Key must be a string in JSON
            std::string key = it->first.as<std::string>();
            
            // Escape key for JSON
            std::string escaped_key = "\"";
            for (char c : key) {
                switch (c) {
                    case '\"': escaped_key += "\\\""; break;
                    case '\\': escaped_key += "\\\\"; break;
                    case '/':  escaped_key += "\\/"; break;
                    case '\b': escaped_key += "\\b"; break;
                    case '\f': escaped_key += "\\f"; break;
                    case '\n': escaped_key += "\\n"; break;
                    case '\r': escaped_key += "\\r"; break;
                    case '\t': escaped_key += "\\t"; break;
                    default:
                        if ('\x00' <= c && c <= '\x1f') {
                            char buf[8];
                            snprintf(buf, sizeof(buf), "\\u%04x", c);
                            escaped_key += buf;
                        } else {
                            escaped_key += c;
                        }
                }
            }
            escaped_key += "\"";
            
            // Recursively convert value to JSON
            Value json_val = YAMLNodeToValue(context, it->second, LogicalType::JSON());
            
            json_str += escaped_key + ":" + json_val.GetValue<string>();
        }
        
        json_str += "}";
        return Value::JSON(json_str);
    }
    
    if (target_type.id() != LogicalTypeId::STRUCT) {
        throw TypeMismatchException(target_type, LogicalType::STRUCT({{"", LogicalType::ANY}}));
    }
    
    // Create a struct with child fields
    child_list_t<Value> struct_values;
    
    // If target type is struct, extract fields and parse them according to the struct definition
    auto &struct_children = StructType::GetChildTypes(target_type);
    
    for (auto &entry : struct_children) {
        const std::string &field_name = entry.first;
        const LogicalType &field_type = entry.second;
        
        if (node[field_name]) {
            struct_values.push_back(make_pair(field_name, YAMLNodeToValue(context, node[field_name], field_type)));
        } else {
            struct_values.push_back(make_pair(field_name, Value(field_type)));  // NULL if field not found
        }
    }
    
    return Value::STRUCT(struct_values);
}

LogicalType YAMLReader::DetectYAMLType(YAML::Node node) {
    // Implementation remains the same as before
    // (I'll omit repeating the entire implementation for brevity)
    if (!node) {
        return LogicalType::VARCHAR;
    }
    
    switch (node.Type()) {
        case YAML::NodeType::Scalar:
            // ... implementation ...
            return LogicalType::VARCHAR;
        case YAML::NodeType::Sequence:
            // ... implementation ...
            return LogicalType::LIST(LogicalType::VARCHAR);
        case YAML::NodeType::Map:
            // ... implementation ...
            return LogicalType::STRUCT({});
        case YAML::NodeType::Null:
            return LogicalType::VARCHAR;
        default:
            return LogicalType::VARCHAR;
    }
}

void YAMLReader::ExtractColumnDefinitions(YAML::Node node, vector<string> &names, vector<LogicalType> &types) {
    // Implementation remains the same as before
    // (I'll omit repeating the entire implementation for brevity)
}

void YAMLReader::FillDataChunk(ClientContext &context, vector<YAML::Node> &nodes, 
                              const vector<string> &names, const vector<LogicalType> &types,
                              DataChunk &output, idx_t &row_count, const YAMLReadOptions &options) {
    // Implementation remains the same as before
    // (I'll omit repeating the entire implementation for brevity)
}

// Bind function for read_yaml
struct YAMLReadBindData : public TableFunctionData {
    YAMLReadBindData(string file_path, YAMLReader::YAMLReadOptions options) 
        : file_path(std::move(file_path)), options(options), row_count(0) {}
    
    string file_path;
    YAMLReader::YAMLReadOptions options;
    idx_t row_count;
    vector<YAML::Node> yaml_nodes;
};

static unique_ptr<FunctionData> YAMLReadBind(ClientContext &context, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<string> &names) {
    // Implementation remains the same as before
    // (I'll omit repeating the entire implementation for brevity)
    return nullptr; // This is just a placeholder
}

// Function to read a YAML file and convert it to a DuckDB table
static void YAMLReadFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    // Implementation remains the same as before
    // (I'll omit repeating the entire implementation for brevity)
}

void YAMLReader::RegisterFunction(DatabaseInstance &db) {
    // Implementation remains the same as before
    // (I'll omit repeating the entire implementation for brevity)
}

} // namespace duckdb
