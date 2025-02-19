#include "duckdb/common/types/null_value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector_size.hpp"
#include "duckdb/function/compression_function.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/checkpoint/string_checkpoint_state.hpp"
#include "duckdb/storage/segment/uncompressed.hpp"
#include "duckdb/storage/statistics/string_statistics.hpp"
#include "duckdb/storage/table/append_state.hpp"
#include "duckdb/storage/table/column_segment.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Storage Class
//===--------------------------------------------------------------------===//
UncompressedStringSegmentState::~UncompressedStringSegmentState() {
	while (head) {
		// prevent deep recursion here
		head = move(head->next);
	}
}

struct StringDictionaryContainer {
	//! The size of the dictionary
	uint32_t size;
	//! The end of the dictionary (typically Storage::BLOCK_SIZE)
	uint32_t end;

	void Verify() {
		D_ASSERT(size <= Storage::BLOCK_SIZE);
		D_ASSERT(end <= Storage::BLOCK_SIZE);
		D_ASSERT(size <= end);
	}
};

struct UncompressedStringStorage {
public:
	//! Dictionary header size at the beginning of the string segment (offset + length)
	static constexpr uint16_t DICTIONARY_HEADER_SIZE = sizeof(uint32_t) + sizeof(uint32_t);
	//! Marker used in length field to indicate the presence of a big string
	static constexpr uint16_t BIG_STRING_MARKER = (uint16_t)-1;
	//! Base size of big string marker (block id + offset)
	static constexpr idx_t BIG_STRING_MARKER_BASE_SIZE = sizeof(block_id_t) + sizeof(int32_t);
	//! The marker size of the big string
	static constexpr idx_t BIG_STRING_MARKER_SIZE = BIG_STRING_MARKER_BASE_SIZE + sizeof(uint16_t);

public:
	static unique_ptr<AnalyzeState> StringInitAnalyze(ColumnData &col_data, PhysicalType type);
	static bool StringAnalyze(AnalyzeState &state_p, Vector &input, idx_t count);
	static idx_t StringFinalAnalyze(AnalyzeState &state_p);
	static unique_ptr<SegmentScanState> StringInitScan(ColumnSegment &segment);
	static void StringScanPartial(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result,
	                              idx_t result_offset);
	static void StringScan(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result);
	static void StringFetchRow(ColumnSegment &segment, ColumnFetchState &state, row_t row_id, Vector &result,
	                           idx_t result_idx);
	static unique_ptr<CompressedSegmentState> StringInitSegment(ColumnSegment &segment, block_id_t block_id);
	static idx_t StringAppend(ColumnSegment &segment, SegmentStatistics &stats, VectorData &data, idx_t offset,
	                          idx_t count);
	static idx_t FinalizeAppend(ColumnSegment &segment, SegmentStatistics &stats);

public:
	static inline void UpdateStringStats(SegmentStatistics &stats, const string_t &new_value) {
		auto &sstats = (StringStatistics &)*stats.statistics;
		sstats.Update(new_value);
	}

	static void SetDictionary(ColumnSegment &segment, BufferHandle &handle, StringDictionaryContainer dict);
	static StringDictionaryContainer GetDictionary(ColumnSegment &segment, BufferHandle &handle);
	static idx_t RemainingSpace(ColumnSegment &segment, BufferHandle &handle);
	static void WriteString(ColumnSegment &segment, string_t string, block_id_t &result_block, int32_t &result_offset);
	static void WriteStringMemory(ColumnSegment &segment, string_t string, block_id_t &result_block,
	                              int32_t &result_offset);
	static string_t ReadString(ColumnSegment &segment, Vector &result, block_id_t block, int32_t offset);
	static string_t ReadString(data_ptr_t target, int32_t offset);
	static void WriteStringMarker(data_ptr_t target, block_id_t block_id, int32_t offset);
	static void ReadStringMarker(data_ptr_t target, block_id_t &block_id, int32_t &offset);

