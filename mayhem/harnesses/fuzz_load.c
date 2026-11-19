/*
 * Fuzz the SafeGraphs checkpoint readers of one modality through USearch's public C 99 interface.
 *
 * Load mode (default): the input is a checkpoint buffer (16-byte SAFEGRPH prefix + JSON header +
 * column buffers, or a JSON manifest). It goes to `usearch_<m>_metadata_read_buffer`, then
 * `usearch_<m>_load_ram_buffer`.
 * View mode (-DUSEARCH_FUZZ_VIEW): the input is written to a scratch file and served in place with
 * `usearch_<m>_view_nvme` on a storage executor (the blocking-file engine when no NVMe device is
 * present), so the modality's columns are parsed straight from the file.
 * Either way, a checkpoint that opens is described, searched (graph walk + exact scan) and saved
 * back to a buffer.
 *
 * Built once per modality with -DUSEARCH_FUZZ_DENSE / _SEQUENCE / _SHAPES / _GEOSPATIAL.
 * State kept across inputs: one single-threaded executor (RAM, or storage in view mode), one arena
 * (rewound by every call that takes it, per its contract), and in view mode one scratch file under
 * $TMPDIR that every input truncates and rewrites in full before use, so no input influences another.
 */
#if defined(USEARCH_FUZZ_VIEW)
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#endif
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <usearch/usearch.h>

static usearch_executor_t executor;
static usearch_arena_t arena;
#if defined(USEARCH_FUZZ_VIEW)
static usearch_executor_t ram;
static char path[4096];
static int fd = -1;
#endif

int LLVMFuzzerInitialize(int* argc, char*** argv) {
    (void)argc, (void)argv;
    usearch_outcome_t outcome = {0};
    usearch_outcome_t arena_outcome = {0};
#if defined(USEARCH_FUZZ_VIEW)
    usearch_outcome_t ram_outcome = {0};
    usearch_executor_for_nvme(&(usearch_executor_for_nvme_t) {.executor = &executor, .outcome = &outcome, .threads = 1});
    usearch_executor_for_ram(&(usearch_executor_for_ram_t) {.executor = &ram, .outcome = &ram_outcome, .threads = 1});
    usearch_arena_init(&(usearch_arena_init_t) {.arena = &arena, .executor = ram, .outcome = &arena_outcome});
    char const* tmp = getenv("TMPDIR");
    snprintf(path, sizeof path, "%s/usearch-view-XXXXXX.safegraphs", tmp && *tmp ? tmp : "/tmp");
    fd = mkstemps(path, (int)strlen(".safegraphs"));
    if (fd < 0) { perror("mkstemps"); abort(); }
#else
    usearch_executor_for_ram(&(usearch_executor_for_ram_t) {.executor = &executor, .outcome = &outcome, .threads = 1});
    usearch_arena_init(&(usearch_arena_init_t) {.arena = &arena, .executor = executor, .outcome = &arena_outcome});
#endif
    return 0;
}

#define OK(o) (usearch_outcome_status(&(o)) == usearch_status_success_k)

