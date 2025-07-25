// Licensed to the LF AI & Data foundation under one
// or more contributor license agreements. See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership. The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <memory>

#include "arrow/array/builder_binary.h"
#include "arrow/array/builder_nested.h"
#include "arrow/scalar.h"
#include "arrow/type_fwd.h"
#include "fmt/format.h"
#include "index/Utils.h"
#include "log/Log.h"

#include "common/Consts.h"
#include "common/EasyAssert.h"
#include "common/FieldData.h"
#include "common/FieldDataInterface.h"
#ifdef AZURE_BUILD_DIR
#include "storage/azure/AzureChunkManager.h"
#endif
#ifdef ENABLE_GCP_NATIVE
#include "storage/gcp-native-storage/GcpNativeChunkManager.h"
#endif
#include "storage/ChunkManager.h"
#include "storage/DiskFileManagerImpl.h"
#include "storage/InsertData.h"
#include "storage/LocalChunkManager.h"
#include "storage/MemFileManagerImpl.h"
#include "storage/MinioChunkManager.h"
#ifdef USE_OPENDAL
#include "storage/opendal/OpenDALChunkManager.h"
#endif
#include "storage/Types.h"
#include "storage/Util.h"
#include "storage/ThreadPools.h"
#include "storage/MemFileManagerImpl.h"
#include "storage/DiskFileManagerImpl.h"
#include "segcore/memory_planner.h"
#include "mmap/Types.h"
#include "milvus-storage/format/parquet/file_reader.h"
#include "milvus-storage/filesystem/fs.h"

