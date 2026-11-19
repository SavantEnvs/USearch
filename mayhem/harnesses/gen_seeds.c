/* Writes one small SafeGraphs checkpoint per modality (dense, sequence, shapes, geospatial) into the
 * directory named by argv[1], through the public save_buffer verbs. Used to (re)make the seed corpora. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <usearch/usearch.h>

#define CHECK(o, what) \
    if (usearch_outcome_status(&(o)) != usearch_status_success_k) { \
        fprintf(stderr, "%s: %s\n", what, (o).message ? (o).message : "?"); exit(1); }

static usearch_key_t key_of(unsigned v) {
    usearch_key_t k; memset(k.bytes, 0, 16); k.bytes[15] = (unsigned char)v; k.bytes[14] = (unsigned char)(v >> 8); return k;
}

static void dump(char const* dir, char const* name, usearch_bytes_view_t b) {
    char path[4096]; snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE* f = fopen(path, "wb"); if (!f) { perror(path); exit(1); }
    fwrite(b.data, 1, b.length, f); fclose(f);
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    usearch_outcome_t o = {0};
    usearch_executor_t ex = NULL;
    usearch_executor_for_ram(&(usearch_executor_for_ram_t) {.executor = &ex, .outcome = &o, .threads = 1});
    CHECK(o, "executor");
    usearch_arena_t arena = NULL;
    usearch_arena_init(&(usearch_arena_init_t) {.arena = &arena, .executor = ex, .outcome = &o});
    CHECK(o, "arena");
    usearch_key_t keys[32];
    for (unsigned i = 0; i != 32; ++i) keys[i] = key_of(i);

    { /* dense: 32 x 8 bf16 angular */
        usearch_dense_index_t idx = NULL;
        usearch_dense_init_ram(&(usearch_dense_init_ram_t) {.index = &idx, .outcome = &o,
            .metric = usearch_dense_metric_angular_k, .quantization = usearch_scalar_bf16_k, .dimensions = 8, .connectivity = 8});
        CHECK(o, "dense init");
        float v[32 * 8];
        for (unsigned i = 0; i != 32 * 8; ++i) v[i] = (float)((i * 7919u) % 17) - 8.0f;
        usearch_dense_insert(&(usearch_dense_insert_t) {.index = idx, .executor = ex, .outcome = &o, .count = 32,
            .keys = {keys, sizeof(usearch_key_t)}, .vectors = {v, 8 * sizeof(float), 8, usearch_scalar_f32_k}});
        CHECK(o, "dense insert");
        usearch_bytes_view_t b = {0};
        usearch_dense_save_buffer(&(usearch_dense_save_buffer_t) {.index = idx, .executor = ex, .outcome = &o, .monolith = &b, .arena = arena});
        CHECK(o, "dense save");
        dump(argv[1], "dense.safegraphs", b);
        usearch_dense_free(idx);
    }
    { /* sequence: levenshtein bytes */
        usearch_sequence_index_t idx = NULL;
        usearch_sequence_init_ram(&(usearch_sequence_init_ram_t) {.index = &idx, .outcome = &o,
            .metric = usearch_sequence_metric_levenshtein_bytes_k, .connectivity = 8});
        CHECK(o, "sequence init");
        static char const text[] = "kittensittingmittenflaskfuzzingbuzzinggraphsearchvector";
        uint64_t off[] = {0, 6, 13, 19, 24, 31, 38, 43, 49, 55};
        usearch_tape_view_t tape = {.data = text, .layout = usearch_tape_u64_k, .scalar = usearch_scalar_u8_k};
        tape.offsets.u64 = off;
        usearch_sequence_insert(&(usearch_sequence_insert_t) {.index = idx, .executor = ex, .outcome = &o, .count = 9,
            .keys = {keys, sizeof(usearch_key_t)}, .sequences = tape});
        CHECK(o, "sequence insert");
        usearch_bytes_view_t b = {0};
        usearch_sequence_save_buffer(&(usearch_sequence_save_buffer_t) {.index = idx, .executor = ex, .outcome = &o, .monolith = &b, .arena = arena});
        CHECK(o, "sequence save");
        dump(argv[1], "sequence.safegraphs", b);
        usearch_sequence_free(idx);
    }
    { /* shapes: 8 poses of 4 points, chamfer, f32 */
        usearch_shapes_index_t idx = NULL;
        usearch_shapes_init_ram(&(usearch_shapes_init_ram_t) {.index = &idx, .outcome = &o,
            .metric = usearch_shapes_metric_chamfer_k, .quantization = usearch_scalar_f32_k, .points = 4});
        CHECK(o, "shapes init");
        float p[8 * 4 * 3];
        for (unsigned i = 0; i != 8 * 4 * 3; ++i) p[i] = (float)((i * 31u) % 13) * 0.5f;
        uint64_t off[9];
        for (unsigned i = 0; i != 9; ++i) off[i] = 4 * i;
        usearch_tape_view_t tape = {.data = p, .layout = usearch_tape_u64_k, .scalar = usearch_scalar_f32_k};
        tape.offsets.u64 = off;
        usearch_shapes_insert(&(usearch_shapes_insert_t) {.index = idx, .executor = ex, .outcome = &o, .count = 8,
            .keys = {keys, sizeof(usearch_key_t)}, .shapes = tape});
        CHECK(o, "shapes insert");
        usearch_bytes_view_t b = {0};
        usearch_shapes_save_buffer(&(usearch_shapes_save_buffer_t) {.index = idx, .executor = ex, .outcome = &o, .monolith = &b, .arena = arena});
        CHECK(o, "shapes save");
        dump(argv[1], "shapes.safegraphs", b);
        usearch_shapes_free(idx);
    }
    { /* geospatial: a few observations */
        usearch_geospatial_index_t idx = NULL;
        usearch_geospatial_init_ram(&(usearch_geospatial_init_ram_t) {.index = &idx, .outcome = &o});
        CHECK(o, "geospatial init");
        usearch_geospatial_observation_t obs[4] = {
            {.latitude_degrees = 48.8566, .longitude_degrees = 2.3522, .field_of_view_degrees = 60, .range_metres = 1000},
            {.latitude_degrees = 48.8570, .longitude_degrees = 2.3530, .heading_degrees = 90, .field_of_view_degrees = 60, .range_metres = 800},
            {.latitude_degrees = 40.7128, .longitude_degrees = -74.006, .field_of_view_degrees = 90, .range_metres = 5000},
            {.latitude_degrees = 51.5074, .longitude_degrees = -0.1278, .altitude_metres = 30, .range_metres = 200},
        };
        usearch_geospatial_insert(&(usearch_geospatial_insert_t) {.index = idx, .executor = ex, .outcome = &o, .count = 4,
            .keys = {keys, sizeof(usearch_key_t)}, .observations = obs});
        CHECK(o, "geospatial insert");
        usearch_bytes_view_t b = {0};
        usearch_geospatial_save_buffer(&(usearch_geospatial_save_buffer_t) {.index = idx, .executor = ex, .outcome = &o, .monolith = &b, .arena = arena});
        CHECK(o, "geospatial save");
        dump(argv[1], "geospatial.safegraphs", b);
        usearch_geospatial_free(idx);
    }
    usearch_arena_free(arena);
    usearch_executor_free(ex);
    return 0;
}