	static string_location_t FetchStringLocation(StringDictionaryContainer dict, data_ptr_t baseptr,
	                                             int32_t dict_offset);
	static string_t FetchStringFromDict(ColumnSegment &segment, StringDictionaryContainer dict, Vector &result,
	                                    data_ptr_t baseptr, int32_t dict_offset);
	static string_t FetchString(ColumnSegment &segment, StringDictionaryContainer dict, Vector &result,
	                            data_ptr_t baseptr, string_location_t location);
};

//===--------------------------------------------------------------------===//
// Analyze
//===--------------------------------------------------------------------===//
struct StringAnalyzeState : public AnalyzeState {
	StringAnalyzeState() : count(0), total_string_size(0), overflow_strings(0) {
	}

	idx_t count;
	idx_t total_string_size;
	idx_t overflow_strings;
};

unique_ptr<AnalyzeState> UncompressedStringStorage::StringInitAnalyze(ColumnData &col_data, PhysicalType type) {
	return make_unique<StringAnalyzeState>();
}

bool UncompressedStringStorage::StringAnalyze(AnalyzeState &state_p, Vector &input, idx_t count) {
	auto &state = (StringAnalyzeState &)state_p;
	VectorData vdata;
	input.Orrify(count, vdata);

	state.count += count;
	auto data = (string_t *)vdata.data;
	for (idx_t i = 0; i < count; i++) {
		auto idx = vdata.sel->get_index(i);
		if (vdata.validity.RowIsValid(idx)) {
			auto string_size = data[idx].GetSize();
			state.total_string_size += string_size;
			if (string_size >= StringUncompressed::STRING_BLOCK_LIMIT) {
				state.overflow_strings++;
			}
		}
	}
	return true;
}

idx_t UncompressedStringStorage::StringFinalAnalyze(AnalyzeState &state_p) {
	auto &state = (StringAnalyzeState &)state_p;
	return state.count * sizeof(int32_t) + state.total_string_size + state.overflow_strings * BIG_STRING_MARKER_SIZE;
}

//===--------------------------------------------------------------------===//
// Scan
//===--------------------------------------------------------------------===//
struct StringScanState : public SegmentScanState {
	unique_ptr<BufferHandle> handle;
};

unique_ptr<SegmentScanState> UncompressedStringStorage::StringInitScan(ColumnSegment &segment) {
	auto result = make_unique<StringScanState>();
	auto &buffer_manager = BufferManager::GetBufferManager(segment.db);
	result->handle = buffer_manager.Pin(segment.block);
	return move(result);
}

//===--------------------------------------------------------------------===//
// Scan base data
//===--------------------------------------------------------------------===//
void UncompressedStringStorage::StringScanPartial(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count,
                                                  Vector &result, idx_t result_offset) {
	// clear any previously locked buffers and get the primary buffer handle
	auto &scan_state = (StringScanState &)*state.scan_state;
	auto start = segment.GetRelativeIndex(state.row_index);

	auto baseptr = scan_state.handle->node->buffer + segment.GetBlockOffset();
	auto dict = GetDictionary(segment, *scan_state.handle);
	auto base_data = (int32_t *)(baseptr + DICTIONARY_HEADER_SIZE);
	auto result_data = FlatVector::GetData<string_t>(result);

	for (idx_t i = 0; i < scan_count; i++) {
		result_data[result_offset + i] = FetchStringFromDict(segment, dict, result, baseptr, base_data[start + i]);
	}
}

void UncompressedStringStorage::StringScan(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count,
                                           Vector &result) {
	StringScanPartial(segment, state, scan_count, result, 0);
}

