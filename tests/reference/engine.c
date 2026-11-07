/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "engine.h"

/* ---- scales -----------------------------------------------------------------------------------
 */
/* The Digitone's keyboard scales after CHROMATIC, in the manual's order (APPENDIX D). Rows whose
 * intervals were read back from the OS 1.44 table (in RAM at 0x401d42a0) are marked [fw]; the
 * others use the standard interval sets for their names and are not verified against the unit. */
#define S(...) {__VA_ARGS__, 255}
static const uint8_t scales[][9] = {
    S(0, 2, 4, 5, 7, 9, 11),    /*  0 IONIAN (MAJOR)          [fw] */
    S(0, 2, 3, 5, 7, 9, 10),    /*  1 DORIAN                  [fw] */
    S(0, 1, 3, 5, 7, 8, 10),    /*  2 PHRYGIAN                [fw] */
    S(0, 2, 4, 6, 7, 9, 11),    /*  3 LYDIAN                  [fw] */
    S(0, 2, 4, 5, 7, 9, 10),    /*  4 MIXOLYDIAN              [fw] */
    S(0, 2, 3, 5, 7, 8, 10),    /*  5 AEOLIAN (MINOR)         [fw] */
    S(0, 1, 3, 5, 6, 8, 10),    /*  6 LOCRIAN                 [fw] */
    S(0, 3, 5, 7, 10),          /*  7 PENTATONIC MINOR */
    S(0, 2, 4, 7, 9),           /*  8 PENTATONIC MAJOR */
    S(0, 2, 3, 5, 7, 9, 11),    /*  9 MELODIC MINOR           [fw] */
    S(0, 2, 3, 5, 7, 8, 11),    /* 10 HARMONIC MINOR          [fw] */
    S(0, 2, 4, 6, 8, 10),       /* 11 WHOLE TONE              [fw] */
    S(0, 3, 5, 6, 7, 10),       /* 12 BLUES */
    S(0, 2, 3, 5, 7, 8, 9, 10), /* 13 COMBO MINOR (guess) */
    S(0, 1, 4, 5, 6, 8, 11),    /* 14 PERSIAN */
    S(0, 1, 5, 6, 10),          /* 15 IWATO */
    S(0, 1, 5, 7, 10),          /* 16 IN-SEN */
    S(0, 2, 3, 7, 8),           /* 17 HIRAJOSHI */
    S(0, 1, 3, 7, 8),           /* 18 PELOG */
    S(0, 1, 4, 5, 7, 8, 10),    /* 19 PHRYGIAN DOMINANT      [fw] */
    S(0, 2, 3, 5, 6, 8, 9, 11), /* 20 WHOLE-HALF DIMINISHED */
    S(0, 1, 3, 4, 6, 7, 9, 10), /* 21 HALF-WHOLE DIMINISHED */
    S(0, 1, 3, 4, 5, 7, 8, 10), /* 22 SPANISH (guess) */
    S(0, 2, 4, 5, 6, 8, 10),    /* 23 MAJOR LOCRIAN          [fw] */
    S(0, 1, 3, 4, 6, 8, 10),    /* 24 SUPER LOCRIAN          [fw] */
    S(0, 1, 3, 5, 7, 9, 10),    /* 25 DORIAN b2              [fw] */
    S(0, 2, 4, 6, 8, 9, 11),    /* 26 LYDIAN AUGMENTED       [fw] */
    S(0, 2, 4, 6, 7, 9, 10),    /* 27 LYDIAN DOMINANT        [fw] */
    S(0, 1, 4, 5, 7, 8, 11),    /* 28 DOUBLE HARMONIC MAJOR  [fw] */
    S(0, 3, 4, 6, 7, 10, 11),   /* 29 LYDIAN #2 #6           [fw] */
    S(0, 1, 3, 4, 7, 8, 9),     /* 30 ULTRAPHRYGIAN          [fw] */
    S(0, 2, 3, 6, 7, 8, 11),    /* 31 HUNGARIAN MINOR        [fw] */
    S(0, 1, 4, 5, 6, 9, 10),    /* 32 ORIENTAL               [fw] */
    S(0, 3, 4, 5, 8, 9, 11),    /* 33 IONIAN #2 #5           [fw] */
    S(0, 1, 2, 5, 6, 8, 9),     /* 34 LOCRIAN bb3 bb7        [fw] */
};
#undef S
#define N_SCALES (sizeof scales / sizeof scales[0])

