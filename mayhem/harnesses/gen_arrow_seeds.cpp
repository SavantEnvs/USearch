/*
 * Writes fuzz-arrow-ipc seeds with the codec's own writers, laid out as arrow_write_messages
 * (tools/arrow_checkpoints.hpp) lays them: `<dir>/table.arrow` (file framing) and `<dir>/table.arrows`
 * (stream framing), one batch of keys (UUID fixed-size binary), f32 embeddings and large-list matches.
 * Not built by build.sh:  clang++ -std=c++20 -Itools mayhem/harnesses/gen_arrow_seeds.cpp -o /tmp/g && /tmp/g <dir>
 */
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "arrow_ipc.hpp"

using namespace unum::usearch::tools;

static std::vector<std::byte> write(bool footed, ipc_schema_t const& schema, ipc_batch_t const& batch) {
    std::vector<std::byte> out;
    auto put = [&](bytes_view_t bytes) { out.insert(out.end(), bytes.begin(), bytes.end()); };
    std::vector<std::byte> metadata(1 << 16);
    std::size_t written = 0;
    if (footed) put(std::as_bytes(std::span {ipc_file_magic_k}));
    if (ipc_write_schema(schema, {}, out.size(), metadata, written) != ipc_status_t::success_k) std::abort();
    put(std::span {metadata}.first(written));
    std::array<ipc_gather_t, ipc_buffers_max_k> gathers;
    std::uint64_t body_bytes = 0;
    std::size_t const position = out.size();
    if (ipc_write_batch(schema, batch, {}, position, metadata, written, gathers, body_bytes) != ipc_status_t::success_k)
        std::abort();
    ipc_block_t const block {static_cast<std::int64_t>(position), static_cast<std::int32_t>(written),
                             static_cast<std::int64_t>(body_bytes)};
    put(std::span {metadata}.first(written));
    for (std::size_t i = 0; i != batch.buffers.size(); ++i)
        put(gathers[i].bytes), out.resize(out.size() + gathers[i].padding);
    std::array<std::byte, 8> eos {};
    ipc_write_end_of_stream(eos, written);
    put(eos);
    if (footed) {
        if (ipc_write_footer(schema, {&block, 1}, metadata, written) != ipc_status_t::success_k) std::abort();
        put(std::span {metadata}.first(written));
    }
    return out;
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    constexpr std::int64_t rows = 3, dims = 4;
    std::array<ipc_key_value_t, 1> const uuid {{{ipc_extension_name_key_k, ipc_extension_uuid_k}}};
    std::array<ipc_key_value_t, 1> const meta {{{"metric", "cos"}}};
    ipc_field_t const element {.name = "item", .type = ipc_type_t::floating_point_k, .width = 32};
    std::array<ipc_field_t, 2> const match_fields {{
        {.name = "key", .type = ipc_type_t::fixed_size_binary_k, .width = 16, .metadata = uuid},
        {.name = "distance", .type = ipc_type_t::floating_point_k, .width = 64},
    }};
    ipc_field_t const match {.name = "item", .type = ipc_type_t::struct_k, .children = match_fields};
    std::array<ipc_field_t, 4> const fields {{
        {.name = "key", .type = ipc_type_t::fixed_size_binary_k, .width = 16, .metadata = uuid},
        {.name = "value", .type = ipc_type_t::fixed_size_list_k, .width = dims, .children = {&element, 1}},
        {.name = "matches", .type = ipc_type_t::large_list_k, .children = {&match, 1}},
        {.name = "label", .type = ipc_type_t::utf8_k},
    }};
    std::array<std::uint8_t, 16 * rows> keys {};
    for (std::size_t i = 0; i != keys.size(); ++i) keys[i] = static_cast<std::uint8_t>(i * 7);
    std::array<float, rows * dims> values {};
    for (std::size_t i = 0; i != values.size(); ++i) values[i] = static_cast<float>(i) - 5.0f;
    std::array<std::int64_t, rows + 1> const offsets {0, 1, 1, 2};
    std::array<std::uint8_t, 16 * 2> match_keys {};
    std::array<double, 2> const distances {0.25, 0.5};
    std::array<std::int32_t, rows + 1> const label_offsets {0, 3, 3, 8};
    char const labels[] = "catmouse";
    std::array<ipc_node_t, 8> const nodes {{{rows, 0}, {rows, 0}, {rows * dims, 0}, {rows, 0},
                                             {2, 0}, {2, 0}, {2, 0}, {rows, 0}}};
    std::array<bytes_view_t, 14> const buffers {
        bytes_view_t {}, std::as_bytes(std::span {keys}),                     // key
        bytes_view_t {}, bytes_view_t {}, std::as_bytes(std::span {values}),  // value, item
        bytes_view_t {}, std::as_bytes(std::span {offsets}),                  // matches
        bytes_view_t {},                                                      // item (struct)
        bytes_view_t {}, std::as_bytes(std::span {match_keys}),               // key
        bytes_view_t {}, std::as_bytes(std::span {distances}),                // distance
        bytes_view_t {}, std::as_bytes(std::span {label_offsets}),            // label ...
    };
    std::array<bytes_view_t, 15> all {};
    for (std::size_t i = 0; i != buffers.size(); ++i) all[i] = buffers[i];
    all[14] = std::as_bytes(std::span {labels, 8});
    ipc_schema_t const schema {fields, meta};
    ipc_batch_t const batch {rows, nodes, all};
    if (ipc_validate_batch(schema, batch) != ipc_status_t::success_k) { std::fputs("invalid batch\n", stderr); return 1; }
    for (bool footed : {true, false}) {
        std::vector<std::byte> const bytes = write(footed, schema, batch);
        std::string const path = std::string(argv[1]) + (footed ? "/table.arrow" : "/table.arrows");
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f || std::fwrite(bytes.data(), 1, bytes.size(), f) != bytes.size() || std::fclose(f)) return 1;
    }
    return 0;
}
