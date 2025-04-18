// Copyright (C) 2019-2020 Zilliz. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied. See the License for the specific language governing permissions and limitations under the License

#pragma once

#include <tbb/concurrent_priority_queue.h>
#include <tbb/concurrent_vector.h>

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ConcurrentVector.h"
#include "DeletedRecord.h"
#include "SealedIndexingRecord.h"
#include "SegmentSealed.h"
#include "TimestampIndex.h"
#include "common/EasyAssert.h"
#include "google/protobuf/message_lite.h"
#include "mmap/ChunkedColumn.h"
#include "index/ScalarIndex.h"
#include "sys/mman.h"
#include "common/Types.h"
#include "common/IndexMeta.h"

namespace milvus::segcore {

class ChunkedSegmentSealedImpl : public SegmentSealed {
 public:
    explicit ChunkedSegmentSealedImpl(SchemaPtr schema,
                                      IndexMetaPtr index_meta,
                                      const SegcoreConfig& segcore_config,
                                      int64_t segment_id,
                                      bool TEST_skip_index_for_retrieve = false,
                                      bool is_sorted_by_pk = false);
    ~ChunkedSegmentSealedImpl() override;
    void
    LoadIndex(const LoadIndexInfo& info) override;
    void
    LoadFieldData(const LoadFieldDataInfo& info) override;
    void
    LoadDeletedRecord(const LoadDeletedRecordInfo& info) override;
    void
    LoadSegmentMeta(
        const milvus::proto::segcore::LoadSegmentMeta& segment_meta) override;
    void
    DropIndex(const FieldId field_id) override;
    void
    DropFieldData(const FieldId field_id) override;
    bool
    HasIndex(FieldId field_id) const override;
    bool
    HasFieldData(FieldId field_id) const override;

    bool
    Contain(const PkType& pk) const override {
        return insert_record_.contain(pk);
    }

    void
    LoadFieldData(FieldId field_id, FieldDataInfo& data) override;
    void
    MapFieldData(const FieldId field_id, FieldDataInfo& data) override;
    void
    AddFieldDataInfoForSealed(
        const LoadFieldDataInfo& field_data_info) override;

    int64_t
    get_segment_id() const override {
        return id_;
    }

    bool
    HasRawData(int64_t field_id) const override;

    DataType
    GetFieldDataType(FieldId fieldId) const override;

    void
    RemoveFieldFile(const FieldId field_id) override;

    void
    CreateTextIndex(FieldId field_id) override;

    void
    LoadTextIndex(FieldId field_id,
                  std::unique_ptr<index::TextMatchIndex> index) override;
    void
    LoadJsonKeyIndex(
        FieldId field_id,
        std::unique_ptr<index::JsonKeyStatsInvertedIndex> index) override {
        std::unique_lock lck(mutex_);
        const auto& field_meta = schema_->operator[](field_id);
        json_key_indexes_[field_id] = std::move(index);
    }

    index::JsonKeyStatsInvertedIndex*
    GetJsonKeyIndex(FieldId field_id) const override {
        std::shared_lock lck(mutex_);
        auto iter = json_key_indexes_.find(field_id);
        if (iter == json_key_indexes_.end()) {
            return nullptr;
        }
        return iter->second.get();
    }

    std::pair<std::string_view, bool>
    GetJsonData(FieldId field_id, size_t offset) const override {
        auto column = fields_.at(field_id);
        bool is_valid = column->IsValid(offset);
        return std::make_pair(std::move(column->RawAt(offset)), is_valid);
    }

 public:
    size_t
    GetMemoryUsageInBytes() const override {
        return stats_.mem_size.load() + deleted_record_.mem_size();
    }

    InsertRecord<true>&
    get_insert_record() override {
        return insert_record_;
    }

    int64_t
    get_row_count() const override;

    int64_t
    get_deleted_count() const override;

    const Schema&
    get_schema() const override;

    std::vector<SegOffset>
    search_pk(const PkType& pk, Timestamp timestamp) const override;

    std::vector<SegOffset>
    search_pk(const PkType& pk, int64_t insert_barrier) const override;

    template <typename Condition>
    std::vector<SegOffset>
    search_sorted_pk(const PkType& pk, Condition condition) const;

    std::unique_ptr<DataArray>
    get_vector(FieldId field_id,
               const int64_t* ids,
               int64_t count) const override;