uint32_t mu_scale_notes(int8_t scale, uint8_t out[12]) {
    uint32_t n = 0;
    if (scale < 0 || (uint32_t)scale >= N_SCALES) { /* chromatic, or an index we do not know */
        for (; n < 12; n++)
            out[n] = (uint8_t)n;
        return 12;
    }
    while (scales[scale][n] != 255 && n < 9) {
        out[n] = scales[scale][n];
        n++;
    }
    return n;
}

/* The note for a scale degree: the scale is rooted on the key's ROOT, in the register of NOTE
 * (the highest ROOT-class note at or below NOTE), degrees rising through the octaves. */
static uint8_t pitch(const struct mu_key *key, const uint8_t *ivs, uint32_t n, uint32_t degree) {
    int32_t root = key->root == 0xff ? key->note : key->root;
    int32_t base = key->note - (((int32_t)key->note - root + 1200) % 12);
    int32_t note = base + ivs[degree % n] + 12 * (int32_t)(degree / n);
    return (uint8_t)(note < 0 ? 0 : note > 127 ? 127 : note);
}

/* ---- deterministic randomness -----------------------------------------------------------------
 */
static uint32_t mix(uint32_t seed, uint32_t i) {
    uint32_t x = seed * 0x9E3779B1u + i * 0x85EBCA6Bu + 0x6D2B79F5u;
    x ^= x >> 15;
    x *= 0x2C1B3C6Du;
    x ^= x >> 12;
    x *= 0x297A2D39u;
    x ^= x >> 15;
    return x;
}

static uint32_t chance(uint32_t seed, uint32_t salt, uint32_t percent) {
    return percent && (mix(seed, salt) % 100u) < percent;
}

/* ---- the controls -----------------------------------------------------------------------------
 */
/* EUCL  B STEP  C HITS  D ROT   E DEGR  F ACNT  G FILL  H RIFF
 * TURI  B LOOP  C DENS  D RANG  E ACNT  F ROT   G TRNS  H LOCK
 * TB3P  B LEN   C GATE  D PTCH  E SLID  F ACNT  G TRNS  H SEED (16 bits, in acid[]) */
/* step: detents a turn needs for one unit (4 counts is one detent); fast: what a turn of more than
 * one detent multiplies its step by. A knob that already needs several detents to move at all is
 * fine-grained and does not accelerate; the 0-100 controls and STEP/LEN move a unit a detent and
 * accelerate, as they did, and SEED travels fastest. */
struct knob {
    const char *label;
    int16_t min, max, def;
    uint8_t step, fast;
};
static const struct knob eucl[MU_PARAMS] = {
    {"STEP", 1, 65, 16, 1, 3}, {"HITS", 0, 64, 5, 2, 1},  {"ROT", 0, 63, 0, 2, 1},
    {"DEGR", 0, 14, 0, 2, 1},  {"ACNT", 0, 100, 0, 1, 3}, {"FILL", 0, 100, 0, 1, 3},
    {"RIFF", 0, 100, 0, 1, 3},
};
static const struct knob turi[MU_PARAMS] = {
    {"LOOP", 2, 17, 8, 2, 1},   {"DENS", 0, 100, 70, 1, 3}, {"RANG", 1, 3, 1, 8, 1},
    {"ACNT", 0, 100, 0, 1, 3},  {"ROT", 0, 15, 0, 2, 1},    {"TRNS", 0, 14, 0, 2, 1},
    {"LOCK", 0, 100, 85, 1, 3},
};

static const struct knob tb3p[MU_PARAMS] = {
    /* TRNS 0-14 is -7..+7; H is the 16-bit seed */
    {"LEN", 1, 33, 16, 1, 3},   {"GATE", 0, 7, 5, 4, 1},    {"PTCH", 0, 8, 8, 4, 1},
    {"SLID", 0, 100, 50, 1, 3}, {"ACNT", 0, 100, 60, 1, 3}, {"TRNS", 0, 14, 7, 2, 1},
    {"SEED", 0, 0, 0, 1, 8},
};

static const struct knob *knobs(uint8_t model) {
    return model == MU_EUCL ? eucl : model == MU_TURI ? turi : model == MU_TB3P ? tb3p : 0;
}

