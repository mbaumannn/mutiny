/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host tests of the caller-owned TB3P derived-base cache.
 *
 * The pure engine entry points are the reference: every cached render and picture is compared field
 * by field with mu_render/mu_preview, and every base with mu_acid_generate. The probe counts prove
 * the invalidation rule: exactly the seeded inputs (seed, GATE, PTCH, scale size, T3/T4 mode)
 * regenerate, while LEN, SLID, ACNT, TRNS, ROOT/NOTE and a same-size scale swap only remap. No
 * target code. */
#include "../src/engine/engine.h"
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

#ifdef MU_ENGINE_PROBE
extern uint32_t mu_engine_acid_generations;
#else
static uint32_t mu_engine_acid_generations;
#endif

static int same_steps(const struct mu_step *a, const struct mu_step *b) {
    uint32_t i;
    for (i = 0; i < MU_STEPS; i++)
        if (a[i].trig != b[i].trig || a[i].note != b[i].note || a[i].vel != b[i].vel ||
            a[i].slide != b[i].slide || a[i].hold != b[i].hold)
            return 0;
    return 1;
}

static int same_base(const struct mu_acid *a, const struct mu_acid *b) {
    return a->gates == b->gates && a->slides == b->slides && a->accents == b->accents &&
           a->oct_ups == b->oct_ups && a->oct_downs == b->oct_downs &&
           memcmp(a->notes, b->notes, sizeof a->notes) == 0 &&
           memcmp(a->slide_r, b->slide_r, sizeof a->slide_r) == 0;
}

static int same_preview(const struct mu_preview *a, const struct mu_preview *b) {
    return a->gates == b->gates && a->play == b->play && a->accent == b->accent &&
           a->slide == b->slide && a->len == b->len && a->cells == b->cells && a->start == b->start;
}

static uint8_t other(uint8_t v, int32_t lo, int32_t hi) {
    return (uint8_t)(v < hi ? v + 1 : v > lo ? v - 1 : v);
}

/* One cached render must equal the pure render for every field of all 64 steps. -> the base
 * generations the cached call itself made (the pure render's own local generation is excluded). */
static uint32_t check_render(const struct mu_track *t, const struct mu_key *k, uint8_t track,
                             struct mu_acid_cache *c, const char *what) {
    struct mu_step pure[MU_STEPS], cached[MU_STEPS];
    uint32_t before;
    mu_render(t, k, pure);
    before = mu_engine_acid_generations;
    mu_render_cached(t, k, track, c, cached);
    CHECK(same_steps(pure, cached), "%s: cached render differs from the pure engine", what);
    return mu_engine_acid_generations - before;
}

static uint32_t check_preview(const struct mu_track *t, const struct mu_key *k, uint8_t track,
                              struct mu_acid_cache *c, const char *what) {
    struct mu_preview pure, cached;
    uint32_t before;
    mu_preview(t, k, &pure);
    before = mu_engine_acid_generations;
    mu_preview_cached(t, k, track, c, &cached);
    CHECK(same_preview(&pure, &cached), "%s: cached picture differs from the pure engine", what);
    return mu_engine_acid_generations - before;
}

