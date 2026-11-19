/*
 * Fuzz USearch's header-only Apache Arrow IPC codec (tools/arrow_ipc.hpp), the hand-written FlatBuffers
 * reader behind `usearch import` / `usearch recall` / `usearch metadata` on `.arrow` and `.arrows` inputs.
 * It mirrors the two readers in tools/arrow_checkpoints.hpp, with fixed-size pools as the codec expects:
 *  - file framing (input opens with "ARROW1"): `ipc_read_footer`, then per Block `ipc_read_prefix` ->
 *    `ipc_read_message` -> `ipc_read_batch` -> `ipc_validate_batch` -> `ipc_batch_to_arrow`
 *    (as `arrow_import` does);
 *  - stream framing (both framings carry one after the magic): `ipc_read_prefix` / `ipc_read_message`,
 *    a leading `ipc_read_schema` + `ipc_schema_to_arrow`, then every record batch as above (as
 *    `arrow_input_t::start_` / `next` do).
 * The codec neither allocates nor does I/O, so no state is kept across inputs.
 */
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "arrow_ipc.hpp"

using namespace unum::usearch::tools;

namespace {

// Large enough for any schema ipc_fields_max_k allows; the text pool is sized as the tool sizes it.
std::array<ipc_field_t, ipc_fields_max_k> fields_pool;
std::array<ipc_key_value_t, 4 * ipc_fields_max_k> pairs_pool;
std::array<ipc_node_t, ipc_fields_max_k> nodes_pool;
std::array<bytes_view_t, ipc_buffers_max_k> buffers_pool;
std::array<ipc_block_t, 64> blocks_pool;
ipc_arrow_storage_t storage;
alignas(8) char text_pool[1 << 16];

void export_batch(ipc_schema_t const& schema, ipc_batch_t const& batch) {
    ArrowArray array {};
    if (ipc_validate_batch(schema, batch) != ipc_status_t::success_k) return;
    if (ipc_batch_to_arrow(schema, batch, storage, array) == ipc_status_t::success_k && array.release)
        array.release(&array);
}

/* As arrow_import reads a whole mapped file through its footer. */
void read_file(bytes_view_t file) {
    ipc_schema_t schema;
    std::size_t blocks_count = 0;
    if (ipc_read_footer(file, fields_pool, pairs_pool, schema, blocks_pool, blocks_count) != ipc_status_t::success_k)
        return;
    for (std::size_t index = 0; index != blocks_count; ++index) {
        ipc_block_t const& block = blocks_pool[index];
        bytes_view_t const message_bytes = file.subspan(static_cast<std::size_t>(block.offset));
        std::uint32_t metadata_bytes = 0;
        std::size_t prefix_bytes = 0;
        ipc_message_t message;
        ipc_status_t status = ipc_read_prefix(message_bytes, metadata_bytes, prefix_bytes);
        if (status == ipc_status_t::success_k && prefix_bytes + metadata_bytes > message_bytes.size())
            status = ipc_status_t::truncated_k;
        if (status == ipc_status_t::success_k)
            status = ipc_read_message(message_bytes.subspan(prefix_bytes, metadata_bytes), message);
        ipc_batch_t batch;
        if (status == ipc_status_t::success_k)
            status = ipc_read_batch(message, message_bytes.subspan(static_cast<std::size_t>(block.metadata_bytes)),
                                    schema, nodes_pool, buffers_pool, batch);
        if (status != ipc_status_t::success_k) return;
        export_batch(schema, batch);
    }
}

/* As arrow_input_t::read_message_ frames one message off the front of `rest`; false to stop. */
bool next_message(bytes_view_t& rest, ipc_message_t& message, bytes_view_t& body, bool& ended) {
    std::uint32_t metadata_bytes = 0;
    std::size_t prefix_bytes = 0;
    if (ipc_read_prefix(rest, metadata_bytes, prefix_bytes) != ipc_status_t::success_k) return false;
    rest = rest.subspan(prefix_bytes);
    ended = !metadata_bytes;
    if (ended) return true;
    if (metadata_bytes > rest.size()) return false;
    bytes_view_t const framed = rest.first(metadata_bytes);
    rest = rest.subspan(metadata_bytes);
    if (ipc_read_message(framed, message) != ipc_status_t::success_k) return false;
    std::uint64_t const body_bytes = static_cast<std::uint64_t>(message.body_bytes);
    if (body_bytes > rest.size()) return false;
    body = rest.first(static_cast<std::size_t>(body_bytes));
    rest = rest.subspan(static_cast<std::size_t>(body_bytes));
    return true;
}

/* As arrow_input_t::start_ and next read a stream, or a file front to back. */
void read_stream(bytes_view_t rest) {
    ipc_message_t message;
    bytes_view_t body;
    bool ended = false;
    if (!next_message(rest, message, body, ended) || ended || message.kind != ipc_message_kind_t::schema_k) return;
    ipc_schema_t schema;
    if (ipc_read_schema(message, fields_pool, pairs_pool, schema) != ipc_status_t::success_k) return;
    std::size_t const text_bytes = ipc_arrow_metadata_bytes(schema.metadata) + ipc_arrow_text_bytes(schema.fields) + 1;
    if (text_bytes <= sizeof text_pool) {
        storage.text = {text_pool, text_bytes};
        ArrowSchema exported {};
        if (ipc_schema_to_arrow(schema, storage, exported) == ipc_status_t::success_k && exported.release)
            exported.release(&exported);
    }
    while (next_message(rest, message, body, ended) && !ended) {
        if (message.kind != ipc_message_kind_t::record_batch_k) return;
        ipc_batch_t batch;
        if (ipc_read_batch(message, body, schema, nodes_pool, buffers_pool, batch) != ipc_status_t::success_k) return;
        export_batch(schema, batch);
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data, std::size_t size) {
    bytes_view_t const input {reinterpret_cast<std::byte const*>(data), size};
    bool const is_file = size >= 6 && std::memcmp(data, ipc_file_magic_k.data(), 6) == 0;
    if (is_file) {
        read_file(input);
        if (size >= ipc_file_magic_k.size()) read_stream(input.subspan(ipc_file_magic_k.size()));
    }
    else
        read_stream(input);
    return 0;
}
