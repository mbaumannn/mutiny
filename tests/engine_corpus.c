/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Deterministic differential corpus for the MUTINY model engine.
 *
 * Compiled twice by tests/test_engine_equivalence.py: once against tests/reference (the reference
 * engine) and once against src/engine (the current engine). Each run prints the model identity and
 * all five fields of all 64 rendered steps, so a comparison is not limited to a hit mask or a
 * checksum.
 *
 * The corpus covers every model, every legal explicit loop length and the TRK position, track
 * lengths 1-64, every supported scale, ROOT unset/0/60/127, NOTE 0/60/127, slow/fast knob extremes,
 * seeds 0/1/255, TB3P's 16-bit seed, parameter pairs, odd lengths and 256 fixed-seed pseudorandom
 * recipes. Every dimension is swept with the other dimensions at a defined default, and the random
 * recipes cross-check the combinations; this is coverage by dimension, not a full cross product.
 */
#include "engine.h"
#include <stdio.h>
#include <stdlib.h>

#ifdef MU_USE_CACHE
/* The production entry point with a caller-owned base cache. The reference build never defines
 * this, so the test also checks the cached current path against the reference engine. */
static struct mu_acid_cache cache;
#endif

static uint32_t rng_state;

static uint32_t rnd(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

static struct mu_track base_track(void) {
    struct mu_track t;
    mu_defaults(&t);
    return t;
}

static void emit_header(const char *label, uint32_t index, const struct mu_track *t,
                        const struct mu_key *k) {
    uint32_t i;
    printf("C %s %u m=%u len=%u note=%u root=%u scale=%d seed=%u acid=%u p=", label, index,
           t->model, k->len, k->note, k->root, (int)k->scale, t->seed, mu_acid_seed(t));
    for (i = 0; i < MU_PARAMS; i++) {
        printf("%s%u", i ? "," : "", t->p[t->model][i]);
    }
    printf("\n");
}

static void emit_steps(const struct mu_step *s) {
    uint32_t i;
    for (i = 0; i < MU_STEPS; i++) {
        printf("S %u %u %u %u %u %u\n", i, s[i].trig, s[i].note, s[i].vel, s[i].slide, s[i].hold);
    }
}

static void emit(const char *label, uint32_t index, const struct mu_track *t,
                 const struct mu_key *k) {
#ifdef MU_USE_CACHE
    /* Every case is rendered twice on the same track and inputs: once cold (reset) and once as an
     * immediate hit. Both records are printed, so the Python comparison checks each against the
     * reference engine. A miss-only corpus would not prove hit output. */
    struct mu_step cold[MU_STEPS], hit[MU_STEPS];
    uint32_t i;
    mu_acid_cache_reset(&cache);
    mu_render_cached(t, k, 0, &cache, cold);
    mu_render_cached(t, k, 0, &cache, hit);
    for (i = 0; i < MU_STEPS; i++) {
        if (cold[i].trig != hit[i].trig || cold[i].note != hit[i].note ||
            cold[i].vel != hit[i].vel || cold[i].slide != hit[i].slide ||
            cold[i].hold != hit[i].hold) {
            printf("cached cold/hit mismatch at case %u step %u\n", index, i);
            exit(2);
        }
    }
    emit_header(label, index, t, k);
    emit_steps(cold);
    emit_header(label, index, t, k);
    emit_steps(hit);
#else
    struct mu_step s[MU_STEPS];
    mu_render(t, k, s);
    emit_header(label, index, t, k);
    emit_steps(s);
#endif
}

int main(void) {
    static const uint8_t pairs[MU_MODELS][4][2] = {
        {{0, 0}, {0, 0}, {0, 0}, {0, 0}},
        {{0, 1}, {0, 2}, {1, 5}, {0, 6}},
        {{0, 1}, {0, 4}, {1, 3}, {0, 5}},
        {{0, 1}, {0, 2}, {0, 3}, {1, 4}},
    };
    static const uint8_t roots[4] = {0xff, 0, 60, 127};
    static const uint8_t notes[3] = {0, 60, 127};
    struct mu_key key = {60, 60, 5, 16};
    uint32_t index = 0, m, i, k, v;

    for (m = 0; m < MU_MODELS; m++) {
        struct mu_track t = base_track();
        t.model = (uint8_t)m;
        emit("default", index++, &t, &key);
    }

    /* every legal explicit loop length, then the TRK position */
    for (m = 1; m < MU_MODELS; m++) {
        uint32_t cap = mu_model_cap((uint8_t)m);
        struct mu_key k64 = key;
        k64.len = 64;
        for (v = 1; v <= cap + 1u; v++) {
            struct mu_track t = base_track();
            t.model = (uint8_t)m;
            t.p[m][0] = (uint8_t)v;
            emit("loop", index++, &t, &k64);
        }
    }

    /* track lengths 1-64 at the model default and at TRK */
    for (m = 1; m < MU_MODELS; m++) {
        uint32_t cap = mu_model_cap((uint8_t)m);
        for (i = 0; i < MU_STEPS; i++) {
            uint32_t j;
            for (j = 0; j < 2; j++) {
                struct mu_track t = base_track();
                struct mu_key kc = key;
                t.model = (uint8_t)m;
                t.p[m][0] = (uint8_t)(j ? cap + 1u : (uint32_t)mu_knob_default((uint8_t)m, 1));
                kc.len = (uint8_t)(i + 1u);
                emit("track", index++, &t, &kc);
            }
        }
    }

    /* every supported scale, including chromatic (-1) */
    for (m = 1; m < MU_MODELS; m++) {
        int32_t sc;
        for (sc = -1; sc <= 34; sc++) {
            struct mu_track t = base_track();
            struct mu_key kc = key;
            t.model = (uint8_t)m;
            kc.scale = (int8_t)sc;
            emit("scale", index++, &t, &kc);
        }
    }

    /* ROOT unset/0/60/127 against NOTE 0/60/127 */
    for (m = 1; m < MU_MODELS; m++) {
        for (i = 0; i < 4; i++) {
            for (k = 0; k < 3; k++) {
                struct mu_track t = base_track();
                struct mu_key kc = key;
                t.model = (uint8_t)m;
                kc.root = roots[i];
                kc.note = notes[k];
                emit("key", index++, &t, &kc);
            }
        }
    }

    /* knob extremes: min, mid, max and default for each control */
    for (m = 1; m < MU_MODELS; m++) {
        for (k = 1; k <= MU_PARAMS; k++) {
            int32_t lo = mu_knob_min((uint8_t)m, (uint8_t)k);
            int32_t hi = mu_knob_max((uint8_t)m, (uint8_t)k);
            int32_t values[4];
            uint32_t j;
            values[0] = lo;
            values[1] = (lo + hi) / 2;
            values[2] = hi;
            values[3] = mu_knob_default((uint8_t)m, (uint8_t)k);
            for (j = 0; j < 4; j++) {
                struct mu_track t = base_track();
                t.model = (uint8_t)m;
                t.p[m][k - 1] = (uint8_t)values[j];
                emit("knob", index++, &t, &key);
            }
        }
    }

    /* parameter pairs at min/mid/max, which exercises odd lengths and extreme density */
    for (m = 1; m < MU_MODELS; m++) {
        for (i = 0; i < 4; i++) {
            uint8_t ka = pairs[m][i][0], kb = pairs[m][i][1];
            uint32_t a, b;
            for (a = 0; a < 3; a++) {
                for (b = 0; b < 3; b++) {
                    int32_t alo = mu_knob_min((uint8_t)m, ka + 1);
                    int32_t ahi = mu_knob_max((uint8_t)m, ka + 1);
                    int32_t blo = mu_knob_min((uint8_t)m, kb + 1);
                    int32_t bhi = mu_knob_max((uint8_t)m, kb + 1);
                    struct mu_track t = base_track();
                    t.model = (uint8_t)m;
                    t.p[m][ka] = (uint8_t)(a == 0 ? alo : a == 1 ? (alo + ahi) / 2 : ahi);
                    t.p[m][kb] = (uint8_t)(b == 0 ? blo : b == 1 ? (blo + bhi) / 2 : bhi);
                    emit("pair", index++, &t, &key);
                }
            }
        }
    }

    /* seed counters and TB3P's 16-bit seed */
    for (m = 1; m < MU_MODELS; m++) {
        static const uint8_t seeds[3] = {0, 1, 255};
        for (i = 0; i < 3; i++) {
            struct mu_track t = base_track();
            t.model = (uint8_t)m;
            t.seed = seeds[i];
            emit("seed", index++, &t, &key);
        }
    }
    {
        static const uint16_t acid_seeds[2] = {0, 65535};
        for (i = 0; i < 2; i++) {
            struct mu_track t = base_track();
            t.model = MU_TB3P;
            mu_acid_set_seed(&t, acid_seeds[i]);
            emit("acid-seed", index++, &t, &key);
        }
    }

    /* OFF with different keys */
    for (i = 0; i < 4; i++) {
        struct mu_track t = base_track();
        struct mu_key kc = key;
        kc.scale = i == 0 ? -1 : (int8_t)i;
        kc.len = (uint8_t)(i * 16u + 1u);
        emit("off", index++, &t, &kc);
    }

    /* 256 fixed-seed pseudorandom recipes */
    rng_state = 0x13579BDFu;
    for (i = 0; i < 256; i++) {
        struct mu_track t = base_track();
        struct mu_key kc;
        uint32_t j;
        t.model = (uint8_t)(1u + rnd() % 3u);
        t.seed = (uint8_t)(rnd() & 0xffu);
        mu_acid_set_seed(&t, rnd() & 0xffffu);
        for (j = 0; j < MU_SRC; j++) {
            t.src.g[j] = (uint8_t)(rnd() & 0xffu);
            t.src.p[j] = (uint8_t)(rnd() & 0xffu);
            t.src.a[j] = (uint8_t)(rnd() & 0xffu);
        }
        for (k = 1; k <= MU_PARAMS; k++) {
            int32_t lo = mu_knob_min(t.model, (uint8_t)k);
            int32_t hi = mu_knob_max(t.model, (uint8_t)k);
            t.p[t.model][k - 1] = (uint8_t)(lo + (int32_t)(rnd() % (uint32_t)(hi - lo + 1)));
        }
        kc.note = (uint8_t)(rnd() & 0x7fu);
        kc.root = (rnd() & 1u) ? 0xff : (uint8_t)(rnd() & 0x7fu);
        kc.scale = (rnd() & 1u) ? -1 : (int8_t)(rnd() % 35u);
        kc.len = (uint8_t)(1u + rnd() % 64u);
        emit("random", index++, &t, &kc);
    }

    printf("END %u\n", index);
    return 0;
}