const char *mu_model_name(uint8_t model) {
    return model == MU_EUCL   ? "EUCL"
           : model == MU_TURI ? "TURI"
           : model == MU_TB3P ? "TB3P"
                              : "OFF";
}

int32_t mu_knob_min(uint8_t model, uint8_t k) {
    if (k == 0)
        return MU_OFF;
    return knobs(model) && k <= MU_PARAMS ? knobs(model)[k - 1].min : 0;
}

int32_t mu_knob_max(uint8_t model, uint8_t k) {
    if (k == 0)
        return MU_MODELS - 1;
    return knobs(model) && k <= MU_PARAMS ? knobs(model)[k - 1].max : 0;
}

const char *mu_knob_label(uint8_t model, uint8_t k) {
    if (k == 0)
        return "MODEL";
    return knobs(model) && k <= MU_PARAMS ? knobs(model)[k - 1].label : "";
}

#define MODEL_STEP 3 /* a model is three detents: the coarsest control on the page */
#define MODEL_FAST 1
int32_t mu_knob_step(uint8_t model, uint8_t k) {
    if (k == 0)
        return MODEL_STEP;
    return knobs(model) && k <= MU_PARAMS ? knobs(model)[k - 1].step : 1;
}

int32_t mu_knob_default(uint8_t model, uint8_t k) {
    if (k == 0)
        return MU_OFF; /* MODEL's reset is OFF, as every model's own default is its knob default */
    return knobs(model) && k <= MU_PARAMS ? knobs(model)[k - 1].def : 0;
}

int32_t mu_knob_fast(uint8_t model, uint8_t k) {
    if (k == 0)
        return MODEL_FAST;
    return knobs(model) && k <= MU_PARAMS ? knobs(model)[k - 1].fast : 1;
}

void mu_fill(struct mu_track *t) {
    uint32_t i, s = (uint32_t)t->seed + 1u;
    for (i = 0; i < MU_SRC; i++) {
        t->src.g[i] = (uint8_t)(mix(s, i * 3u) >> 24);
        t->src.p[i] = (uint8_t)(mix(s, i * 3u + 1u) >> 24);
        t->src.a[i] = (uint8_t)(mix(s, i * 3u + 2u) >> 24);
    }
}

void mu_reroll(struct mu_track *t) {
    t->seed = (uint8_t)(t->seed + 1u);
    if (t->model == MU_TB3P) { /* a new seed, as the applet's reseed; the source stays */
        mu_acid_set_seed(t, mix((uint32_t)mu_acid_seed(t) << 8 | t->seed, 0xAC1Du) >> 16);
        return;
    }
    mu_fill(t);
}

void mu_evolve(struct mu_track *t, uint32_t pass) {
    uint32_t i, lock = t->model ? MU_LOCK(t) : 100u;
    uint32_t s = ((uint32_t)t->seed + 1u) * 0x10001u + pass * 0x9E37u;
    if (lock >= 100)
        return;
    for (i = 0; i < MU_SRC; i++) {
        if (chance(s, i + 1000u, 100u - lock)) {
            t->src.g[i] = (uint8_t)(mix(s, i * 3u + 2000u) >> 24);
            t->src.p[i] = (uint8_t)(mix(s, i * 3u + 2001u) >> 24);
            t->src.a[i] = (uint8_t)(mix(s, i * 3u + 2002u) >> 24);
        }
    }
}

void mu_defaults(struct mu_track *t) {
    uint32_t m, i;
    t->model = MU_OFF;
    t->seed = 0;
    mu_acid_set_seed(t, 0xAC1Du); /* the applet starts on a random seed; this one is fixed */
    for (m = 0; m < MU_MODELS; m++)
        for (i = 0; i < MU_PARAMS; i++)
            t->p[m][i] = knobs((uint8_t)m) ? (uint8_t)knobs((uint8_t)m)[i].def : 0;
    mu_fill(t);
}

void mu_clamp(struct mu_track *t) {
    uint32_t m, i;
    if (t->model >= MU_MODELS)
        t->model = MU_OFF;
    for (m = 1; m < MU_MODELS; m++) {
        const struct knob *k = knobs((uint8_t)m);
        for (i = 0; i < MU_PARAMS; i++) {
            if (t->p[m][i] < k[i].min)
                t->p[m][i] = (uint8_t)k[i].min;
            if (t->p[m][i] > k[i].max)
                t->p[m][i] = (uint8_t)k[i].max;
        }
    }
    /* No dependent clamps: one knob never moves another. EUCL's render caps its hits at the step
     * count and TURI's takes ROT modulo its loop, so a value out of range plays the same and stays
     * where the user put it when the length comes back. */
}