#if defined(USEARCH_FUZZ_DENSE)
static void exercise(usearch_bytes_view_t buffer) {
#if !defined(USEARCH_FUZZ_VIEW)
    usearch_outcome_t outcome = {0};
    usearch_dense_metadata_t metadata = {0};
    usearch_dense_metadata_read_buffer(&(usearch_dense_metadata_read_buffer_t) {
        .executor = executor, .outcome = &outcome, .buffer = buffer, .metadata = &metadata});
#endif

    usearch_outcome_t load = {0};
    usearch_dense_index_t index = NULL;
#if defined(USEARCH_FUZZ_VIEW)
    usearch_dense_view_nvme(&(usearch_dense_view_nvme_t) {
        .index = &index, .executor = executor, .outcome = &load, .path = buffer});
#else
    usearch_dense_load_ram_buffer(&(usearch_dense_load_ram_buffer_t) {
        .index = &index, .executor = executor, .outcome = &load, .buffer = buffer});
#endif
    if (!OK(load) || !index) return;

    usearch_outcome_t o = {0};
    usearch_dense_index_description_t description = {0};
    usearch_dense_describe(&(usearch_dense_describe_t) {
        .index = index, .executor = executor, .outcome = &o, .description = &description});
    size_t dims = description.metadata.dimensions;
    if (OK(o) && dims && dims <= 4096) {
        static float queries[2 * 4096];
        for (size_t i = 0; i != 2 * dims; ++i) queries[i] = (float)((i * 37) % 11) - 5.0f;
        for (int t = 0; t != 2; ++t) {
            usearch_matches_view_t matches = {0};
            usearch_outcome_t so = {0};
            usearch_dense_search(&(usearch_dense_search_t) {
                .index = index, .executor = executor, .outcome = &so, .count = 2,
                .queries = {queries, dims * sizeof(float), dims, usearch_scalar_f32_k},
                .matches = &matches, .arena = arena, .wanted = 5,
                .traversal = t ? usearch_traversal_scan_k : usearch_traversal_walk_k});
        }
    }
    usearch_bytes_view_t saved = {0};
    usearch_outcome_t sv = {0};
    usearch_dense_save_buffer(&(usearch_dense_save_buffer_t) {
        .index = index, .executor = executor, .outcome = &sv, .monolith = &saved, .arena = arena});
    usearch_dense_free(index);
}
#elif defined(USEARCH_FUZZ_SEQUENCE)
static void exercise(usearch_bytes_view_t buffer) {
#if !defined(USEARCH_FUZZ_VIEW)
    usearch_outcome_t outcome = {0};
    usearch_sequence_metadata_t metadata = {0};
    usearch_sequence_metadata_read_buffer(&(usearch_sequence_metadata_read_buffer_t) {
        .executor = executor, .outcome = &outcome, .buffer = buffer, .metadata = &metadata});
#endif

    usearch_outcome_t load = {0};
    usearch_sequence_index_t index = NULL;
#if defined(USEARCH_FUZZ_VIEW)
    usearch_sequence_view_nvme(&(usearch_sequence_view_nvme_t) {
        .index = &index, .executor = executor, .outcome = &load, .path = buffer});
#else
    usearch_sequence_load_ram_buffer(&(usearch_sequence_load_ram_buffer_t) {
        .index = &index, .executor = executor, .outcome = &load, .buffer = buffer});
#endif
    if (!OK(load) || !index) return;

    usearch_outcome_t o = {0};
    usearch_sequence_index_description_t description = {0};
    usearch_sequence_describe(&(usearch_sequence_describe_t) {
        .index = index, .executor = executor, .outcome = &o, .description = &description});
    static char const text[] = "kittensittingmitten";
    static uint64_t const offsets[] = {0, 7, 19};
    for (int t = 0; t != 2; ++t) {
        usearch_tape_view_t tape = {.data = text, .layout = usearch_tape_u64_k, .scalar = usearch_scalar_u8_k};
        tape.offsets.u64 = offsets;
        usearch_matches_view_t matches = {0};
        usearch_outcome_t so = {0};
        usearch_sequence_search(&(usearch_sequence_search_t) {
            .index = index, .executor = executor, .outcome = &so, .count = 2, .queries = tape,
            .matches = &matches, .arena = arena, .wanted = 5,
            .traversal = t ? usearch_traversal_scan_k : usearch_traversal_walk_k});
    }
    usearch_bytes_view_t saved = {0};
    usearch_outcome_t sv = {0};
    usearch_sequence_save_buffer(&(usearch_sequence_save_buffer_t) {
        .index = index, .executor = executor, .outcome = &sv, .monolith = &saved, .arena = arena});
    usearch_sequence_free(index);
}
#elif defined(USEARCH_FUZZ_SHAPES)
static void exercise(usearch_bytes_view_t buffer) {
#if !defined(USEARCH_FUZZ_VIEW)
    usearch_outcome_t outcome = {0};
    usearch_shapes_metadata_t metadata = {0};
    usearch_shapes_metadata_read_buffer(&(usearch_shapes_metadata_read_buffer_t) {
        .executor = executor, .outcome = &outcome, .buffer = buffer, .metadata = &metadata});
#endif

    usearch_outcome_t load = {0};
    usearch_shapes_index_t index = NULL;
#if defined(USEARCH_FUZZ_VIEW)
    usearch_shapes_view_nvme(&(usearch_shapes_view_nvme_t) {
        .index = &index, .executor = executor, .outcome = &load, .path = buffer});
#else
    usearch_shapes_load_ram_buffer(&(usearch_shapes_load_ram_buffer_t) {
        .index = &index, .executor = executor, .outcome = &load, .buffer = buffer});
#endif
    if (!OK(load) || !index) return;

    usearch_outcome_t o = {0};
    usearch_shapes_index_description_t description = {0};
    usearch_shapes_describe(&(usearch_shapes_describe_t) {
        .index = index, .executor = executor, .outcome = &o, .description = &description});
    size_t points = description.metadata.points;
    if (OK(o) && points && points <= 1024) {
        static float coords[2 * 1024 * 3];
        static uint64_t offsets[3];
        for (size_t i = 0; i != 2 * points * 3; ++i) coords[i] = (float)((i * 13) % 7) - 3.0f;
        offsets[0] = 0, offsets[1] = points, offsets[2] = 2 * points;
        for (int t = 0; t != 2; ++t) {
            usearch_tape_view_t tape = {.data = coords, .layout = usearch_tape_u64_k, .scalar = usearch_scalar_f32_k};
            tape.offsets.u64 = offsets;
            usearch_matches_view_t matches = {0};
            usearch_outcome_t so = {0};
            usearch_shapes_search(&(usearch_shapes_search_t) {
                .index = index, .executor = executor, .outcome = &so, .count = 2, .queries = tape,
                .matches = &matches, .arena = arena, .wanted = 5,
                .traversal = t ? usearch_traversal_scan_k : usearch_traversal_walk_k});
        }
    }
    usearch_bytes_view_t saved = {0};
    usearch_outcome_t sv = {0};
    usearch_shapes_save_buffer(&(usearch_shapes_save_buffer_t) {
        .index = index, .executor = executor, .outcome = &sv, .monolith = &saved, .arena = arena});
    usearch_shapes_free(index);
}
#elif defined(USEARCH_FUZZ_GEOSPATIAL)
static void exercise(usearch_bytes_view_t buffer) {
#if !defined(USEARCH_FUZZ_VIEW)
    usearch_outcome_t outcome = {0};
    usearch_geospatial_metadata_t metadata = {0};
    usearch_geospatial_metadata_read_buffer(&(usearch_geospatial_metadata_read_buffer_t) {
        .executor = executor, .outcome = &outcome, .buffer = buffer, .metadata = &metadata});
#endif

    usearch_outcome_t load = {0};
    usearch_geospatial_index_t index = NULL;
#if defined(USEARCH_FUZZ_VIEW)
    usearch_geospatial_view_nvme(&(usearch_geospatial_view_nvme_t) {
        .index = &index, .executor = executor, .outcome = &load, .path = buffer});
#else
    usearch_geospatial_load_ram_buffer(&(usearch_geospatial_load_ram_buffer_t) {
        .index = &index, .executor = executor, .outcome = &load, .buffer = buffer});
#endif
    if (!OK(load) || !index) return;

    usearch_outcome_t o = {0};
    usearch_geospatial_index_description_t description = {0};
    usearch_geospatial_describe(&(usearch_geospatial_describe_t) {
        .index = index, .executor = executor, .outcome = &o, .description = &description});
    usearch_geospatial_observation_t const queries[2] = {
        {.latitude_degrees = 48.8566, .longitude_degrees = 2.3522, .field_of_view_degrees = 60, .range_metres = 1000},
        {.latitude_degrees = 40.7128, .longitude_degrees = -74.006, .field_of_view_degrees = 90, .range_metres = 5000},
    };
    for (int r = 0; r != 2; ++r) {
        usearch_matches_view_t matches = {0};
        usearch_outcome_t so = {0};
        usearch_geospatial_search(&(usearch_geospatial_search_t) {
            .index = index, .executor = executor, .outcome = &so, .count = 2, .queries = queries,
            .relation = r ? usearch_geospatial_relation_visible_k : usearch_geospatial_relation_near_k,
            .tolerance = {.metres = 500}, .matches = &matches, .arena = arena, .wanted = 5});
    }
    usearch_bytes_view_t saved = {0};
    usearch_outcome_t sv = {0};
    usearch_geospatial_save_buffer(&(usearch_geospatial_save_buffer_t) {
        .index = index, .executor = executor, .outcome = &sv, .monolith = &saved, .arena = arena});
    usearch_geospatial_free(index);
}
#else
#error "define one of USEARCH_FUZZ_DENSE / _SEQUENCE / _SHAPES / _GEOSPATIAL"
#endif

int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size) {
#if defined(USEARCH_FUZZ_VIEW)
    if (ftruncate(fd, 0) != 0 || pwrite(fd, data, size, 0) != (ssize_t)size) abort();
    exercise((usearch_bytes_view_t) {(usearch_byte_t const*)path, strlen(path)});
#else
    /* An empty buffer is refused with invalid_argument by contract; skip it. */
    if (size == 0) return -1;
    exercise((usearch_bytes_view_t) {data, size});
#endif
    return 0;
}