namespace milvus::storage {

constexpr const char* TEMP = "tmp";

std::map<std::string, ChunkManagerType> ChunkManagerType_Map = {
    {"local", ChunkManagerType::Local},
    {"minio", ChunkManagerType::Minio},
    {"remote", ChunkManagerType::Remote},
    {"opendal", ChunkManagerType::OpenDAL}};

enum class CloudProviderType : int8_t {
    UNKNOWN = 0,
    AWS = 1,
    GCP = 2,
    ALIYUN = 3,
    AZURE = 4,
    TENCENTCLOUD = 5,
    GCPNATIVE = 6,
};

std::map<std::string, CloudProviderType> CloudProviderType_Map = {
    {"aws", CloudProviderType::AWS},
    {"gcp", CloudProviderType::GCP},
    {"aliyun", CloudProviderType::ALIYUN},
    {"azure", CloudProviderType::AZURE},
    {"tencent", CloudProviderType::TENCENTCLOUD},
    {"gcpnative", CloudProviderType::GCPNATIVE}};

std::map<std::string, int> ReadAheadPolicy_Map = {
    {"normal", MADV_NORMAL},
    {"random", MADV_RANDOM},
    {"sequential", MADV_SEQUENTIAL},
    {"willneed", MADV_WILLNEED},
    {"dontneed", MADV_DONTNEED}};

// in arrow, null_bitmap read from the least significant bit
std::vector<uint8_t>
genValidIter(const uint8_t* valid_data, int length) {
    std::vector<uint8_t> valid_data_;
    valid_data_.reserve(length);
    for (size_t i = 0; i < length; ++i) {
        auto bit = (valid_data[i >> 3] >> (i & 0x07)) & 1;
        valid_data_.push_back(bit);
    }
    return valid_data_;
}

void
ReadMediumType(BinlogReaderPtr reader) {
    AssertInfo(reader->Tell() == 0,
               "medium type must be parsed from stream header");
    int32_t magic_num;
    auto ret = reader->Read(sizeof(magic_num), &magic_num);
    AssertInfo(ret.ok(), "read binlog failed: {}", ret.what());
    AssertInfo(magic_num == MAGIC_NUM, "invalid magic num: {}", magic_num);
}

void
add_vector_payload(std::shared_ptr<arrow::ArrayBuilder> builder,
                   uint8_t* values,
                   int length) {
    AssertInfo(builder != nullptr, "empty arrow builder");
    auto binary_builder =
        std::dynamic_pointer_cast<arrow::FixedSizeBinaryBuilder>(builder);
    auto ast = binary_builder->AppendValues(values, length);
    AssertInfo(
        ast.ok(), "append value to arrow builder failed: {}", ast.ToString());
}

// append values for numeric data
template <typename DT, typename BT>
void
add_numeric_payload(std::shared_ptr<arrow::ArrayBuilder> builder,
                    DT* start,
                    const uint8_t* valid_data,
                    bool nullable,
                    int length) {
    AssertInfo(builder != nullptr, "empty arrow builder");
    auto numeric_builder = std::dynamic_pointer_cast<BT>(builder);
    arrow::Status ast;
    if (nullable) {
        // need iter to read valid_data when write
        auto iter = genValidIter(valid_data, length);
        ast =
            numeric_builder->AppendValues(start, start + length, iter.begin());
        AssertInfo(ast.ok(), "append value to arrow builder failed");
    } else {
        ast = numeric_builder->AppendValues(start, start + length);
        AssertInfo(ast.ok(), "append value to arrow builder failed");
    }
}

void
AddPayloadToArrowBuilder(std::shared_ptr<arrow::ArrayBuilder> builder,
                         const Payload& payload) {
    AssertInfo(builder != nullptr, "empty arrow builder");
    auto raw_data = const_cast<uint8_t*>(payload.raw_data);
    auto length = payload.rows;
    auto data_type = payload.data_type;
    auto nullable = payload.nullable;

    switch (data_type) {
        case DataType::BOOL: {
            auto bool_data = reinterpret_cast<bool*>(raw_data);
            add_numeric_payload<bool, arrow::BooleanBuilder>(
                builder, bool_data, payload.valid_data, nullable, length);
            break;
        }
        case DataType::INT8: {
            auto int8_data = reinterpret_cast<int8_t*>(raw_data);
            add_numeric_payload<int8_t, arrow::Int8Builder>(
                builder, int8_data, payload.valid_data, nullable, length);
            break;
        }
        case DataType::INT16: {
            auto int16_data = reinterpret_cast<int16_t*>(raw_data);
            add_numeric_payload<int16_t, arrow::Int16Builder>(
                builder, int16_data, payload.valid_data, nullable, length);
            break;
        }
        case DataType::INT32: {
            auto int32_data = reinterpret_cast<int32_t*>(raw_data);
            add_numeric_payload<int32_t, arrow::Int32Builder>(
                builder, int32_data, payload.valid_data, nullable, length);
            break;
        }
        case DataType::INT64: {
            auto int64_data = reinterpret_cast<int64_t*>(raw_data);
            add_numeric_payload<int64_t, arrow::Int64Builder>(
                builder, int64_data, payload.valid_data, nullable, length);
            break;
        }
        case DataType::FLOAT: {
            auto float_data = reinterpret_cast<float*>(raw_data);
            add_numeric_payload<float, arrow::FloatBuilder>(
                builder, float_data, payload.valid_data, nullable, length);
            break;
        }
        case DataType::DOUBLE: {
            auto double_data = reinterpret_cast<double_t*>(raw_data);
            add_numeric_payload<double, arrow::DoubleBuilder>(
                builder, double_data, payload.valid_data, nullable, length);
            break;
        }
        case DataType::VECTOR_FLOAT16:
        case DataType::VECTOR_BFLOAT16:
        case DataType::VECTOR_BINARY:
        case DataType::VECTOR_INT8:
        case DataType::VECTOR_FLOAT: {
            add_vector_payload(builder, const_cast<uint8_t*>(raw_data), length);
            break;
        }
        case DataType::VECTOR_SPARSE_FLOAT: {
            PanicInfo(DataTypeInvalid,
                      "Sparse Float Vector payload should be added by calling "
                      "add_one_binary_payload",
                      data_type);
        }
        default: {
            PanicInfo(DataTypeInvalid, "unsupported data type {}", data_type);
        }
    }
}

void
AddOneStringToArrowBuilder(std::shared_ptr<arrow::ArrayBuilder> builder,
                           const char* str,
                           int str_size) {
    AssertInfo(builder != nullptr, "empty arrow builder");
    auto string_builder =
        std::dynamic_pointer_cast<arrow::StringBuilder>(builder);
    arrow::Status ast;
    if (str == nullptr || str_size < 0) {
        ast = string_builder->AppendNull();
    } else {
        ast = string_builder->Append(str, str_size);
    }
    AssertInfo(
        ast.ok(), "append value to arrow builder failed: {}", ast.ToString());
}

void
AddOneBinaryToArrowBuilder(std::shared_ptr<arrow::ArrayBuilder> builder,
                           const uint8_t* data,
                           int length) {
    AssertInfo(builder != nullptr, "empty arrow builder");
    auto binary_builder =
        std::dynamic_pointer_cast<arrow::BinaryBuilder>(builder);
    arrow::Status ast;
    if (data == nullptr || length < 0) {
        ast = binary_builder->AppendNull();
    } else {
        ast = binary_builder->Append(data, length);
    }
    AssertInfo(
        ast.ok(), "append value to arrow builder failed: {}", ast.ToString());
}

std::shared_ptr<arrow::ArrayBuilder>
CreateArrowBuilder(DataType data_type) {
    switch (static_cast<DataType>(data_type)) {
        case DataType::BOOL: {
            return std::make_shared<arrow::BooleanBuilder>();
        }
        case DataType::INT8: {
            return std::make_shared<arrow::Int8Builder>();
        }
        case DataType::INT16: {
            return std::make_shared<arrow::Int16Builder>();
        }
        case DataType::INT32: {
            return std::make_shared<arrow::Int32Builder>();
        }
        case DataType::INT64: {
            return std::make_shared<arrow::Int64Builder>();
        }
        case DataType::FLOAT: {
            return std::make_shared<arrow::FloatBuilder>();
        }
        case DataType::DOUBLE: {
            return std::make_shared<arrow::DoubleBuilder>();
        }
        case DataType::VARCHAR:
        case DataType::STRING:
        case DataType::TEXT: {
            return std::make_shared<arrow::StringBuilder>();
        }
        case DataType::ARRAY:
        case DataType::VECTOR_ARRAY:
        case DataType::JSON: {
            return std::make_shared<arrow::BinaryBuilder>();
        }
        // sparse float vector doesn't require a dim
        case DataType::VECTOR_SPARSE_FLOAT: {
            return std::make_shared<arrow::BinaryBuilder>();
        }
        default: {
            PanicInfo(
                DataTypeInvalid, "unsupported numeric data type {}", data_type);
        }
    }
}

std::shared_ptr<arrow::ArrayBuilder>
CreateArrowBuilder(DataType data_type, int dim) {
    switch (static_cast<DataType>(data_type)) {
        case DataType::VECTOR_FLOAT: {
            AssertInfo(dim > 0, "invalid dim value: {}", dim);
            return std::make_shared<arrow::FixedSizeBinaryBuilder>(
                arrow::fixed_size_binary(dim * sizeof(float)));
        }
        case DataType::VECTOR_BINARY: {
            AssertInfo(dim % 8 == 0 && dim > 0, "invalid dim value: {}", dim);
            return std::make_shared<arrow::FixedSizeBinaryBuilder>(
                arrow::fixed_size_binary(dim / 8));
        }
        case DataType::VECTOR_FLOAT16: {
            AssertInfo(dim > 0, "invalid dim value: {}", dim);
            return std::make_shared<arrow::FixedSizeBinaryBuilder>(
                arrow::fixed_size_binary(dim * sizeof(float16)));
        }
        case DataType::VECTOR_BFLOAT16: {
            AssertInfo(dim > 0, "invalid dim value");
            return std::make_shared<arrow::FixedSizeBinaryBuilder>(
                arrow::fixed_size_binary(dim * sizeof(bfloat16)));
        }
        case DataType::VECTOR_INT8: {
            AssertInfo(dim > 0, "invalid dim value");
            return std::make_shared<arrow::FixedSizeBinaryBuilder>(
                arrow::fixed_size_binary(dim * sizeof(int8)));
        }
        default: {
            PanicInfo(
                DataTypeInvalid, "unsupported vector data type {}", data_type);
        }
    }
}

std::shared_ptr<arrow::Scalar>
CreateArrowScalarFromDefaultValue(const FieldMeta& field_meta) {
    auto default_var = field_meta.default_value();
    AssertInfo(default_var.has_value(),
               "cannot create Arrow Scalar from empty default value");
    auto default_value = default_var.value();
    switch (field_meta.get_data_type()) {
        case DataType::BOOL:
            return std::make_shared<arrow::BooleanScalar>(
                default_value.bool_data());
        case DataType::INT8:
            return std::make_shared<arrow::Int8Scalar>(
                default_value.int_data());
        case DataType::INT16:
            return std::make_shared<arrow::Int16Scalar>(
                default_value.int_data());
        case DataType::INT32:
            return std::make_shared<arrow::Int32Scalar>(
                default_value.int_data());
        case DataType::INT64:
            return std::make_shared<arrow::Int64Scalar>(
                default_value.long_data());
        case DataType::FLOAT:
            return std::make_shared<arrow::FloatScalar>(
                default_value.float_data());
        case DataType::DOUBLE:
            return std::make_shared<arrow::DoubleScalar>(
                default_value.double_data());
        case DataType::VARCHAR:
        case DataType::STRING:
        case DataType::TEXT:
            return std::make_shared<arrow::StringScalar>(
                default_value.string_data());
        default:
            PanicInfo(DataTypeInvalid,
                      "unsupported default value data type {}",
                      field_meta.get_data_type());
    }
}

std::shared_ptr<arrow::Schema>
CreateArrowSchema(DataType data_type, bool nullable) {
    switch (static_cast<DataType>(data_type)) {
        case DataType::BOOL: {
            return arrow::schema(
                {arrow::field("val", arrow::boolean(), nullable)});
        }
        case DataType::INT8: {
            return arrow::schema(
                {arrow::field("val", arrow::int8(), nullable)});
        }
        case DataType::INT16: {
            return arrow::schema(
                {arrow::field("val", arrow::int16(), nullable)});
        }
        case DataType::INT32: {
            return arrow::schema(
                {arrow::field("val", arrow::int32(), nullable)});
        }
        case DataType::INT64: {
            return arrow::schema(
                {arrow::field("val", arrow::int64(), nullable)});
        }
        case DataType::FLOAT: {
            return arrow::schema(
                {arrow::field("val", arrow::float32(), nullable)});
        }
        case DataType::DOUBLE: {
            return arrow::schema(
                {arrow::field("val", arrow::float64(), nullable)});
        }
        case DataType::VARCHAR:
        case DataType::STRING:
        case DataType::TEXT: {
            return arrow::schema(
                {arrow::field("val", arrow::utf8(), nullable)});
        }
        case DataType::ARRAY:
        case DataType::VECTOR_ARRAY:
        case DataType::JSON: {
            return arrow::schema(
                {arrow::field("val", arrow::binary(), nullable)});
        }
        // sparse float vector doesn't require a dim
        case DataType::VECTOR_SPARSE_FLOAT: {
            return arrow::schema(
                {arrow::field("val", arrow::binary(), nullable)});
        }
        default: {
            PanicInfo(
                DataTypeInvalid, "unsupported numeric data type {}", data_type);
        }
    }
}

std::shared_ptr<arrow::Schema>
CreateArrowSchema(DataType data_type, int dim, bool nullable) {
    switch (static_cast<DataType>(data_type)) {
        case DataType::VECTOR_FLOAT: {
            AssertInfo(dim > 0, "invalid dim value: {}", dim);
            return arrow::schema(
                {arrow::field("val",
                              arrow::fixed_size_binary(dim * sizeof(float)),
                              nullable)});
        }
        case DataType::VECTOR_BINARY: {
            AssertInfo(dim % 8 == 0 && dim > 0, "invalid dim value: {}", dim);
            return arrow::schema({arrow::field(
                "val", arrow::fixed_size_binary(dim / 8), nullable)});
        }
        case DataType::VECTOR_FLOAT16: {
            AssertInfo(dim > 0, "invalid dim value: {}", dim);
            return arrow::schema(
                {arrow::field("val",
                              arrow::fixed_size_binary(dim * sizeof(float16)),
                              nullable)});
        }
        case DataType::VECTOR_BFLOAT16: {
            AssertInfo(dim > 0, "invalid dim value");
            return arrow::schema(
                {arrow::field("val",
                              arrow::fixed_size_binary(dim * sizeof(bfloat16)),
                              nullable)});
        }
        case DataType::VECTOR_SPARSE_FLOAT: {
            return arrow::schema(
                {arrow::field("val", arrow::binary(), nullable)});
        }
        case DataType::VECTOR_INT8: {
            AssertInfo(dim > 0, "invalid dim value");
            return arrow::schema(
                {arrow::field("val",
                              arrow::fixed_size_binary(dim * sizeof(int8)),
                              nullable)});
        }
        default: {
            PanicInfo(
                DataTypeInvalid, "unsupported vector data type {}", data_type);
        }
    }
}

int
GetDimensionFromFileMetaData(const parquet::ColumnDescriptor* schema,
                             DataType data_type) {
    switch (data_type) {
        case DataType::VECTOR_FLOAT: {
            return schema->type_length() / sizeof(float);
        }
        case DataType::VECTOR_BINARY: {
            return schema->type_length() * 8;
        }
        case DataType::VECTOR_FLOAT16: {
            return schema->type_length() / sizeof(float16);
        }
        case DataType::VECTOR_BFLOAT16: {
            return schema->type_length() / sizeof(bfloat16);
        }
        case DataType::VECTOR_SPARSE_FLOAT: {
            PanicInfo(DataTypeInvalid,
                      fmt::format("GetDimensionFromFileMetaData should not be "
                                  "called for sparse vector"));
        }
        case DataType::VECTOR_INT8: {
            return schema->type_length() / sizeof(int8);
        }
        default:
            PanicInfo(DataTypeInvalid, "unsupported data type {}", data_type);
    }
}

int
GetDimensionFromArrowArray(std::shared_ptr<arrow::Array> data,
                           DataType data_type) {
    switch (data_type) {
        case DataType::VECTOR_FLOAT: {
            AssertInfo(
                data->type()->id() == arrow::Type::type::FIXED_SIZE_BINARY,
                "inconsistent data type: {}",
                data->type_id());
            auto array =
                std::dynamic_pointer_cast<arrow::FixedSizeBinaryArray>(data);
            return array->byte_width() / sizeof(float);
        }
        case DataType::VECTOR_BINARY: {
            AssertInfo(
                data->type()->id() == arrow::Type::type::FIXED_SIZE_BINARY,
                "inconsistent data type: {}",
                data->type_id());
            auto array =
                std::dynamic_pointer_cast<arrow::FixedSizeBinaryArray>(data);
            return array->byte_width() * 8;
        }
        case DataType::VECTOR_FLOAT16: {
            AssertInfo(
                data->type()->id() == arrow::Type::type::FIXED_SIZE_BINARY,
                "inconsistent data type: {}",
                data->type_id());
            auto array =
                std::dynamic_pointer_cast<arrow::FixedSizeBinaryArray>(data);
            return array->byte_width() / sizeof(float16);
        }
        case DataType::VECTOR_BFLOAT16: {
            AssertInfo(
                data->type()->id() == arrow::Type::type::FIXED_SIZE_BINARY,
                "inconsistent data type: {}",
                data->type_id());
            auto array =
                std::dynamic_pointer_cast<arrow::FixedSizeBinaryArray>(data);
            return array->byte_width() / sizeof(bfloat16);
        }
        case DataType::VECTOR_INT8: {
            AssertInfo(
                data->type()->id() == arrow::Type::type::FIXED_SIZE_BINARY,
                "inconsistent data type: {}",
                data->type_id());
            auto array =
                std::dynamic_pointer_cast<arrow::FixedSizeBinaryArray>(data);
            return array->byte_width() / sizeof(int8);
        }
        default:
            PanicInfo(DataTypeInvalid, "unsupported data type {}", data_type);
    }
}

std::string
GenIndexPathIdentifier(int64_t build_id,
                       int64_t index_version,
                       int64_t segment_id,
                       int64_t field_id) {
    return std::to_string(build_id) + "_" + std::to_string(index_version) +
           "_" + std::to_string(segment_id) + "_" + std::to_string(field_id) +
           "/";
}

std::string
GenIndexPathPrefix(ChunkManagerPtr cm,
                   int64_t build_id,
                   int64_t index_version,
                   int64_t segment_id,
                   int64_t field_id,
                   bool is_temp) {
    boost::filesystem::path prefix = cm->GetRootPath();

    if (is_temp) {
        prefix = prefix / TEMP;
    }

    boost::filesystem::path path = std::string(INDEX_ROOT_PATH);
    boost::filesystem::path path1 =
        GenIndexPathIdentifier(build_id, index_version, segment_id, field_id);
    return (prefix / path / path1).string();
}

std::string
GenTextIndexPathPrefix(ChunkManagerPtr cm,
                       int64_t build_id,
                       int64_t index_version,
                       int64_t segment_id,
                       int64_t field_id,
                       bool is_temp) {
    boost::filesystem::path prefix = cm->GetRootPath();

    if (is_temp) {
        prefix = prefix / TEMP;
    }

    boost::filesystem::path path = std::string(TEXT_LOG_ROOT_PATH);
    boost::filesystem::path path1 =
        GenIndexPathIdentifier(build_id, index_version, segment_id, field_id);
    return (prefix / path / path1).string();
}

std::string
GenJsonKeyIndexPathPrefix(ChunkManagerPtr cm,
                          int64_t build_id,
                          int64_t index_version,
                          int64_t segment_id,
                          int64_t field_id,
                          bool is_temp) {
    boost::filesystem::path prefix = cm->GetRootPath();

    if (is_temp) {
        prefix = prefix / TEMP;
    }

    boost::filesystem::path path = std::string(JSON_KEY_INDEX_LOG_ROOT_PATH);
    boost::filesystem::path path1 =
        GenIndexPathIdentifier(build_id, index_version, segment_id, field_id);
    return (prefix / path / path1).string();
}

std::string
GenJsonKeyIndexPathIdentifier(int64_t build_id,
                              int64_t index_version,
                              int64_t collection_id,
                              int64_t partition_id,
                              int64_t segment_id,
                              int64_t field_id) {
    return std::to_string(build_id) + "/" + std::to_string(index_version) +
           "/" + std::to_string(collection_id) + "/" +
           std::to_string(partition_id) + "/" + std::to_string(segment_id) +
           "/" + std::to_string(field_id) + "/";
}

std::string
GenRemoteJsonKeyIndexPathPrefix(ChunkManagerPtr cm,
                                int64_t build_id,
                                int64_t index_version,
                                int64_t collection_id,
                                int64_t partition_id,
                                int64_t segment_id,
                                int64_t field_id) {
    return cm->GetRootPath() + "/" + std::string(JSON_KEY_INDEX_LOG_ROOT_PATH) +
           "/" +
           GenJsonKeyIndexPathIdentifier(build_id,
                                         index_version,
                                         collection_id,
                                         partition_id,
                                         segment_id,
                                         field_id);
}

std::string
GenNgramIndexPrefix(ChunkManagerPtr cm,
                    int64_t build_id,
                    int64_t index_version,
                    int64_t segment_id,
                    int64_t field_id,
                    bool is_temp) {
    boost::filesystem::path prefix = cm->GetRootPath();

    if (is_temp) {
        prefix = prefix / TEMP;
    }

    boost::filesystem::path path = std::string(NGRAM_LOG_ROOT_PATH);
    boost::filesystem::path path1 =
        GenIndexPathIdentifier(build_id, index_version, segment_id, field_id);
    return (prefix / path / path1).string();
}

std::string
GenFieldRawDataPathPrefix(ChunkManagerPtr cm,
                          int64_t segment_id,
                          int64_t field_id) {
    boost::filesystem::path prefix = cm->GetRootPath();
    boost::filesystem::path path = std::string(RAWDATA_ROOT_PATH);
    boost::filesystem::path path1 =
        std::to_string(segment_id) + "/" + std::to_string(field_id) + "/";
    return (prefix / path / path1).string();
}

std::string
GetSegmentRawDataPathPrefix(ChunkManagerPtr cm, int64_t segment_id) {
    boost::filesystem::path prefix = cm->GetRootPath();
    boost::filesystem::path path = std::string(RAWDATA_ROOT_PATH);
    boost::filesystem::path path1 = std::to_string(segment_id);
    return (prefix / path / path1).string();
}

std::pair<std::string, size_t>
EncodeAndUploadIndexSlice(ChunkManager* chunk_manager,
                          uint8_t* buf,
                          int64_t batch_size,
                          IndexMeta index_meta,
                          FieldDataMeta field_meta,
                          std::string object_key) {
    std::shared_ptr<IndexData> index_data = nullptr;
    if (index_meta.index_non_encoding) {
        index_data = std::make_shared<IndexData>(buf, batch_size);
        // index-build tasks assigned from new milvus-coord nodes to none-encoding
    } else {
        auto field_data = CreateFieldData(DataType::INT8, false);
        field_data->FillFieldData(buf, batch_size);
        auto payload_reader = std::make_shared<PayloadReader>(field_data);
        index_data = std::make_shared<IndexData>(payload_reader);
        // index-build tasks assigned from old milvus-coord nodes, fallback to int8 encoding
    }
    // index not use valid_data, so no need to set nullable==true
    index_data->set_index_meta(index_meta);
    index_data->SetFieldDataMeta(field_meta);
    auto serialized_index_data = index_data->serialize_to_remote_file();
    auto serialized_index_size = serialized_index_data.size();
    chunk_manager->Write(
        object_key, serialized_index_data.data(), serialized_index_size);
    return std::make_pair(std::move(object_key), serialized_index_size);
}

std::vector<std::future<std::unique_ptr<DataCodec>>>
GetObjectData(ChunkManager* remote_chunk_manager,
              const std::vector<std::string>& remote_files,
              milvus::ThreadPoolPriority priority,
              bool is_field_data) {
    auto& pool = ThreadPools::GetThreadPool(priority);
    std::vector<std::future<std::unique_ptr<DataCodec>>> futures;
    futures.reserve(remote_files.size());

    auto DownloadAndDeserialize = [&](ChunkManager* chunk_manager,
                                      const std::string& file,
                                      bool is_field_data) {
        // TODO remove this Size() cost
        auto fileSize = chunk_manager->Size(file);
        auto buf = std::shared_ptr<uint8_t[]>(new uint8_t[fileSize]);
        chunk_manager->Read(file, buf.get(), fileSize);
        auto res = DeserializeFileData(buf, fileSize, is_field_data);
        return res;
    };

    for (auto& file : remote_files) {
        futures.emplace_back(pool.Submit(
            DownloadAndDeserialize, remote_chunk_manager, file, is_field_data));
    }
    return futures;
}

std::map<std::string, int64_t>
PutIndexData(ChunkManager* remote_chunk_manager,
             const std::vector<const uint8_t*>& data_slices,
             const std::vector<int64_t>& slice_sizes,
             const std::vector<std::string>& slice_names,
             FieldDataMeta& field_meta,
             IndexMeta& index_meta) {
    auto& pool = ThreadPools::GetThreadPool(milvus::ThreadPoolPriority::MIDDLE);
    std::vector<std::future<std::pair<std::string, size_t>>> futures;
    AssertInfo(data_slices.size() == slice_sizes.size(),
               "inconsistent data slices size {} with slice sizes {}",
               data_slices.size(),
               slice_sizes.size());
    AssertInfo(data_slices.size() == slice_names.size(),
               "inconsistent data slices size {} with slice names size {}",
               data_slices.size(),
               slice_names.size());

    for (int64_t i = 0; i < data_slices.size(); ++i) {
        futures.push_back(pool.Submit(EncodeAndUploadIndexSlice,
                                      remote_chunk_manager,
                                      const_cast<uint8_t*>(data_slices[i]),
                                      slice_sizes[i],
                                      index_meta,
                                      field_meta,
                                      slice_names[i]));
    }

    std::map<std::string, int64_t> remote_paths_to_size;
    std::exception_ptr first_exception = nullptr;
    for (auto& future : futures) {
        try {
            auto res = future.get();
            remote_paths_to_size[res.first] = res.second;
        } catch (...) {
            if (!first_exception) {
                first_exception = std::current_exception();
            }
        }
    }
    ReleaseArrowUnused();
    if (first_exception) {
        std::rethrow_exception(first_exception);
    }

    return remote_paths_to_size;
}

int64_t
GetTotalNumRowsForFieldDatas(const std::vector<FieldDataPtr>& field_datas) {
    int64_t count = 0;
    for (auto& field_data : field_datas) {
        count += field_data->get_num_rows();
    }

    return count;
}

size_t
GetNumRowsForLoadInfo(const LoadFieldDataInfo& load_info) {
    if (load_info.field_infos.empty()) {
        return 0;
    }

    auto& field = load_info.field_infos.begin()->second;
    return field.row_count;
}

void
ReleaseArrowUnused() {
    static std::mutex release_mutex;

    // While multiple threads are releasing memory,
    // we don't need everyone do releasing,
    // just let some of them do this also works well
    if (release_mutex.try_lock()) {
        arrow::default_memory_pool()->ReleaseUnused();
        release_mutex.unlock();
    }
}

ChunkManagerPtr
CreateChunkManager(const StorageConfig& storage_config) {
    auto storage_type = ChunkManagerType_Map[storage_config.storage_type];

    switch (storage_type) {
        case ChunkManagerType::Local: {
            return std::make_shared<LocalChunkManager>(
                storage_config.root_path);
        }
        case ChunkManagerType::Minio: {
            return std::make_shared<MinioChunkManager>(storage_config);
        }
        case ChunkManagerType::Remote: {
            auto cloud_provider_type =
                CloudProviderType_Map[storage_config.cloud_provider];
            switch (cloud_provider_type) {
                case CloudProviderType::AWS: {
                    return std::make_shared<AwsChunkManager>(storage_config);
                }
                case CloudProviderType::GCP: {
                    return std::make_shared<GcpChunkManager>(storage_config);
                }
                case CloudProviderType::ALIYUN: {
                    return std::make_shared<AliyunChunkManager>(storage_config);
                }
                case CloudProviderType::TENCENTCLOUD: {
                    return std::make_shared<TencentCloudChunkManager>(
                        storage_config);
                }
#ifdef AZURE_BUILD_DIR
                case CloudProviderType::AZURE: {
                    return std::make_shared<AzureChunkManager>(storage_config);
                }
#endif
#ifdef ENABLE_GCP_NATIVE
                case CloudProviderType::GCPNATIVE: {
                    return std::make_shared<GcpNativeChunkManager>(
                        storage_config);
                }
#endif
                default: {
                    return std::make_shared<MinioChunkManager>(storage_config);
                }
            }
        }
#ifdef USE_OPENDAL
        case ChunkManagerType::OpenDAL: {
            return std::make_shared<OpenDALChunkManager>(storage_config);
        }
#endif
        default: {
            PanicInfo(ConfigInvalid,
                      "unsupported storage_config.storage_type {}",
                      fmt::underlying(storage_type));
        }
    }
}

milvus_storage::ArrowFileSystemPtr
InitArrowFileSystem(milvus::storage::StorageConfig storage_config) {
    if (storage_config.storage_type == "local") {
        std::string path(storage_config.root_path);
        milvus_storage::ArrowFileSystemConfig conf;
        conf.root_path = path;
        conf.storage_type = "local";
        milvus_storage::ArrowFileSystemSingleton::GetInstance().Init(conf);
    } else {
        milvus_storage::ArrowFileSystemConfig conf;
        conf.address = std::string(storage_config.address);
        conf.bucket_name = std::string(storage_config.bucket_name);
        conf.access_key_id = std::string(storage_config.access_key_id);
        conf.access_key_value = std::string(storage_config.access_key_value);
        conf.root_path = std::string(storage_config.root_path);
        conf.storage_type = std::string(storage_config.storage_type);
        conf.cloud_provider = std::string(storage_config.cloud_provider);
        conf.iam_endpoint = std::string(storage_config.iam_endpoint);
        conf.log_level = std::string(storage_config.log_level);
        conf.region = std::string(storage_config.region);
        conf.useSSL = storage_config.useSSL;
        conf.sslCACert = std::string(storage_config.sslCACert);
        conf.useIAM = storage_config.useIAM;
        conf.useVirtualHost = storage_config.useVirtualHost;
        conf.requestTimeoutMs = storage_config.requestTimeoutMs;
        conf.gcp_credential_json =
            std::string(storage_config.gcp_credential_json);
        conf.use_custom_part_upload = true;
        milvus_storage::ArrowFileSystemSingleton::GetInstance().Init(conf);
    }
    return milvus_storage::ArrowFileSystemSingleton::GetInstance()
        .GetArrowFileSystem();
}

FieldDataPtr
CreateFieldData(const DataType& type,
                bool nullable,
                int64_t dim,
                int64_t total_num_rows) {
    switch (type) {
        case DataType::BOOL:
            return std::make_shared<FieldData<bool>>(
                type, nullable, total_num_rows);
        case DataType::INT8:
            return std::make_shared<FieldData<int8_t>>(
                type, nullable, total_num_rows);
        case DataType::INT16:
            return std::make_shared<FieldData<int16_t>>(
                type, nullable, total_num_rows);
        case DataType::INT32:
            return std::make_shared<FieldData<int32_t>>(
                type, nullable, total_num_rows);
        case DataType::INT64:
            return std::make_shared<FieldData<int64_t>>(
                type, nullable, total_num_rows);
        case DataType::FLOAT:
            return std::make_shared<FieldData<float>>(
                type, nullable, total_num_rows);
        case DataType::DOUBLE:
            return std::make_shared<FieldData<double>>(
                type, nullable, total_num_rows);
        case DataType::STRING:
        case DataType::VARCHAR:
        case DataType::TEXT:
            return std::make_shared<FieldData<std::string>>(
                type, nullable, total_num_rows);
        case DataType::JSON:
            return std::make_shared<FieldData<Json>>(
                type, nullable, total_num_rows);
        case DataType::ARRAY:
            return std::make_shared<FieldData<Array>>(
                type, nullable, total_num_rows);
        case DataType::VECTOR_FLOAT:
            return std::make_shared<FieldData<FloatVector>>(
                dim, type, total_num_rows);
        case DataType::VECTOR_BINARY:
            return std::make_shared<FieldData<BinaryVector>>(
                dim, type, total_num_rows);
        case DataType::VECTOR_FLOAT16:
            return std::make_shared<FieldData<Float16Vector>>(
                dim, type, total_num_rows);
        case DataType::VECTOR_BFLOAT16:
            return std::make_shared<FieldData<BFloat16Vector>>(
                dim, type, total_num_rows);
        case DataType::VECTOR_SPARSE_FLOAT:
            return std::make_shared<FieldData<SparseFloatVector>>(
                type, total_num_rows);
        case DataType::VECTOR_INT8:
            return std::make_shared<FieldData<Int8Vector>>(
                dim, type, total_num_rows);
        case DataType::VECTOR_ARRAY:
            return std::make_shared<FieldData<VectorArray>>(type,
                                                            total_num_rows);
        default:
            PanicInfo(DataTypeInvalid,
                      "CreateFieldData not support data type " +
                          GetDataTypeName(type));
    }
}

int64_t
GetByteSizeOfFieldDatas(const std::vector<FieldDataPtr>& field_datas) {
    int64_t result = 0;
    for (auto& data : field_datas) {
        result += data->Size();
    }

    return result;
}

std::vector<FieldDataPtr>
CollectFieldDataChannel(FieldDataChannelPtr& channel) {
    std::vector<FieldDataPtr> result;
    FieldDataPtr field_data;
    while (channel->pop(field_data)) {
        result.push_back(field_data);
    }
    return result;
}

FieldDataPtr
MergeFieldData(std::vector<FieldDataPtr>& data_array) {
    if (data_array.size() == 0) {
        return nullptr;
    }

    if (data_array.size() == 1) {
        return data_array[0];
    }

    size_t total_length = 0;
    for (const auto& data : data_array) {
        total_length += data->Length();
    }
    auto merged_data = storage::CreateFieldData(data_array[0]->get_data_type(),
                                                data_array[0]->IsNullable());
    merged_data->Reserve(total_length);
    for (const auto& data : data_array) {
        if (merged_data->IsNullable()) {
            merged_data->FillFieldData(
                data->Data(), data->ValidData(), data->Length(), 0);
        } else {
            merged_data->FillFieldData(data->Data(), data->Length());
        }
    }
    return merged_data;
}

std::vector<FieldDataPtr>
FetchFieldData(ChunkManager* cm, const std::vector<std::string>& remote_files) {
    std::vector<FieldDataPtr> field_datas;
    std::vector<std::string> batch_files;
    auto FetchRawData = [&]() {
        auto fds = GetObjectData(cm, batch_files);
        for (size_t i = 0; i < batch_files.size(); ++i) {
            auto data = fds[i].get()->GetFieldData();
            field_datas.emplace_back(data);
        }
    };

    auto parallel_degree =
        uint64_t(DEFAULT_FIELD_MAX_MEMORY_LIMIT / FILE_SLICE_SIZE);

    for (auto& file : remote_files) {
        if (batch_files.size() >= parallel_degree) {
            FetchRawData();
            batch_files.clear();
        }
        batch_files.emplace_back(file);
    }
    if (batch_files.size() > 0) {
        FetchRawData();
    }
    return field_datas;
}

std::vector<FieldDataPtr>
GetFieldDatasFromStorageV2(std::vector<std::vector<std::string>>& remote_files,
                           int64_t field_id,
                           DataType data_type,
                           int64_t dim,
                           milvus_storage::ArrowFileSystemPtr fs) {
    AssertInfo(remote_files.size() > 0, "remote files size is 0");
    std::vector<FieldDataPtr> field_data_list;

    // remote files might not followed the sequence of column group id,
    // so we need to put into map<column_group_id, remote_chunk_files>
    std::unordered_map<int64_t, std::vector<std::string>> column_group_files;
    for (auto& remote_chunk_files : remote_files) {
        AssertInfo(remote_chunk_files.size() > 0, "remote files size is 0");
        int64_t group_id = ExtractGroupIdFromPath(remote_chunk_files[0]);
        column_group_files[group_id] = remote_chunk_files;
    }

    std::vector<std::string> remote_chunk_files;
    int64_t column_group_id;
    if (column_group_files.find(field_id) == column_group_files.end()) {
        column_group_id = DEFAULT_SHORT_COLUMN_GROUP_ID;
        remote_chunk_files = column_group_files[DEFAULT_SHORT_COLUMN_GROUP_ID];
    } else {
        remote_chunk_files = column_group_files[field_id];
        column_group_id = field_id;
    }

    AssertInfo(remote_chunk_files.size() > 0, "remote files size is 0");

    // find column offset
    milvus_storage::FieldIDList field_id_list = storage::GetFieldIDList(
        FieldId(column_group_id), remote_chunk_files[0], nullptr, fs);
    size_t col_offset = -1;
    for (size_t i = 0; i < field_id_list.size(); ++i) {
        if (field_id_list.Get(i) == field_id) {
            col_offset = i;
            break;
        }
    }
    // field not found, must be newly added field, return empty resultset
    if (col_offset == -1) {
        LOG_INFO(
            "field {} not found in column group {}, return empty result set",
            field_id,
            column_group_id);
        return field_data_list;
    }

    AssertInfo(fs != nullptr,
               "storage v2 arrow file system is not initialized");

    // set up channel for arrow reader
    auto field_data_info = FieldDataInfo();
    auto parallel_degree =
        static_cast<uint64_t>(DEFAULT_FIELD_MAX_MEMORY_LIMIT / FILE_SLICE_SIZE);
    field_data_info.arrow_reader_channel->set_capacity(parallel_degree);

    auto& pool = ThreadPools::GetThreadPool(milvus::ThreadPoolPriority::MIDDLE);

    for (auto& column_group_file : remote_chunk_files) {
        // get all row groups for each file
        std::vector<std::vector<int64_t>> row_group_lists;
        auto reader = std::make_shared<milvus_storage::FileRowGroupReader>(
            fs, column_group_file);

        auto row_group_num =
            reader->file_metadata()->GetRowGroupMetadataVector().size();
        std::vector<int64_t> all_row_groups(row_group_num);
        std::iota(all_row_groups.begin(), all_row_groups.end(), 0);
        row_group_lists.push_back(all_row_groups);

        // create a schema with only the field id
        auto field_schema = reader->schema()->field(col_offset)->Copy();
        auto arrow_schema = arrow::schema({field_schema});
        auto status = reader->Close();
        AssertInfo(
            status.ok(),
            "failed to close file reader when get arrow schema from file: " +
                column_group_file + " with error: " + status.ToString());

        // split row groups for parallel reading
        auto strategy = std::make_unique<segcore::ParallelDegreeSplitStrategy>(
            parallel_degree);
        auto load_future = pool.Submit([&]() {
            return LoadWithStrategy(std::vector<std::string>{column_group_file},
                                    field_data_info.arrow_reader_channel,
                                    DEFAULT_FIELD_MAX_MEMORY_LIMIT,
                                    std::move(strategy),
                                    row_group_lists,
                                    nullptr);
        });
        // read field data from channel
        std::shared_ptr<milvus::ArrowDataWrapper> r;
        while (field_data_info.arrow_reader_channel->pop(r)) {
            size_t num_rows = 0;
            std::vector<std::shared_ptr<arrow::ChunkedArray>> chunked_arrays;
            for (const auto& table : r->arrow_tables) {
                num_rows += table->num_rows();
                chunked_arrays.push_back(table->column(col_offset));
            }
            auto field_data = storage::CreateFieldData(
                data_type, field_schema->nullable(), dim, num_rows);
            for (const auto& chunked_array : chunked_arrays) {
                field_data->FillFieldData(chunked_array);
            }
            field_data_list.push_back(field_data);
        }
    }
    return field_data_list;
}

std::vector<FieldDataPtr>
CacheRawDataAndFillMissing(const MemFileManagerImplPtr& file_manager,
                           const Config& config) {
    // download field data
    auto field_datas = file_manager->CacheRawDataToMemory(config);

    // check storage version
    auto storage_version =
        index::GetValueFromConfig<int64_t>(config, STORAGE_VERSION_KEY)
            .value_or(0);

    int64_t lack_binlog_rows =
        index::GetValueFromConfig<int64_t>(config, INDEX_NUM_ROWS_KEY)
            .value_or(0);
    for (auto& field_data : field_datas) {
        lack_binlog_rows -= field_data->get_num_rows();
    }

    if (lack_binlog_rows > 0) {
        LOG_INFO("create index lack binlog detected, lack row num: {}",
                 lack_binlog_rows);
        auto field_schema = file_manager->GetFieldDataMeta().field_schema;
        auto default_value = [&]() -> std::optional<DefaultValueType> {
            if (!field_schema.has_default_value()) {
                return std::nullopt;
            }
            return field_schema.default_value();
        }();
        auto field_data = storage::CreateFieldData(
            static_cast<DataType>(field_schema.data_type()),
            true,
            1,
            lack_binlog_rows);
        field_data->FillFieldData(default_value, lack_binlog_rows);
        field_datas.insert(field_datas.begin(), field_data);
    }

    return field_datas;
}

int64_t
ExtractGroupIdFromPath(const std::string& path) {
    // find second last of / to get group_id
    size_t last_slash = path.find_last_of("/");
    size_t second_last_slash = path.find_last_of("/", last_slash - 1);
    return std::stol(
        path.substr(second_last_slash + 1, last_slash - second_last_slash - 1));
}

// if it is multi-field column group, read field id list from file metadata
// if it is single-field column group, return the column group id
milvus_storage::FieldIDList
GetFieldIDList(FieldId column_group_id,
               const std::string& filepath,
               const std::shared_ptr<arrow::Schema>& arrow_schema,
               milvus_storage::ArrowFileSystemPtr fs) {
    milvus_storage::FieldIDList field_id_list;
    if (column_group_id >= FieldId(START_USER_FIELDID)) {
        field_id_list.Add(column_group_id.get());
        return field_id_list;
    }
    auto file_reader = std::make_shared<milvus_storage::FileRowGroupReader>(
        fs, filepath, arrow_schema);
    field_id_list =
        file_reader->file_metadata()->GetGroupFieldIDList().GetFieldIDList(
            column_group_id.get());
    auto status = file_reader->Close();
    AssertInfo(status.ok(),
               "failed to close file reader when get field id list from {}",
               filepath);
    return field_id_list;
}

}  // namespace milvus::storage