/* x of 255 against a percentage: x*100 < pct*256, so 100 passes every value and 0 none, and a
 * larger percentage only ever admits more. */
int mu_below(uint32_t x, uint32_t pct) { return x * 100u < pct * 256u; }
#define below mu_below

/* ---- one length and one preview coordinate ----------------------------------------------------
 */
uint32_t mu_model_cap(uint8_t model) {
    return model == MU_EUCL ? MU_STEPS : model == MU_TURI ? MU_SRC : model == MU_TB3P ? 32 : 0;
}

uint32_t mu_model_default(uint8_t model) { return model == MU_TURI ? 8 : model ? 16 : 0; }

/* The length control's top position + 1 means TRK: follow the track's pattern length as far as the
 * model can hold it. A model's cap is a real maximum here, never a sentinel passed on to an index;
 * an unknown track length falls back to the model's own default, so the loop is never 0. */
uint32_t mu_loop_len(uint8_t model, uint32_t v, const struct mu_key *key) {
    uint32_t cap = mu_model_cap(model), len;
    if (!cap)
        return 0;
    len = v > cap ? key->len : v; /* TRK, or a hand-set length */
    if (!len)
        len = v > cap ? mu_model_default(model) : 1;
    if (model == MU_TURI && len < 2)
        len = 2; /* the shift register's shortest useful loop */
    return len > cap ? cap : len;
}

/* EUCL carries ROT as "the hits move to a later step", TURI as "the loop starts later"; each is
 * this one mapping, applied by the render and by every picture. */
uint32_t mu_loop_pos(uint8_t model, uint32_t len, uint32_t rot, uint32_t step) {
    if (!len)
        return 0;
    rot %= len;
    if (model == MU_EUCL)
        return (step + len - rot) % len;
    return (step + rot) % len;
}

/* ---- the models -------------------------------------------------------------------------------
 */
/* ACNT is the accent's depth: at 0 every hit plays at VEL_FLAT (the Digitone's default trig VEL),
 * at 100 an accented hit plays at 127 and the others drop to 40, so the accents carry even on a
 * sound that follows velocity weakly. FILL's ghost hits play at half the unaccented level; TURI
 * accents up to half its notes (ACNT / 2 %), so its contrast stays at 100. */
static uint8_t vel_plain(uint32_t acnt) { return (uint8_t)(MU_VEL_FLAT - acnt * 60u / 100u); }
static uint8_t vel_accent(uint32_t acnt) { return (uint8_t)(MU_VEL_FLAT + acnt * 27u / 100u); }
int mu_step_accent(const struct mu_step *s) { return s->vel > MU_VEL_FLAT; }

/* RIFF's line: the degree of each source step, from a walk over the 16 source draws, so
 * consecutive hits are one or two scale steps apart — a line rather than independent jumps. The
 * walk depends on the source and the scale only, so raising RIFF adds hits and never moves a note.
 */
void mu_riff_degrees(const struct mu_track *t, uint32_t n, uint8_t out[MU_SRC]) {
    int32_t cur = 0;
    uint32_t i;
    if (!n)
        n = 1;
    for (i = 0; i < MU_SRC; i++) {
        cur += (int32_t)(t->src.p[i] % 5u) - 2; /* -2..+2 degrees a step */
        while (cur < 0)
            cur += (int32_t)n;
        while (cur >= (int32_t)n)
            cur -= (int32_t)n;
        out[i] = (uint8_t)cur;
    }
}