//===--------------------------------------------------------------------===//
// Fetch
//===--------------------------------------------------------------------===//
void UncompressedStringStorage::StringFetchRow(ColumnSegment &segment, ColumnFetchState &state, row_t row_id,
                                               Vector &result, idx_t result_idx) {
	// fetch a single row from the string segment
	// first pin the main buffer if it is not already pinned
	auto primary_id = segment.block->BlockId();

	BufferHandle *handle_ptr;
	auto entry = state.handles.find(primary_id);
	if (entry == state.handles.end()) {
		// not pinned yet: pin it
		auto &buffer_manager = BufferManager::GetBufferManager(segment.db);
		auto handle = buffer_manager.Pin(segment.block);
		handle_ptr = handle.get();
		state.handles[primary_id] = move(handle);
	} else {
		// already pinned: use the pinned handle
		handle_ptr = entry->second.get();
	}
	auto baseptr = handle_ptr->node->buffer + segment.GetBlockOffset();
	auto dict = GetDictionary(segment, *handle_ptr);
	auto base_data = (int32_t *)(baseptr + DICTIONARY_HEADER_SIZE);
	auto result_data = FlatVector::GetData<string_t>(result);

	result_data[result_idx] = FetchStringFromDict(segment, dict, result, baseptr, base_data[row_id]);
}

//===--------------------------------------------------------------------===//
// Append
//===--------------------------------------------------------------------===//
unique_ptr<CompressedSegmentState> UncompressedStringStorage::StringInitSegment(ColumnSegment &segment,
                                                                                block_id_t block_id) {
	auto &buffer_manager = BufferManager::GetBufferManager(segment.db);
	if (block_id == INVALID_BLOCK) {
		auto handle = buffer_manager.Pin(segment.block);
		StringDictionaryContainer dictionary;
		dictionary.size = 0;
		dictionary.end = Storage::BLOCK_SIZE;
		SetDictionary(segment, *handle, dictionary);
	}
	return make_unique<UncompressedStringSegmentState>();
}

idx_t UncompressedStringStorage::StringAppend(ColumnSegment &segment, SegmentStatistics &stats, VectorData &data,
                                              idx_t offset, idx_t count) {
	auto &buffer_manager = BufferManager::GetBufferManager(segment.db);
	auto handle = buffer_manager.Pin(segment.block);

	D_ASSERT(segment.GetBlockOffset() == 0);
	auto source_data = (string_t *)data.data;
	auto result_data = (int32_t *)(handle->node->buffer + DICTIONARY_HEADER_SIZE);
	for (idx_t i = 0; i < count; i++) {
		auto source_idx = data.sel->get_index(offset + i);
		auto target_idx = segment.count.load();
		idx_t remaining_space = RemainingSpace(segment, *handle);
		if (remaining_space < sizeof(int32_t)) {
			// string index does not fit in the block at all
			return i;
		}
		remaining_space -= sizeof(int32_t);
		if (!data.validity.RowIsValid(source_idx)) {
			// null value is stored as -1
			result_data[target_idx] = 0;
		} else {
			auto dictionary = GetDictionary(segment, *handle);
			auto end = handle->node->buffer + dictionary.end;

			dictionary.Verify();
			// non-null value, check if we can fit it within the block
			idx_t string_length = source_data[source_idx].GetSize();
			idx_t dictionary_length = string_length + sizeof(uint16_t);

			// determine whether or not we have space in the block for this string
			bool use_overflow_block = false;
			idx_t required_space = dictionary_length;
			if (required_space >= StringUncompressed::STRING_BLOCK_LIMIT) {
				// string exceeds block limit, store in overflow block and only write a marker here
				required_space = BIG_STRING_MARKER_SIZE;
				use_overflow_block = true;
			}
			if (required_space > remaining_space) {
				// no space remaining: return how many tuples we ended up writing
				return i;
			}
			// we have space: write the string
			UpdateStringStats(stats, source_data[source_idx]);

			if (use_overflow_block) {
				// write to overflow blocks
				block_id_t block;
				int32_t offset;
				// write the string into the current string block
				WriteString(segment, source_data[source_idx], block, offset);
				dictionary.size += BIG_STRING_MARKER_SIZE;
				auto dict_pos = end - dictionary.size;

				// write a big string marker into the dictionary
				WriteStringMarker(dict_pos, block, offset);
			} else {
				// string fits in block, append to dictionary and increment dictionary position
				D_ASSERT(string_length < NumericLimits<uint16_t>::Maximum());
				dictionary.size += required_space;
				auto dict_pos = end - dictionary.size; // first write the length as u16
				Store<uint16_t>(string_length, dict_pos);
				// now write the actual string data into the dictionary
				memcpy(dict_pos + sizeof(uint16_t), source_data[source_idx].GetDataUnsafe(), string_length);
			}
			D_ASSERT(RemainingSpace(segment, *handle) <= Storage::BLOCK_SIZE);
			// place the dictionary offset into the set of vectors
			dictionary.Verify();

			result_data[target_idx] = dictionary.size;
			SetDictionary(segment, *handle, dictionary);
		}
		segment.count++;
	}
	return count;
}

