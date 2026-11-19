/*
 * Fuzz the dense in-memory index through a sequence of public C 99 calls decoded from the input:
 * create (metric, quantization, dimensions, connectivity, values-per-key from the first bytes), then
 * insert / search (walk or exact scan) / get / clear / save_buffer -> load_ram_buffer round trips.
 * Keys and vector components come straight from the input. One single-threaded RAM executor and one
 * arena are kept across inputs (the arena is rewound by every call that takes it); every index is
 * created and freed within one input.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <usearch/usearch.h>

static usearch_executor_t executor;
static usearch_arena_t arena;

int LLVMFuzzerInitialize(int* argc, char*** argv) {
    (void)argc, (void)argv;
    usearch_outcome_t outcome = {0};
    usearch_executor_for_ram(&(usearch_executor_for_ram_t) {.executor = &executor, .outcome = &outcome, .threads = 1});
    usearch_outcome_t arena_outcome = {0};
    usearch_arena_init(&(usearch_arena_init_t) {.arena = &arena, .executor = executor, .outcome = &arena_outcome});
    return 0;
}

#define OK(o) (usearch_outcome_status(&(o)) == usearch_status_success_k)

typedef struct {
    uint8_t const* data;
    size_t size;
} cursor_t;

static int take(cursor_t* c, void* out, size_t n) {
    if (c->size < n) return 0;
    memcpy(out, c->data, n);
    c->data += n, c->size -= n;
    return 1;
}

static usearch_key_t key_of(uint8_t value) {
    usearch_key_t key;
    memset(key.bytes, 0, sizeof(key.bytes));
    key.bytes[15] = value;
    return key;
}

int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size) {
    static usearch_dense_metric_t const metrics[] = {usearch_dense_metric_inner_k, usearch_dense_metric_angular_k,
                                                      usearch_dense_metric_euclidean_k, usearch_dense_metric_hamming_k,
                                                      usearch_dense_metric_jaccard_k};
    static usearch_scalar_t const scalars[] = {usearch_scalar_bf16_k, usearch_scalar_i8_k, usearch_scalar_e4m3_k,
                                                usearch_scalar_u1x8_k, usearch_scalar_f32_k};
    cursor_t c = {data, size};
    uint8_t header[5];
    if (!take(&c, header, sizeof(header))) return -1;
    size_t const dims = 1 + header[2] % 64;
    usearch_outcome_t o = {0};
    usearch_dense_index_t index = NULL;
    usearch_dense_init_ram(&(usearch_dense_init_ram_t) {.index = &index,
                                                        .outcome = &o,
                                                        .metric = metrics[header[0] % 5],
                                                        .quantization = scalars[header[1] % 5],
                                                        .dimensions = dims,
                                                        .connectivity = header[3] % 33,
                                                        .values_per_key = header[4] % 4});
    if (!OK(o) || !index) return 0;

    float vectors[8 * 64];
    usearch_key_t keys[8];
    uint8_t op;
    while (take(&c, &op, 1)) {
        uint8_t arg = 0;
        take(&c, &arg, 1);
        size_t const count = 1 + arg % 8;
        switch (op % 6) {
        case 0: /* insert `count` rows */
        case 1: { /* search `count` queries */
            int8_t raw[8 * 64];
            size_t const bytes = count * dims;
            size_t const got = c.size < bytes ? c.size : bytes;
            memset(raw, 0, sizeof(raw));
            take(&c, raw, got);
            for (size_t i = 0; i != bytes; ++i) vectors[i] = (float)raw[i] / 8.0f;
            usearch_rows_view_t rows = {vectors, dims * sizeof(float), dims, usearch_scalar_f32_k};
            usearch_outcome_t so = {0};
            if (op % 6 == 0) {
                for (size_t i = 0; i != count; ++i) keys[i] = key_of((uint8_t)(raw[i * dims] ^ arg));
                usearch_dense_insert(&(usearch_dense_insert_t) {.index = index, .executor = executor, .outcome = &so,
                                                                .count = count, .keys = {keys, sizeof(usearch_key_t)},
                                                                .vectors = rows});
            } else {
                usearch_matches_view_t matches = {0};
                usearch_dense_search(&(usearch_dense_search_t) {
                    .index = index, .executor = executor, .outcome = &so, .count = count, .queries = rows,
                    .matches = &matches, .arena = arena, .wanted = arg % 9, .expansion_factor = arg / 16,
                    .traversal = (arg & 1) ? usearch_traversal_scan_k : usearch_traversal_walk_k});
            }
            break;
        }
        case 2: { /* get one key */
            usearch_rows_view_t got_rows = {0};
            size_t written = 0;
            usearch_outcome_t go = {0};
            usearch_dense_get(&(usearch_dense_get_t) {.index = index, .executor = executor, .outcome = &go,
                                                      .key = key_of(arg), .vectors = &got_rows, .written = &written,
                                                      .arena = arena, .outputs_scalar = usearch_scalar_f32_k});
            break;
        }
        case 3: { /* save to a buffer and reload */
            usearch_bytes_view_t saved = {0};
            usearch_outcome_t sv = {0};
            usearch_dense_save_buffer(&(usearch_dense_save_buffer_t) {
                .index = index, .executor = executor, .outcome = &sv, .monolith = &saved, .arena = arena});
            if (OK(sv) && saved.data && saved.length) {
                usearch_dense_index_t reloaded = NULL;
                usearch_outcome_t lo = {0};
                usearch_dense_load_ram_buffer(&(usearch_dense_load_ram_buffer_t) {
                    .index = &reloaded, .executor = executor, .outcome = &lo, .buffer = saved,
                    .values_per_key = header[4] % 4});
                if (OK(lo) && reloaded) usearch_dense_free(index), index = reloaded;
            }
            break;
        }
        case 4: { /* describe */
            usearch_dense_index_description_t description = {0};
            usearch_outcome_t d = {0};
            usearch_dense_describe(&(usearch_dense_describe_t) {
                .index = index, .executor = executor, .outcome = &d, .description = &description});
            break;
        }
        case 5: { /* clear on a rare opcode */
            if (arg != 0xC1) break;
            usearch_outcome_t co = {0};
            usearch_dense_clear(&(usearch_dense_clear_t) {.index = index, .executor = executor, .outcome = &co});
            break;
        }
        }
    }
    usearch_dense_free(index);
    return 0;
}