static void render_eucl(const struct mu_track *t, const struct mu_key *key, struct mu_step *out) {
    const uint8_t *p = t->p[MU_EUCL];
    uint8_t ivs[12];
    uint32_t n = mu_scale_notes(key->scale, ivs);
    uint32_t len = mu_loop_len(MU_EUCL, p[0], key), hits = p[1], rot = p[2], degree = p[3];
    uint32_t acnt = p[4], fill = p[5];
    uint32_t riff = p[6], i;
    uint8_t walk[MU_SRC];
    if (hits > len)
        hits = len;
    mu_riff_degrees(t, n, walk); /* RIFF's notes: a walk over the source, not independent draws */
    for (i = 0; i < MU_STEPS; i++) {
        uint32_t c = i % len;
        uint32_t phase = mu_loop_pos(MU_EUCL, len, rot, i); /* the rotated position */
        uint32_t si = phase % MU_SRC;
        int32_t deg = (int32_t)degree;
        /* RIFF: a hit takes a note of its own from the 16-step source. src.a decides which hits do
         * (EUCL accents on the beat, so it is free here) and the walk over src.p gives the degree,
         * 0 to n-1: an octave above DEGR. RIFF 0 leaves every hit on DEGR. */
        if (below(t->src.a[si], riff))
            deg += (int32_t)walk[si];
        out[i].note = pitch(key, ivs, n, (uint32_t)deg);
        if ((phase * hits) % len < hits) { /* Bjorklund-style spread: the rotated E(hits, len) */
            out[i].trig = 1;               /* accent the hits on the beat */
            out[i].vel = (c & 3u) == 0 ? vel_accent(acnt) : vel_plain(acnt);
        } else if (below(t->src.g[si], fill)) { /* ghost hits: FILL only adds them */
            out[i].trig = 1;
            out[i].vel = (uint8_t)(vel_plain(acnt) / 2u);
        }
    }
}

static void render_turi(const struct mu_track *t, const struct mu_key *key, struct mu_step *out) {
    const uint8_t *p = t->p[MU_TURI];
    uint8_t ivs[12];
    uint32_t n = mu_scale_notes(key->scale, ivs);
    uint32_t bits = mu_loop_len(MU_TURI, p[0], key), dens = p[1], rang = p[2], acnt = p[3];
    uint32_t rot = p[4], trns = p[5];
    uint32_t nd, j;
    if (rang < 1 || rang > 3)
        rang = 1;
    nd = n * rang;
    for (j = 0; j < MU_STEPS; j++) {
        uint32_t s = mu_loop_pos(MU_TURI, bits, rot, j);
        out[j].trig = (uint8_t)below(t->src.g[s], dens);
        out[j].note = pitch(key, ivs, n, ((uint32_t)t->src.p[s] * nd >> 8) + trns);
        out[j].vel = (uint8_t)(below(t->src.a[s], acnt / 2u) ? vel_accent(acnt) : vel_plain(acnt));
    }
}

/* ---- TB3P: the TB-3PO applet's generator --------------------------------------------------------
 * A C port of TB-3PO's pattern generation (Phazerville O_C, software/src/applets/TB3PO.h, commit
 * 684c952), with the Teensy core's random(), to give the applet's patterns for the same seed:
 *
 *   Copyright (c) 2020, Logarhythm (modified by djphazer)
 *   Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 *   and associated documentation files (the "Software"), to deal in the Software without
 *   restriction, including without limitation the rights to use, copy, modify, merge, publish,
 *   distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
 *   Software is furnished to do so, subject to the following conditions:
 *   The above copyright notice and this permission notice shall be included in all copies or
 *   substantial portions of the Software.
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
 *   BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 *   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 *   DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The applet's one DENSITY (0-14, shown -7..+7) feeds gate density |d - 7| and pitch density
 * min(d, 8); TB3P takes the two halves as two knobs. The Teensy 3 build generates in four passes,
 * each seeded seed + pass, so the halves are independent there and every applet DENSITY is a
 * GATE/PTCH pair. tests/test_engine.c checks the port against the applet's own code. */
#define ACID_MAX_STEPS 32

struct acid_rng {
    uint32_t seed;
};

static void acid_seed(struct acid_rng *r, uint32_t s) { /* randomSeed(): 0 keeps the old seed */
    if (s > 0)
        r->seed = s;
}

static int32_t
acid_random(struct acid_rng *r) { /* random(): avr-libc 1.6.4's, as the Teensy core */
    int32_t hi, lo, x = (int32_t)r->seed;
    if (x == 0)
        x = 123459876;
    hi = x / 127773;
    lo = x % 127773;
    x = 16807 * lo - 2836 * hi;
    if (x < 0)
        x += 0x7FFFFFFF;
    r->seed = (uint32_t)x;
    return x;
}

static uint32_t acid_below(struct acid_rng *r, uint32_t howbig) { /* random(howbig) */
    return howbig ? (uint32_t)acid_random(r) % howbig : 0;
}