idx_t UncompressedStringStorage::FinalizeAppend(ColumnSegment &segment, SegmentStatistics &stats) {
	auto &buffer_manager = BufferManager::GetBufferManager(segment.db);
	auto handle = buffer_manager.Pin(segment.block);
	auto dict = GetDictionary(segment, *handle);
	D_ASSERT(dict.end == Storage::BLOCK_SIZE);
	// compute the total size required to store this segment
	auto offset_size = DICTIONARY_HEADER_SIZE + segment.count * sizeof(int32_t);
	auto total_size = offset_size + dict.size;
	if (total_size >= Storage::BLOCK_SIZE / 5 * 4) {
		// the block is full enough, don't bother moving around the dictionary
		return Storage::BLOCK_SIZE;
	}
	// the block has space left: figure out how much space we can save
	auto move_amount = Storage::BLOCK_SIZE - total_size;
	// move the dictionary so it lines up exactly with the offsets
	memmove(handle->node->buffer + offset_size, handle->node->buffer + dict.end - dict.size, dict.size);
	dict.end -= move_amount;
	D_ASSERT(dict.end == total_size);
	// write the new dictionary (with the updated "end")
	SetDictionary(segment, *handle, dict);
	return total_size;
}

//===--------------------------------------------------------------------===//
// Get Function
//===--------------------------------------------------------------------===//
CompressionFunction StringUncompressed::GetFunction(PhysicalType data_type) {
	D_ASSERT(data_type == PhysicalType::VARCHAR);
	return CompressionFunction(CompressionType::COMPRESSION_UNCOMPRESSED, data_type,
	                           UncompressedStringStorage::StringInitAnalyze, UncompressedStringStorage::StringAnalyze,
	                           UncompressedStringStorage::StringFinalAnalyze, UncompressedFunctions::InitCompression,
	                           UncompressedFunctions::Compress, UncompressedFunctions::FinalizeCompress,
	                           UncompressedStringStorage::StringInitScan, UncompressedStringStorage::StringScan,
	                           UncompressedStringStorage::StringScanPartial, UncompressedStringStorage::StringFetchRow,
	                           UncompressedFunctions::EmptySkip, UncompressedStringStorage::StringInitSegment,
	                           UncompressedStringStorage::StringAppend, UncompressedStringStorage::FinalizeAppend);
}

//===--------------------------------------------------------------------===//
// Helper Functions
//===--------------------------------------------------------------------===//
void UncompressedStringStorage::SetDictionary(ColumnSegment &segment, BufferHandle &handle,
                                              StringDictionaryContainer container) {
	auto startptr = handle.node->buffer + segment.GetBlockOffset();
	Store<uint32_t>(container.size, startptr);
	Store<uint32_t>(container.end, startptr + sizeof(uint32_t));
}

StringDictionaryContainer UncompressedStringStorage::GetDictionary(ColumnSegment &segment, BufferHandle &handle) {
	auto startptr = handle.node->buffer + segment.GetBlockOffset();
	StringDictionaryContainer container;
	container.size = Load<uint32_t>(startptr);
	container.end = Load<uint32_t>(startptr + sizeof(uint32_t));
	return container;
}

idx_t UncompressedStringStorage::RemainingSpace(ColumnSegment &segment, BufferHandle &handle) {
	auto dictionary = GetDictionary(segment, handle);
	D_ASSERT(dictionary.end == Storage::BLOCK_SIZE);
	idx_t used_space = dictionary.size + segment.count * sizeof(int32_t) + DICTIONARY_HEADER_SIZE;
	D_ASSERT(Storage::BLOCK_SIZE >= used_space);
	return Storage::BLOCK_SIZE - used_space;
}

