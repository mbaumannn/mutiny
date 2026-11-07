/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Project-record contract tests: bounded decoding, atomic fallback and reload behavior. */
#include "../mods/mutiny/storage.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            failures++;                                                                            \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                            \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
        }                                                                                          \
    } while (0)

/* Wire layout is an external contract, independent of C struct padding. */
enum {
    RECORD_BYTES = 116,
    HEADER_BYTES = 4,
    TRACK_BYTES = 26,
    CONTROL_OFFSET = 1,
    SEED_OFFSET = 22,
    ACID_OFFSET = 23,
    PLAY_OFFSET = 25,
    CRC_OFFSET = 108,
    FOOTER_OFFSET = 112
};

/* Independent CRC-32/ISO-HDLC implementation used to construct hostile but correctly sealed input.
 */
static uint32_t test_crc32(const uint8_t *bytes, uint32_t n) {
    uint32_t crc = UINT32_C(0xffffffff);
    for (uint32_t i = 0; i < n; i++) {
        crc ^= bytes[i];
        for (uint32_t bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ ((crc & 1) ? UINT32_C(0xedb88320) : 0);
    }
    return crc ^ UINT32_C(0xffffffff);
}

static void seal(uint8_t *bytes) {
    uint32_t crc = test_crc32(bytes, CRC_OFFSET);
    bytes[CRC_OFFSET] = (uint8_t)(crc >> 24);
    bytes[CRC_OFFSET + 1] = (uint8_t)(crc >> 16);
    bytes[CRC_OFFSET + 2] = (uint8_t)(crc >> 8);
    bytes[CRC_OFFSET + 3] = (uint8_t)crc;
}

static int all_off_native(const struct mu_rt *rt) {
    for (uint32_t i = 0; i < MU_STORE_TRACKS; i++)
        if (rt->t[i].recipe.model != MU_OFF || rt->t[i].play != MU_PLAY_NATIVE || rt->t[i].preview)
            return 0;
    return 1;
}

static void make_nondefault(struct mu_rt *rt) {
    mu_rt_init(rt);
    for (uint32_t i = 0; i < MU_STORE_TRACKS; i++)
        mu_rt_select_model(&rt->t[i], MU_TURI);
}

static void reject(const uint8_t *bytes, uint32_t n, const char *what) {
    struct mu_rt rt;
    make_nondefault(&rt);
    CHECK(mu_store_decode(&rt, bytes, n) == 0, "%s accepted", what);
    CHECK(all_off_native(&rt), "%s did not atomically restore defaults", what);
}

static int steps_equal(const struct mu_step *a, const struct mu_step *b) {
    for (uint32_t i = 0; i < MU_STEPS; i++)
        if (a[i].trig != b[i].trig || a[i].note != b[i].note || a[i].vel != b[i].vel ||
            a[i].slide != b[i].slide || a[i].hold != b[i].hold)
            return 0;
    return 1;
}

static uint32_t random_word(uint32_t *state) {
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return *state;
}

int main(void) {
    struct mu_store store, defaults;
    struct mu_rt original, loaded;
    uint8_t wire[RECORD_BYTES + 4];
    uint8_t bad[RECORD_BYTES];
    struct mu_key key = {.note = 48, .root = 0, .scale = -1, .len = 64};
    struct mu_step before[MU_STEPS], after[MU_STEPS];

    CHECK(sizeof store == RECORD_BYTES && MU_STORE_BYTES == RECORD_BYTES,
          "record is not the specified 116 bytes");
    CHECK(MU_STORE_BYTES <= 136, "record %u exceeds digitables coexistence budget", MU_STORE_BYTES);
    CHECK((MU_STORE_BYTES & 3u) == 0, "core descriptor size is not a multiple of four");
    CHECK(4 + 8 + 320 + 8 + MU_STORE_BYTES + 4 <= 480,
          "record and digitables do not fit the project gap");
    CHECK(test_crc32((const uint8_t *)"123456789", 9) == UINT32_C(0xcbf43926),
          "test CRC reference vector failed");

    mu_store_defaults(&defaults);
    CHECK(mu_store_valid(&defaults), "default record is invalid");
    make_nondefault(&loaded);
    mu_store_load(&defaults, &loaded);
    CHECK(all_off_native(&loaded), "default record did not load native/OFF");
    CHECK(defaults.bytes[0] == MU_STORE_VER && defaults.bytes[1] == 0 && defaults.bytes[2] == 0 &&
              defaults.bytes[3] == 0,
          "default header invalid");
    CHECK(memcmp(defaults.bytes + FOOTER_OFFSET, "MUT2", 4) == 0, "terminal marker missing");

    mu_rt_init(&original);
    mu_rt_select_model(&original.t[0], MU_EUCL);
    mu_rt_select_model(&original.t[1], MU_TURI);
    mu_rt_select_model(&original.t[2], MU_TB3P);
    /* All per-model controls remain, including the settings of inactive models. */
    for (uint32_t t = 0; t < MU_STORE_TRACKS; t++) {
        original.t[t].recipe.seed = (uint8_t)(11 + t);
        mu_fill(&original.t[t].recipe);
        mu_acid_set_seed(&original.t[t].recipe, 0x1234 + t * 0x1111);
        original.t[t].recipe.p[MU_EUCL][0] = (uint8_t)(12 + t);
        original.t[t].recipe.p[MU_EUCL][1] = (uint8_t)(5 + t);
        original.t[t].recipe.p[MU_TURI][1] = (uint8_t)(35 + t);
        original.t[t].recipe.p[MU_TB3P][3] = (uint8_t)(40 + t);
    }
    original.t[1].recipe.p[MU_TURI][MU_PARAMS - 1] = 0;
    mu_evolve(&original.t[1].recipe, 17);
    mu_rt_committed(&original.t[2]); /* a retained TB3P recipe playing native trigs */
    mu_render(&original.t[2].recipe, &key, before);
    {
        struct mu_rt before_save = original;
        mu_store_save(&store, &original);
        CHECK(memcmp(&before_save, &original, sizeof original) == 0,
              "saving changed the live runtime");
    }
    CHECK(mu_store_valid(&store), "saved record is invalid");
    CHECK(store.bytes[0] == 1 && memcmp(store.bytes + FOOTER_OFFSET, "MUT2", 4) == 0,
          "saved header or terminal marker differs");
    CHECK(test_crc32(store.bytes, CRC_OFFSET) ==
              ((uint32_t)store.bytes[CRC_OFFSET] << 24 |
               (uint32_t)store.bytes[CRC_OFFSET + 1] << 16 |
               (uint32_t)store.bytes[CRC_OFFSET + 2] << 8 | store.bytes[CRC_OFFSET + 3]),
          "saved CRC-32 does not match reference");
    CHECK(original.t[1].recipe.src.g[0] != 0 || original.t[1].recipe.src.p[0] != 0,
          "TURI fixture is unexpectedly empty");
    CHECK(mu_store_decode(&loaded, store.bytes, MU_STORE_BYTES) == 1, "valid decode failed");
    for (uint32_t t = 0; t < MU_STORE_TRACKS; t++) {
        struct mu_track expected = original.t[t].recipe;
        CHECK(loaded.t[t].recipe.model == original.t[t].recipe.model, "T%u model changed", t + 1);
        CHECK(loaded.t[t].play == original.t[t].play, "T%u playback source changed", t + 1);
        CHECK(loaded.t[t].recipe.seed == original.t[t].recipe.seed, "T%u seed changed", t + 1);
        CHECK(mu_acid_seed(&loaded.t[t].recipe) == mu_acid_seed(&original.t[t].recipe),
              "T%u TB3P seed changed", t + 1);
        for (uint32_t m = MU_EUCL; m < MU_MODELS; m++)
            CHECK(memcmp(loaded.t[t].recipe.p[m], original.t[t].recipe.p[m], MU_PARAMS) == 0,
                  "T%u model %u controls changed", t + 1, m);
        mu_fill(&expected);
        CHECK(memcmp(&loaded.t[t].recipe.src, &expected.src, sizeof expected.src) == 0,
              "T%u source was not regenerated from its saved seed", t + 1);
    }
    CHECK(memcmp(&loaded.t[1].recipe.src, &original.t[1].recipe.src,
                 sizeof loaded.t[1].recipe.src) != 0,
          "fixture did not exercise the accepted TURI reload change");
    CHECK(loaded.t[2].play == MU_PLAY_NATIVE && !loaded.t[2].undo,
          "committed playback or transient undo restored incorrectly");
    mu_render(&loaded.t[2].recipe, &key, after);
    CHECK(steps_equal(before, after), "TB3P output changed for same seed, controls and context");

    /* Cover seed extremes, density, slide, accent and different keys through the wire format. */
    for (uint32_t trial = 0; trial < 256; trial++) {
        struct mu_track *recipe = &original.t[2].recipe;
        mu_acid_set_seed(recipe, trial * 257u);
        recipe->p[MU_TB3P][1] = (uint8_t)(trial % 8u);
        recipe->p[MU_TB3P][2] = (uint8_t)(trial % 9u);
        recipe->p[MU_TB3P][3] = (uint8_t)(trial % 101u);
        recipe->p[MU_TB3P][4] = (uint8_t)((trial * 7u) % 101u);
        key.scale = (int8_t)((int32_t)(trial % 4u) - 1);
        mu_render(recipe, &key, before);
        mu_store_save(&store, &original);
        CHECK(mu_store_decode(&loaded, store.bytes, MU_STORE_BYTES), "TB3P trial %u failed to load",
              trial);
        mu_render(&loaded.t[2].recipe, &key, after);
        CHECK(steps_equal(before, after), "TB3P trial %u changed on reload", trial);
    }

    reject(NULL, 0, "missing record");
    reject(NULL, RECORD_BYTES, "missing buffer");
    memcpy(wire, store.bytes, RECORD_BYTES);
    memset(wire + RECORD_BYTES, 0xa5, 4);
    for (uint32_t n = 0; n < RECORD_BYTES; n++)
        reject(wire, n, "short record");
    for (uint32_t n = RECORD_BYTES + 1; n <= sizeof wire; n++)
        reject(wire, n, "long record");

    /* Core 4.1 copies whole words up to descriptor capacity and passes only found=1. Its callback
     * cannot distinguish a valid record from the same prefix in a longer outer block. */
    for (uint32_t n = 0; n < RECORD_BYTES; n++) {
        struct mu_store core_buffer;
        memset(&core_buffer, 0, sizeof core_buffer);
        memcpy(core_buffer.bytes, wire, n & ~3u);
        reject(core_buffer.bytes, MU_STORE_BYTES, "core-truncated short record");
    }
    {
        struct mu_store core_buffer;
        memset(&core_buffer, 0, sizeof core_buffer);
        memcpy(core_buffer.bytes, wire, MU_STORE_BYTES);
        CHECK(mu_store_decode(&loaded, core_buffer.bytes, MU_STORE_BYTES) == 1,
              "valid prefix from oversized outer block was not accepted as core delivers it");
    }

    memcpy(bad, store.bytes, sizeof bad);
    bad[0] = 2;
    seal(bad);
    reject(bad, sizeof bad, "unknown version");
    memcpy(bad, store.bytes, sizeof bad);
    bad[2] = 1;
    seal(bad);
    reject(bad, sizeof bad, "reserved header");
    memcpy(bad, store.bytes, sizeof bad);
    bad[HEADER_BYTES + 2 * TRACK_BYTES] = MU_MODELS;
    seal(bad);
    reject(bad, sizeof bad, "unknown model on third track");
    memcpy(bad, store.bytes, sizeof bad);
    bad[HEADER_BYTES + 2 * TRACK_BYTES + PLAY_OFFSET] = 2;
    seal(bad);
    reject(bad, sizeof bad, "unknown playback source on third track");
    memcpy(bad, store.bytes, sizeof bad);
    bad[HEADER_BYTES + CONTROL_OFFSET] ^= 1;
    reject(bad, sizeof bad, "corrupt payload checksum");
    memcpy(bad, store.bytes, sizeof bad);
    bad[CRC_OFFSET] ^= 1;
    reject(bad, sizeof bad, "corrupt checksum");
    memcpy(bad, store.bytes, sizeof bad);
    bad[FOOTER_OFFSET + 3] = 0;
    reject(bad, sizeof bad, "missing terminal byte");
    memcpy(bad, store.bytes, sizeof bad);
    bad[HEADER_BYTES + CONTROL_OFFSET] = 255;           /* EUCL STEP max 65 */
    bad[HEADER_BYTES + CONTROL_OFFSET + MU_PARAMS] = 0; /* TURI LOOP min 2 */
    seal(bad);
    CHECK(mu_store_decode(&loaded, bad, sizeof bad) == 1, "clamp fixture rejected");
    CHECK(loaded.t[0].recipe.p[MU_EUCL][0] == mu_knob_max(MU_EUCL, 1), "EUCL STEP was not clamped");
    CHECK(loaded.t[0].recipe.p[MU_TURI][0] == mu_knob_min(MU_TURI, 1), "TURI LOOP was not clamped");

    /* Fuzz valid framing with arbitrary controls, unknown models/play and checksum corruption. */
    uint32_t rng = UINT32_C(0x753fd129);
    for (uint32_t trial = 0; trial < 100000; trial++) {
        struct mu_rt second;
        int ok, ok2;
        memcpy(bad, store.bytes, sizeof bad);
        for (uint32_t t = 0; t < MU_STORE_TRACKS; t++) {
            uint32_t off = HEADER_BYTES + t * TRACK_BYTES;
            bad[off] = (uint8_t)(random_word(&rng) % (MU_MODELS + 2));
            for (uint32_t j = CONTROL_OFFSET; j < TRACK_BYTES; j++)
                bad[off + j] = (uint8_t)random_word(&rng);
            bad[off + PLAY_OFFSET] = (uint8_t)(random_word(&rng) % 4);
        }
        seal(bad);
        if ((trial & 15u) == 0)
            bad[CRC_OFFSET] ^= 1;
        make_nondefault(&loaded);
        make_nondefault(&second);
        ok = mu_store_decode(&loaded, bad, sizeof bad);
        ok2 = mu_store_decode(&second, bad, sizeof bad);
        CHECK(ok == ok2, "fuzz trial %u gave inconsistent status", trial);
        CHECK(memcmp(&loaded, &second, sizeof loaded) == 0, "fuzz trial %u gave inconsistent state",
              trial);
        if (!ok)
            CHECK(all_off_native(&loaded), "fuzz trial %u partially applied", trial);
    }

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    puts("storage tests passed");
    return 0;
}