static int acid_bit(struct acid_rng *r, int32_t prob) { return (int32_t)acid_below(r, 100) < prob; }

static int32_t acid_proportion(int32_t n, int32_t d, int32_t m) { /* Hemisphere's, 14-bit fixed */
    int32_t p = ((n < 0 ? -n : n) << 14) / d, s = (p * m) >> 14;
    return n >= 0 ? s : -s;
}

uint32_t mu_acid_range(uint32_t pcd, uint32_t size) {
    int32_t avail = 0;
    if (size > 0) {
        if (pcd > 7) {
            avail = (int32_t)size - 1;
        } else if (pcd < 2) {
            avail = (int32_t)pcd;
        } else {
            int32_t range = (int32_t)size - 3;
            if (range < 4)
                range = 4;
            avail = 3 + acid_proportion((int32_t)pcd - 3, 4, range);
            if (avail < 1)
                avail = 1;
            if (avail > (int32_t)size - 1)
                avail = (int32_t)size - 1;
        }
    }
    return (uint32_t)avail;
}

static void acid_pitches(struct acid_rng *r, struct mu_acid *a, uint32_t pcd, uint32_t size,
                         int first, int32_t from, int32_t to) {
    int32_t avail = (int32_t)mu_acid_range(pcd, size), s;
    if (first)
        a->oct_ups = a->oct_downs = 0;
    for (s = from; s < to; s++) {
        int32_t repeat = 50 - (int32_t)pcd * 6;
        if (s > 0 && acid_bit(r, repeat)) {
            a->notes[s] = a->notes[s - 1];
        } else {
            uint32_t coin;
            a->notes[s] = (uint8_t)acid_below(r, (uint32_t)avail + 1);
            a->oct_ups <<= 1;
            a->oct_downs <<= 1;
            coin = acid_below(r, 200);
            if (coin < 80) { /* 40 %: an octave up or down */
                if (coin & 1)
                    a->oct_ups |= 1;
                else
                    a->oct_downs |= 1;
            }
        }
    }
}

static void acid_density(struct acid_rng *r, struct mu_acid *a, uint32_t gd, int first, int32_t n) {
    int32_t prob = 10 + (int32_t)gd * 14, i, slide = 0, accent = 0, sr;
    if (first)
        a->gates = a->slides = a->accents = 0;
    for (i = 0; i < n; i++) {
        a->gates = a->gates << 1 | (uint32_t)acid_bit(r, prob);
        sr = (int32_t)acid_below(r, 100); /* rand_bit(): fewer consecutive slides */
        slide = sr < (slide ? 10 : 18);
        a->slide_r[(first ? 31 : 15) - i] = (uint8_t)sr; /* the bit this step ends on */
        a->slides = a->slides << 1 | (uint32_t)slide;
        accent = acid_bit(r, accent ? 7 : 16); /* fewer consecutive accents */
        a->accents = a->accents << 1 | (uint32_t)accent;
    }
}

void mu_acid_generate(uint16_t seed, uint32_t gate_dens, uint32_t pitch_dens, uint32_t scale_size,
                      int t4, struct mu_acid *a) {
    struct acid_rng r = {0};
    uint32_t i;
    for (i = 0; i < ACID_MAX_STEPS; i++)
        a->notes[i] = a->slide_r[i] = 0;
    if (t4) { /* one pass of 32 steps */
        acid_seed(&r, (uint32_t)seed + 1u);
        acid_pitches(&r, a, pitch_dens, scale_size, 1, 0, 32);
        acid_density(&r, a, gate_dens, 1, 32);
        return;
    }
    acid_seed(&r, (uint32_t)seed + 1u); /* two halves of 16, each pass seeded seed + pass */
    acid_pitches(&r, a, pitch_dens, scale_size, 1, 0, 16);
    acid_seed(&r, (uint32_t)seed + 2u);
    acid_density(&r, a, gate_dens, 1, 16);
    acid_seed(&r, (uint32_t)seed + 3u);
    acid_pitches(&r, a, pitch_dens, scale_size, 0, 16, 32);
    acid_seed(&r, (uint32_t)seed + 4u);
    acid_density(&r, a, gate_dens, 0, 16);
}

/* SLID is one curve, its slides only growing as it turns up: a step slides when its slide draw (the
 * applet's random(100)) is under a threshold. Up to 50 only the applet's own slides qualify, the
 * threshold rising to the applet's 18 at 50, so SLID 50 is the applet; above 50 any step does, the
 * threshold rising on to 60 at 100. */