int main(void) {
    struct mu_acid_cache c;
    struct mu_track t;
    struct mu_key k = {60, 60, 5, 16};
    struct mu_acid direct;
    uint32_t gen, i, s;

    mu_defaults(&t);
    t.model = MU_TB3P;
    mu_acid_cache_reset(&c);

    /* Cold, hit and every seeded dependency: each changed input regenerates exactly once. */
    {
        uint32_t n = mu_scale_notes(k.scale, (uint8_t[12]){0});
        uint16_t seed = mu_acid_seed(&t);
        mu_acid_cache_reset(&c);
        gen = mu_engine_acid_generations;
        CHECK(mu_acid_cache_prepare(&c, 0, seed, 5, 8, n, 0) == 1, "cold generation");
        CHECK(mu_engine_acid_generations == gen + 1, "cold made %u generations",
              mu_engine_acid_generations - gen);
        mu_acid_generate(seed, 5, 8, n, 0, &direct);
        CHECK(same_base(&c.acid, &direct), "cold base differs from mu_acid_generate");
        gen = mu_engine_acid_generations;
        CHECK(mu_acid_cache_prepare(&c, 0, seed, 5, 8, n, 0) == 0, "hit regenerated");
        CHECK(mu_engine_acid_generations == gen, "a hit made %u generations",
              mu_engine_acid_generations - gen);

        {
            struct {
                uint8_t what; /* 0 seed, 1 gate, 2 ptch, 3 size, 4 mode, 5 track */
                uint16_t seed;
                uint32_t gate, ptch, size;
                uint8_t mode, track;
            } miss[] = {
                {0, (uint16_t)(seed + 1u), 5, 8, n, 0, 0},
                {1, seed, 6, 8, n, 0, 0},
                {2, seed, 5, 7, n, 0, 0},
                {3, seed, 5, 8, n + 1u, 0, 0},
                {4, seed, 5, 8, n, 1, 0},
                {5, seed, 5, 8, n, 0, 1},
            };
            for (i = 0; i < sizeof miss / sizeof miss[0]; i++) {
                gen = mu_engine_acid_generations;
                CHECK(mu_acid_cache_prepare(&c, miss[i].track, miss[i].seed, miss[i].gate,
                                            miss[i].ptch, miss[i].size, miss[i].mode) == 1,
                      "dependency %u did not miss", miss[i].what);
                CHECK(mu_engine_acid_generations == gen + 1, "dependency %u made %u generations",
                      miss[i].what, mu_engine_acid_generations - gen);
                mu_acid_generate(miss[i].seed, miss[i].gate, miss[i].ptch, miss[i].size,
                                 miss[i].mode, &direct);
                CHECK(same_base(&c.acid, &direct), "dependency %u base differs", miss[i].what);
            }
        }
        /* One entry, not a history: going back to a previous value is a miss again. */
        gen = mu_engine_acid_generations;
        CHECK(mu_acid_cache_prepare(&c, 0, seed, 5, 8, n, 0) == 1, "returning did not miss");
        CHECK(mu_engine_acid_generations == gen + 1, "returning made %u generations",
              mu_engine_acid_generations - gen);
        mu_acid_cache_reset(&c);
        CHECK(mu_acid_cache_prepare(&c, 0, seed, 5, 8, n, 0) == 1, "a reset did not invalidate");
    }

    /* Seeds 0 and 65535 are distinct keys and both match the direct generator. */
    {
        uint32_t n = mu_scale_notes(k.scale, (uint8_t[12]){0});
        mu_acid_cache_reset(&c);
        mu_acid_set_seed(&t, 0);
        CHECK(check_render(&t, &k, 0, &c, "seed 0") == 1, "seed 0 did not generate");
        mu_acid_generate(0, 5, 8, n, 0, &direct);
        CHECK(same_base(&c.acid, &direct), "seed 0 base differs");
        mu_acid_set_seed(&t, 0xffff);
        CHECK(check_render(&t, &k, 0, &c, "seed 65535") == 1, "seed 65535 did not generate once");
        mu_acid_generate(0xffff, 5, 8, n, 0, &direct);
        CHECK(same_base(&c.acid, &direct), "seed 65535 base differs");
        mu_acid_set_seed(&t, 0);
        CHECK(check_render(&t, &k, 0, &c, "seed 0 again") == 1, "returning to seed 0 hit");
    }

    /* Every scale, key and loop: cached output always equals the pure render. A same-size scale
     * change remaps only; a note-count change regenerates. */
    {
        mu_acid_cache_reset(&c);
        CHECK(check_render(&t, &k, 0, &c, "default TB3P") == 1, "the default did not generate");
        for (s = 0; s < 36; s++) {
            struct mu_key kk = k;
            struct mu_step warm[MU_STEPS];
            uint32_t n = mu_scale_notes(k.scale, (uint8_t[12]){0});
            uint32_t nn;
            kk.scale = (int8_t)((int32_t)s - 1);
            nn = mu_scale_notes(kk.scale, (uint8_t[12]){0});
            mu_render_cached(&t, &k, 0, &c, warm); /* the same warm base for every case */
            CHECK(check_render(&t, &kk, 0, &c, "scale") == (nn != n),
                  "scale %d made the wrong number of generations", kk.scale);
            CHECK(check_preview(&t, &kk, 0, &c, "scale picture") == 0,
                  "the picture after the render regenerated a base");
        }
    }
    {
        static const uint8_t roots[4] = {0xff, 0, 60, 127};
        static const uint8_t notes[3] = {0, 60, 127};
        static const uint8_t lens[8] = {0, 1, 7, 8, 16, 17, 33, 64};
        for (i = 0; i < 4; i++)
            for (s = 0; s < 3; s++) {
                struct mu_key kk = k;
                kk.root = roots[i];
                kk.note = notes[s];
                CHECK(check_render(&t, &kk, 0, &c, "key") == 0,
                      "a ROOT/NOTE change regenerated a base");
                CHECK(check_preview(&t, &kk, 0, &c, "key picture") == 0,
                      "a ROOT/NOTE picture regenerated a base");
            }
        for (i = 0; i < 8; i++) {
            struct mu_key kk = k;
            kk.len = lens[i];
            CHECK(check_render(&t, &kk, 0, &c, "track length") == 0,
                  "a track-length change regenerated a base");
        }
    }

    /* Every TB3P control: no seeded generation for LEN/SLID/ACNT/TRNS, exactly one for GATE/PTCH
     * and the seed. */
    {
        for (i = 0; i < MU_PARAMS; i++) {
            struct mu_track tt = t;
            struct mu_step warm[MU_STEPS];
            uint32_t expect;
            mu_render_cached(&t, &k, 0, &c, warm); /* each control starts from the same warm base */
            if (i == 6) {
                mu_acid_set_seed(&tt, (uint32_t)(mu_acid_seed(&tt) + 1u));
            } else {
                tt.p[MU_TB3P][i] = other(tt.p[MU_TB3P][i], mu_knob_min(MU_TB3P, (uint8_t)(i + 1)),
                                         mu_knob_max(MU_TB3P, (uint8_t)(i + 1)));
            }
            expect = (i == 1 || i == 2 || i == 6) ? 1u : 0u;
            CHECK(check_render(&tt, &k, 0, &c, "control") == expect,
                  "control %u made the wrong number of generations", i + 1);
            CHECK(check_preview(&tt, &k, 0, &c, "control picture") == 0,
                  "control %u's picture regenerated a base", i + 1);
        }
    }

    /* Repeated reroll: every new seed regenerates once and stays equivalent. */
    {
        uint32_t j;
        for (j = 0; j < 10; j++) {
            struct mu_track tt = t;
            mu_reroll(&tt);
            CHECK(check_render(&tt, &k, 0, &c, "reroll") == 1, "reroll %u did not generate once",
                  j);
            t = tt;
        }
    }

    /* Model switch and back: a non-TB3P render leaves the base alone; the same seeded inputs are a
     * hit when TB3P returns. */
    {
        struct mu_track tt = t;
        CHECK(check_render(&tt, &k, 0, &c, "TB3P before switch") == 0,
              "the warm TB3P render regenerated");
        tt.model = MU_EUCL;
        CHECK(check_render(&tt, &k, 0, &c, "EUCL") == 0, "EUCL touched the TB3P base");
        tt.model = MU_TURI;
        CHECK(check_render(&tt, &k, 0, &c, "TURI") == 0, "TURI touched the TB3P base");
        tt.model = MU_TB3P;
        CHECK(check_render(&tt, &k, 0, &c, "TB3P back") == 0,
              "a model return regenerated the base");
    }

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("acid cache tests passed\n");
    return 0;
}