void UncompressedStringStorage::WriteString(ColumnSegment &segment, string_t string, block_id_t &result_block,
                                            int32_t &result_offset) {
	auto &state = (UncompressedStringSegmentState &)*segment.GetSegmentState();
	if (state.overflow_writer) {
		// overflow writer is set: write string there
		state.overflow_writer->WriteString(string, result_block, result_offset);
	} else {
		// default overflow behavior: use in-memory buffer to store the overflow string
		WriteStringMemory(segment, string, result_block, result_offset);
	}
}

void UncompressedStringStorage::WriteStringMemory(ColumnSegment &segment, string_t string, block_id_t &result_block,
                                                  int32_t &result_offset) {
	uint32_t total_length = string.GetSize() + sizeof(uint32_t);
	shared_ptr<BlockHandle> block;
	unique_ptr<BufferHandle> handle;

	auto &buffer_manager = BufferManager::GetBufferManager(segment.db);
	auto &state = (UncompressedStringSegmentState &)*segment.GetSegmentState();
	// check if the string fits in the current block
	if (!state.head || state.head->offset + total_length >= state.head->size) {
		// string does not fit, allocate space for it
		// create a new string block
		idx_t alloc_size = MaxValue<idx_t>(total_length, Storage::BLOCK_SIZE);
		auto new_block = make_unique<StringBlock>();
		new_block->offset = 0;
		new_block->size = alloc_size;
		// allocate an in-memory buffer for it
		block = buffer_manager.RegisterMemory(alloc_size, false);
		handle = buffer_manager.Pin(block);
		state.overflow_blocks[block->BlockId()] = new_block.get();
		new_block->block = move(block);
		new_block->next = move(state.head);
		state.head = move(new_block);
	} else {
		// string fits, copy it into the current block
		handle = buffer_manager.Pin(state.head->block);
	}

	result_block = state.head->block->BlockId();
	result_offset = state.head->offset;

	// copy the string and the length there
	auto ptr = handle->node->buffer + state.head->offset;
	Store<uint32_t>(string.GetSize(), ptr);
	ptr += sizeof(uint32_t);
	memcpy(ptr, string.GetDataUnsafe(), string.GetSize());
	state.head->offset += total_length;
}

string_t UncompressedStringStorage::ReadString(ColumnSegment &segment, Vector &result, block_id_t block,
                                               int32_t offset) {
	D_ASSERT(block != INVALID_BLOCK);
	D_ASSERT(offset < Storage::BLOCK_SIZE);

	auto &buffer_manager = BufferManager::GetBufferManager(segment.db);
	auto &state = (UncompressedStringSegmentState &)*segment.GetSegmentState();
	if (block < MAXIMUM_BLOCK) {
		// read the overflow string from disk
		// pin the initial handle and read the length
		auto block_handle = buffer_manager.RegisterBlock(block);
		auto handle = buffer_manager.Pin(block_handle);

		uint32_t length = Load<uint32_t>(handle->node->buffer + offset);
		uint32_t remaining = length;
		offset += sizeof(uint32_t);

		// allocate a buffer to store the string
		auto alloc_size = MaxValue<idx_t>(Storage::BLOCK_SIZE, length + sizeof(uint32_t));
		auto target_handle = buffer_manager.Allocate(alloc_size);
		auto target_ptr = target_handle->node->buffer;
		// write the length in this block as well
		Store<uint32_t>(length, target_ptr);
		target_ptr += sizeof(uint32_t);
		// now append the string to the single buffer
		while (remaining > 0) {
			idx_t to_write = MinValue<idx_t>(remaining, Storage::BLOCK_SIZE - sizeof(block_id_t) - offset);
			memcpy(target_ptr, handle->node->buffer + offset, to_write);

			remaining -= to_write;
			offset += to_write;
			target_ptr += to_write;
			if (remaining > 0) {
				// read the next block
				block_id_t next_block = Load<block_id_t>(handle->node->buffer + offset);
				block_handle = buffer_manager.RegisterBlock(next_block);
				handle = buffer_manager.Pin(block_handle);
				offset = 0;
			}
		}

		auto final_buffer = target_handle->node->buffer;
		StringVector::AddHandle(result, move(target_handle));
		return ReadString(final_buffer, 0);
	} else {
		// read the overflow string from memory
		// first pin the handle, if it is not pinned yet
		auto entry = state.overflow_blocks.find(block);
		D_ASSERT(entry != state.overflow_blocks.end());
		auto handle = buffer_manager.Pin(entry->second->block);
		auto final_buffer = handle->node->buffer;
		StringVector::AddHandle(result, move(handle));
		return ReadString(final_buffer, offset);
	}
}