int mu_acid_slides(const struct mu_acid *a, uint32_t s, uint32_t slid) {
    if (!slid)
        return 0;
    if (slid <= 50)
        return (a->slides >> s & 1u) && a->slide_r[s] < (slid * 18u + 49u) / 50u;
    return (a->slides >> s & 1u) || a->slide_r[s] < 18u + (slid - 50u) * 42u / 50u;
}

uint32_t mu_acid_ptim(uint32_t slid) { /* the glide's time: PTIM 8 at 1, 64 at 50, 96 at 100 */
    if (!slid)
        return 0;
    return slid <= 50 ? 8u + (slid - 1u) * 56u / 49u : 64u + (slid - 50u) * 32u / 50u;
}

uint16_t mu_acid_seed(const struct mu_track *t) { return (uint16_t)(t->acid[0] << 8 | t->acid[1]); }

void mu_acid_set_seed(struct mu_track *t, uint32_t seed) {
    t->acid[0] = (uint8_t)(seed >> 8);
    t->acid[1] = (uint8_t)seed;
}

/* A scale degree from ROOT, negative too, as the applet's quantizer reads 64 + degree. */
static uint8_t pitch_signed(const struct mu_key *key, const uint8_t *ivs, uint32_t n, int32_t deg) {
    int32_t root = key->root == 0xff ? key->note : key->root;
    int32_t base = key->note - (((int32_t)key->note - root + 1200) % 12);
    int32_t oct = deg >= 0 ? deg / (int32_t)n : -((-deg + (int32_t)n - 1) / (int32_t)n);
    int32_t note = base + ivs[deg - oct * (int32_t)n] + 12 * oct;
    return (uint8_t)(note < 0 ? 0 : note > 127 ? 127 : note);
}

/* TB3P on the Digitone: the applet's pattern over LEN steps, repeated across the track. A step
 * sounds when it is gated or the step before slides into it (the applet holds the gate); its pitch
 * is the step's note plus TRNS scale degrees, an octave of the scale up or down where flagged; an
 * accent plays at the accent velocity (ACNT, as the other models), a slid-into note too, as the
 * applet's gate output; a slide is a PORT ON lock on the note it glides into (MU_GLIDE_ON, with
 * PTIM from mu_acid_ptim) and every other note a PORT OFF lock, so a note jumps unless it slides
 * whatever the sound's own PORT is, as the applet's pitch output. The note a slide leaves holds
 * into the next one (hold), as the applet keeps its gate up: on a MONO LEG. sound the slid-into
 * note does not attack again. SLID 50 gives the applet's slides; see mu_acid_slides. */
#ifndef MU_ACID_T4
#define MU_ACID_T4 0 /* the Teensy 3 (original O_C) build's patterns */
#endif
static void render_tb3p(const struct mu_track *t, const struct mu_key *key, struct mu_step *out) {
    const uint8_t *p = t->p[MU_TB3P];
    uint8_t ivs[12];
    uint32_t n = mu_scale_notes(key->scale, ivs);
    uint32_t len = mu_loop_len(MU_TB3P, p[0], key), acnt = p[4], j;
    int32_t trns = (int32_t)p[5] - 7;
    struct mu_acid a;
    mu_acid_generate(mu_acid_seed(t), p[1], p[2], n, MU_ACID_T4, &a);
    for (j = 0; j < MU_STEPS; j++) {
        uint32_t s = j % len, prev = (s + len - 1) % len;
        int slid_in = mu_acid_slides(&a, prev, p[3]);
        int32_t deg = (int32_t)a.notes[s] + trns;
        if (a.oct_ups >> s & 1u)
            deg += (int32_t)n;
        else if (a.oct_downs >> s & 1u)
            deg -= (int32_t)n;
        out[j].trig = (uint8_t)((a.gates >> s & 1u) || slid_in);
        out[j].note = pitch_signed(key, ivs, n, deg);
        out[j].vel = a.accents >> s & 1u ? vel_accent(acnt) : vel_plain(acnt); /* as the applet */
        out[j].slide = (uint8_t)(slid_in ? MU_GLIDE_ON : MU_GLIDE_OFF);
        /* a 303's slide: the gate stays up into the next note, which does not attack again */
        out[j].hold = (uint8_t)(out[j].trig && mu_acid_slides(&a, s, p[3]));
    }
}