    bool
    is_nullable(FieldId field_id) const override {
        auto it = fields_.find(field_id);
        AssertInfo(it != fields_.end(),
                   "Cannot find field with field_id: " +
                       std::to_string(field_id.get()));
        return it->second->IsNullable();
    };

    bool
    is_chunked() const override {
        return true;
    }

 public:
    int64_t
    num_chunk_index(FieldId field_id) const override;

    // count of chunk that has raw data
    int64_t
    num_chunk_data(FieldId field_id) const override;

    int64_t
    num_chunk(FieldId field_id) const override;

    // return size_per_chunk for each chunk, renaming against confusion
    int64_t
    size_per_chunk() const override;

    int64_t
    chunk_size(FieldId field_id, int64_t chunk_id) const override;

    std::pair<int64_t, int64_t>
    get_chunk_by_offset(FieldId field_id, int64_t offset) const override;

    int64_t
    num_rows_until_chunk(FieldId field_id, int64_t chunk_id) const override;

    std::string
    debug() const override;

    SegcoreError
    Delete(int64_t reserved_offset,
           int64_t size,
           const IdArray* pks,
           const Timestamp* timestamps) override;

    std::pair<std::vector<OffsetMap::OffsetType>, bool>
    find_first(int64_t limit, const BitsetType& bitset) const override;

    // Calculate: output[i] = Vec[seg_offset[i]]
    // where Vec is determined from field_offset
    std::unique_ptr<DataArray>
    bulk_subscript(FieldId field_id,
                   const int64_t* seg_offsets,
                   int64_t count) const override;

    std::unique_ptr<DataArray>
    bulk_subscript(
        FieldId field_id,
        const int64_t* seg_offsets,
        int64_t count,
        const std::vector<std::string>& dynamic_field_names) const override;

    bool
    is_mmap_field(FieldId id) const override;

    void
    ClearData() override;

    bool
    is_field_exist(FieldId field_id) const override {
        return schema_->get_fields().find(field_id) !=
               schema_->get_fields().end();
    }

 protected:
    // blob and row_count
    SpanBase
    chunk_data_impl(FieldId field_id, int64_t chunk_id) const override;

    std::pair<std::vector<std::string_view>, FixedVector<bool>>
    chunk_string_view_impl(
        FieldId field_id,
        int64_t chunk_id,
        std::optional<std::pair<int64_t, int64_t>> offset_len) const override;

    std::pair<std::vector<ArrayView>, FixedVector<bool>>
    chunk_array_view_impl(
        FieldId field_id,
        int64_t chunk_id,
        std::optional<std::pair<int64_t, int64_t>> offset_len) const override;

    std::pair<std::vector<std::string_view>, FixedVector<bool>>
    chunk_view_by_offsets(FieldId field_id,
                          int64_t chunk_id,
                          const FixedVector<int32_t>& offsets) const override;

    std::pair<BufferView, FixedVector<bool>>
    get_chunk_buffer(FieldId field_id,
                     int64_t chunk_id,
                     int64_t start_offset,
                     int64_t length) const override;

    const index::IndexBase*
    chunk_index_impl(FieldId field_id, int64_t chunk_id) const override;

    // Calculate: output[i] = Vec[seg_offset[i]],
    // where Vec is determined from field_offset
    void
    bulk_subscript(SystemFieldType system_type,
                   const int64_t* seg_offsets,
                   int64_t count,
                   void* output) const override;

    void
    check_search(const query::Plan* plan) const override;

    int64_t
    get_active_count(Timestamp ts) const override;

    const ConcurrentVector<Timestamp>&
    get_timestamps() const override {
        return insert_record_.timestamps_;
    }

 private:
    template <typename S, typename T = S>
    static void
    bulk_subscript_impl(const void* src_raw,
                        const int64_t* seg_offsets,
                        int64_t count,
                        T* dst_raw);

    template <typename S, typename T = S>
    static void
    bulk_subscript_impl(const ChunkedColumnBase* field,
                        const int64_t* seg_offsets,
                        int64_t count,
                        T* dst_raw);

    template <typename S, typename T = S>
    static void
    bulk_subscript_impl(const ChunkedColumnBase* field,
                        const int64_t* seg_offsets,
                        int64_t count,
                        void* dst_raw);

    template <typename S, typename T = S>
    static void
    bulk_subscript_ptr_impl(const ChunkedColumnBase* field,
                            const int64_t* seg_offsets,
                            int64_t count,
                            google::protobuf::RepeatedPtrField<T>* dst_raw);