string_t UncompressedStringStorage::ReadString(data_ptr_t target, int32_t offset) {
	auto ptr = target + offset;
	auto str_length = Load<uint32_t>(ptr);
	auto str_ptr = (char *)(ptr + sizeof(uint32_t));
	return string_t(str_ptr, str_length);
}

void UncompressedStringStorage::WriteStringMarker(data_ptr_t target, block_id_t block_id, int32_t offset) {
	uint16_t length = BIG_STRING_MARKER;
	memcpy(target, &length, sizeof(uint16_t));
	target += sizeof(uint16_t);
	memcpy(target, &block_id, sizeof(block_id_t));
	target += sizeof(block_id_t);
	memcpy(target, &offset, sizeof(int32_t));
}

void UncompressedStringStorage::ReadStringMarker(data_ptr_t target, block_id_t &block_id, int32_t &offset) {
	target += sizeof(uint16_t);
	memcpy(&block_id, target, sizeof(block_id_t));
	target += sizeof(block_id_t);
	memcpy(&offset, target, sizeof(int32_t));
}

string_location_t UncompressedStringStorage::FetchStringLocation(StringDictionaryContainer dict, data_ptr_t baseptr,
                                                                 int32_t dict_offset) {
	D_ASSERT(dict_offset >= 0 && dict_offset <= Storage::BLOCK_SIZE);
	if (dict_offset == 0) {
		return string_location_t(INVALID_BLOCK, 0);
	}
	// look up result in dictionary
	auto dict_end = baseptr + dict.end;
	auto dict_pos = dict_end - dict_offset;
	auto string_length = Load<uint16_t>(dict_pos);
	string_location_t result;
	if (string_length == BIG_STRING_MARKER) {
		ReadStringMarker(dict_pos, result.block_id, result.offset);
	} else {
		result.block_id = INVALID_BLOCK;
		result.offset = dict_offset;
	}
	return result;
}

string_t UncompressedStringStorage::FetchStringFromDict(ColumnSegment &segment, StringDictionaryContainer dict,
                                                        Vector &result, data_ptr_t baseptr, int32_t dict_offset) {
	// fetch base data
	D_ASSERT(dict_offset <= Storage::BLOCK_SIZE);
	string_location_t location = FetchStringLocation(dict, baseptr, dict_offset);
	return FetchString(segment, dict, result, baseptr, location);
}

string_t UncompressedStringStorage::FetchString(ColumnSegment &segment, StringDictionaryContainer dict, Vector &result,
                                                data_ptr_t baseptr, string_location_t location) {
	if (location.block_id != INVALID_BLOCK) {
		// big string marker: read from separate block
		return ReadString(segment, result, location.block_id, location.offset);
	} else {
		if (location.offset == 0) {
			return string_t(nullptr, 0);
		}
		// normal string: read string from this block
		auto dict_end = baseptr + dict.end;
		auto dict_pos = dict_end - location.offset;
		auto string_length = Load<uint16_t>(dict_pos);

		auto str_ptr = (char *)(dict_pos + sizeof(uint16_t));
		return string_t(str_ptr, string_length);
	}
}

} // namespace duckdb