void mu_render(const struct mu_track *t, const struct mu_key *key, struct mu_step out[MU_STEPS]) {
    uint32_t i;
    for (i = 0; i < MU_STEPS; i++) {
        out[i].trig = 0;
        out[i].note = key->note;
        out[i].vel = t->model ? MU_VEL_FLAT : 0; /* MU_OFF: nothing plays, nothing says otherwise */
        out[i].slide = MU_GLIDE_NONE;
        out[i].hold = 0;
    }
    if (t->model == MU_EUCL)
        render_eucl(t, key, out);
    else if (t->model == MU_TURI)
        render_turi(t, key, out);
    else if (t->model == MU_TB3P)
        render_tb3p(t, key, out);
}

/* The picture's and the trig keys' view of the same loop. gates are the loop's first 16 positions
 * (what a cell grid shows); play is display order, the bits mu_render plays. Every length and every
 * position comes from mu_loop_len and mu_loop_pos, and the tests compare play with mu_render. */
void mu_preview(const struct mu_track *t, const struct mu_key *key, struct mu_preview *out) {
    uint8_t m = t->model;
    uint32_t len = mu_loop_len(m, t->p[m][0], key), rot = 0, i, cells;
    out->gates = 0;
    out->play = out->accent = out->slide = 0;
    out->len = (uint8_t)len;
    cells = len > 16u ? 16u : len;
    out->cells = (uint8_t)cells;
    out->start = 0;
    if (!mu_model_cap(m))
        return;
    if (m == MU_EUCL) {
        uint32_t hits = t->p[m][1], fill = t->p[m][5], acnt = t->p[m][4];
        rot = t->p[m][2];
        out->start = (uint8_t)mu_loop_pos(m, len, rot, 0);
        if (hits > len)
            hits = len;
        for (i = 0; i < cells; i++)
            if ((i * hits) % len < hits || below(t->src.g[i % MU_SRC], fill))
                out->gates |= (uint16_t)(1u << i);
        for (i = 0; i < MU_STEPS; i++) {
            uint32_t pos = mu_loop_pos(m, len, rot, i);
            int hit = (pos * hits) % len < hits;
            if (hit || below(t->src.g[pos % MU_SRC], fill))
                out->play |= (uint64_t)1 << i;
            if (hit && (i % len & 3u) == 0 && acnt)
                out->accent |= (uint64_t)1 << i;
        }
    } else if (m == MU_TURI) {
        uint32_t dens = t->p[m][1], acnt = t->p[m][3];
        rot = t->p[m][4];
        out->start = (uint8_t)mu_loop_pos(m, len, rot, 0);
        for (i = 0; i < cells; i++)
            if (below(t->src.g[i], dens))
                out->gates |= (uint16_t)(1u << i);
        for (i = 0; i < MU_STEPS; i++) {
            uint32_t pos = mu_loop_pos(m, len, rot, i);
            if (below(t->src.g[pos], dens))
                out->play |= (uint64_t)1 << i;
            if (acnt && below(t->src.a[pos], acnt / 2u))
                out->accent |= (uint64_t)1 << i;
        }
    } else {
        const uint8_t *p = t->p[m];
        uint8_t ivs[12];
        uint32_t n = mu_scale_notes(key->scale, ivs);
        struct mu_acid a;
        out->start = 0; /* TB3P has no rotation */
        mu_acid_generate(mu_acid_seed(t), p[1], p[2], n, MU_ACID_T4, &a);
        for (i = 0; i < cells; i++) {
            uint32_t prev = (i + len - 1u) % len;
            if ((a.gates >> i & 1u) || mu_acid_slides(&a, prev, p[3]))
                out->gates |= (uint16_t)(1u << i);
        }
        for (i = 0; i < MU_STEPS; i++) {
            uint32_t pos = mu_loop_pos(m, len, 0, i), prev = (pos + len - 1u) % len;
            if ((a.gates >> pos & 1u) || mu_acid_slides(&a, prev, p[3]))
                out->play |= (uint64_t)1 << i;
            if (a.accents >> pos & 1u)
                out->accent |= (uint64_t)1 << i;
            if (mu_acid_slides(&a, pos, p[3]))
                out->slide |= (uint64_t)1 << i;
        }
    }
}