    template <typename T>
    static void
    bulk_subscript_array_impl(const ChunkedColumnBase* column,
                              const int64_t* seg_offsets,
                              int64_t count,
                              google::protobuf::RepeatedPtrField<T>* dst);

    static void
    bulk_subscript_impl(int64_t element_sizeof,
                        const ChunkedColumnBase* field,
                        const int64_t* seg_offsets,
                        int64_t count,
                        void* dst_raw);

    std::unique_ptr<DataArray>
    fill_with_empty(FieldId field_id, int64_t count) const;

    std::unique_ptr<DataArray>
    get_raw_data(FieldId field_id,
                 const FieldMeta& field_meta,
                 const int64_t* seg_offsets,
                 int64_t count) const;

    void
    update_row_count(int64_t row_count) {
        // if (row_count_opt_.has_value()) {
        //     AssertInfo(row_count_opt_.value() == row_count, "load data has different row count from other columns");
        // } else {
        num_rows_ = row_count;
        // }
        deleted_record_.set_sealed_row_count(row_count);
    }

    void
    mask_with_timestamps(BitsetTypeView& bitset_chunk,
                         Timestamp timestamp) const override;

    void
    vector_search(SearchInfo& search_info,
                  const void* query_data,
                  int64_t query_count,
                  Timestamp timestamp,
                  const BitsetView& bitset,
                  SearchResult& output) const override;

    void
    mask_with_delete(BitsetTypeView& bitset,
                     int64_t ins_barrier,
                     Timestamp timestamp) const override;

    bool
    is_system_field_ready() const {
        return system_ready_count_ == 2;
    }

    std::pair<std::unique_ptr<IdArray>, std::vector<SegOffset>>
    search_ids(const IdArray& id_array, Timestamp timestamp) const override;

    std::tuple<std::string, int64_t>
    GetFieldDataPath(FieldId field_id, int64_t offset) const;

    void
    LoadVecIndex(const LoadIndexInfo& info);

    void
    LoadScalarIndex(const LoadIndexInfo& info);

    void
    WarmupChunkCache(const FieldId field_id, bool mmap_enabled) override;

    bool
    generate_interim_index(const FieldId field_id);

 private:
    // mmap descriptor, used in chunk cache
    storage::MmapChunkDescriptorPtr mmap_descriptor_ = nullptr;
    // segment loading state
    BitsetType field_data_ready_bitset_;
    BitsetType index_ready_bitset_;
    BitsetType binlog_index_bitset_;
    std::atomic<int> system_ready_count_ = 0;
    // segment data

    // TODO: generate index for scalar
    std::optional<int64_t> num_rows_;

    // scalar field index
    std::unordered_map<FieldId, index::IndexBasePtr> scalar_indexings_;
    // vector field index
    SealedIndexingRecord vector_indexings_;

    // inserted fields data and row_ids, timestamps
    InsertRecord<true> insert_record_;

    // deleted pks
    mutable DeletedRecord<true> deleted_record_;

    LoadFieldDataInfo field_data_info_;

    SchemaPtr schema_;
    int64_t id_;
    std::unordered_map<FieldId, std::shared_ptr<ChunkedColumnBase>> fields_;
    std::unordered_set<FieldId> mmap_fields_;

    // only useful in binlog
    IndexMetaPtr col_index_meta_;
    SegcoreConfig segcore_config_;
    std::unordered_map<FieldId, std::unique_ptr<VecIndexConfig>>
        vec_binlog_config_;

    SegmentStats stats_{};

    // for sparse vector unit test only! Once a type of sparse index that
    // doesn't has raw data is added, this should be removed.
    bool TEST_skip_index_for_retrieve_ = false;

    // whether the segment is sorted by the pk
    bool is_sorted_by_pk_ = false;
    // used for json expr optimization
    std::unordered_map<FieldId,
                       std::unique_ptr<index::JsonKeyStatsInvertedIndex>>
        json_key_indexes_;
};

inline SegmentSealedUPtr
CreateSealedSegment(
    SchemaPtr schema,
    IndexMetaPtr index_meta = nullptr,
    int64_t segment_id = 0,
    const SegcoreConfig& segcore_config = SegcoreConfig::default_config(),
    bool TEST_skip_index_for_retrieve = false,
    bool is_sorted_by_pk = false) {
    return std::make_unique<ChunkedSegmentSealedImpl>(
        schema,
        index_meta,
        segcore_config,
        segment_id,
        TEST_skip_index_for_retrieve,
        is_sorted_by_pk);
}
}  // namespace milvus::segcore
