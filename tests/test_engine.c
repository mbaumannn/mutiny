/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host tests of the MUTINY engine: the rhythm, the key, the feel properties and the bounds. */
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

/* EUCL p: STEP HITS ROT DEGR ACNT FILL RIFF.  TURI p: LOOP DENS RANG ACNT ROT TRNS LOCK. */
enum { E_STEP, E_HITS, E_ROT, E_DEGR, E_ACNT, E_FILL, E_RIFF };
enum { T_LOOP, T_DENS, T_RANG, T_ACNT, T_ROT, T_TRNS, T_LOCK };

static void pattern(const struct mu_step *s, int n, char *out) {
    for (int i = 0; i < n; i++)
        out[i] = s[i].trig ? '1' : '0';
    out[n] = 0;
}

static int count(const struct mu_step *s, int n) {
    int c = 0;
    for (int i = 0; i < n; i++)
        c += s[i].trig;
    return c;
}

int main(void) {
    struct mu_track t;
    struct mu_key key = {60, 60, 0, 0};
    struct mu_step a[MU_STEPS], b[MU_STEPS];
    char text[MU_STEPS + 1];
    int hits;

    CHECK(sizeof(struct mu_track) == 80, "mu_track is %zu bytes", sizeof(struct mu_track));

    /* the knob metadata: a step of 1..8 detents and a fast factor, and MODEL moves plainly */
    for (int m = MU_OFF; m < MU_MODELS; m++)
        for (int k = 0; k <= MU_PARAMS; k++) {
            int32_t st = mu_knob_step((uint8_t)m, (uint8_t)k);
            int32_t fa = mu_knob_fast((uint8_t)m, (uint8_t)k);
            CHECK(st >= 1 && st <= 8, "model %d knob %d step %d", m, k, st);
            CHECK(fa == 1 || fa == 2 || fa == 3 || fa == 8, "model %d knob %d fast %d", m, k, fa);
            if (k == 0)
                CHECK(st == 3 && fa == 1, "MODEL is three detents a model");
        }
    CHECK(mu_knob_step(MU_EUCL, 2) == 2, "HITS is two detents a unit");
    CHECK(mu_knob_step(MU_TURI, 3) == 8, "RANG is eight detents a unit");
    CHECK(mu_knob_fast(MU_TB3P, 7) == 8, "SEED is x8");
    CHECK(strcmp(mu_knob_label(MU_EUCL, 7), "RIFF") == 0, "EUCL's H is %s",
          mu_knob_label(MU_EUCL, 7));

    /* Every scale row against the OS 1.44 table, read back at 0x401d42a0. Seven values a row, 0xff when the stock table leaves the row empty; the
     * stock table marks a sixth note's octave with a trailing 12. An empty row reads as
     * chromatic: MUTINY plays no conventional intervals under a stock scale's name. */
    {
        static const uint8_t fw[35][7] = {
            {0, 2, 4, 5, 7, 9, 11}, {0, 2, 3, 5, 7, 9, 10}, {0, 1, 3, 5, 7, 8, 10},
            {0, 2, 4, 6, 7, 9, 11}, {0, 2, 4, 5, 7, 9, 10}, {0, 2, 3, 5, 7, 8, 10},
            {0, 1, 3, 5, 6, 8, 10}, {255}, {255}, {0, 2, 3, 5, 7, 9, 11},
            {0, 2, 3, 5, 7, 8, 11}, {0, 2, 4, 6, 8, 10, 12}, {255}, {255},
            {255}, {255}, {255}, {255}, {255}, {0, 1, 4, 5, 7, 8, 10},
            {255}, {255}, {255}, {0, 2, 4, 5, 6, 8, 10}, {0, 1, 3, 4, 6, 8, 10},
            {0, 1, 3, 5, 7, 9, 10}, {0, 2, 4, 6, 8, 9, 11}, {0, 2, 4, 6, 7, 9, 10},
            {0, 1, 4, 5, 7, 8, 11}, {0, 3, 4, 6, 7, 10, 11}, {0, 1, 3, 4, 7, 8, 9},
            {0, 2, 3, 6, 7, 8, 11}, {0, 1, 4, 5, 6, 9, 10}, {0, 3, 4, 5, 8, 9, 11},
            {0, 1, 2, 5, 6, 8, 9},
        };
        int wrong = 0;
        for (int row = 0; row < 35; row++) {
            uint8_t ivs[12];
            uint32_t notes = mu_scale_notes((int8_t)row, ivs);
            if (fw[row][0] == 255) {
                CHECK(notes == 12, "row %d is not empty in the stock table but MUTINY plays %u",
                      row, notes);
                for (uint32_t i = 0; i < notes; i++)
                    CHECK(ivs[i] == i, "row %d fell back to a scale, not chromatic", row);
                continue;
            }
            uint32_t n = 0;
            while (n < 7 && fw[row][n] != 255)
                n++;
            if (n == 7 && fw[row][6] == 12)
                n = 6; /* the octave marker, not an interval */
            CHECK(notes == n, "row %d has %u notes, the stock table %u", row, notes, n);
            for (uint32_t i = 0; i < notes && i < n; i++)
                if (ivs[i] != fw[row][i]) {
                    wrong++;
                    CHECK(0, "row %d interval %u is %u, the stock table %u", row, i, ivs[i],
                          fw[row][i]);
                }
        }
        CHECK(wrong == 0, "%d scale rows differ from the OS 1.44 table", wrong);
    }

    /* OFF renders nothing */
    mu_defaults(&t);
    mu_render(&t, &key, a);
    CHECK(count(a, MU_STEPS) == 0, "OFF fired");

    /* the Euclidean rhythm */
    t.model = MU_EUCL;
    mu_render(&t, &key, a);
    pattern(a, 16, text);
    CHECK(strcmp(text, "1000100100100100") == 0, "E(5,16): %s", text);
    for (int i = 16; i < MU_STEPS; i++)
        CHECK(a[i].trig == a[i % 16].trig, "tiling at %d", i);
    t.p[MU_EUCL][E_STEP] = 13;
    mu_clamp(&t);
    mu_render(&t, &key, a);
    pattern(a, 13, text);
    CHECK(strcmp(text, "1001001010010") == 0, "E(5,13) = %s", text);
    t.p[MU_EUCL][E_STEP] = 16;
    t.p[MU_EUCL][E_ROT] = 3;
    mu_render(&t, &key, b);
    t.p[MU_EUCL][E_ROT] = 0;
    mu_render(&t, &key, a);
    int same = 1;
    for (int i = 0; i < 16; i++)
        same &= a[i].trig == b[(i + 3) % 16].trig;
    CHECK(same, "rotation is a cyclic shift");
    /* one knob never moves another: STEP down and back leaves HITS and ROT alone, and the render
     * still plays no more hits than the step count */
    t.p[MU_EUCL][E_HITS] = 12;
    t.p[MU_EUCL][E_ROT] = 9;
    t.p[MU_EUCL][E_STEP] = 4;
    mu_clamp(&t);
    CHECK(t.p[MU_EUCL][E_HITS] == 12 && t.p[MU_EUCL][E_ROT] == 9, "STEP moved %d/%d",
          t.p[MU_EUCL][E_HITS], t.p[MU_EUCL][E_ROT]);
    t.p[MU_EUCL][E_STEP] = 16;
    mu_clamp(&t);
    CHECK(t.p[MU_EUCL][E_HITS] == 12 && t.p[MU_EUCL][E_ROT] == 9, "the values did not stay");
    t.p[MU_EUCL][E_STEP] = 4;
    mu_render(&t, &key, a);
    CHECK(count(a, 4) == 4, "STEP 4 with HITS 12 played %d of 4", count(a, 4));

    /* pitch: G root with NOTE 60 puts degree 0 on G3 (55); ionian degree 2 is B3 (59) */
    mu_defaults(&t);
    t.model = MU_EUCL;
    struct mu_key g = {60, 67, 0, 0};
    mu_render(&t, &g, a);
    CHECK(a[0].note == 55, "G degree 0 -> %d", a[0].note);
    t.p[MU_EUCL][E_DEGR] = 2;
    mu_render(&t, &g, a);
    CHECK(a[0].note == 59, "G degree 2 -> %d", a[0].note);
    t.p[MU_EUCL][E_DEGR] = 7;
    mu_render(&t, &key, a);
    CHECK(a[0].note == 72, "C degree 7 -> %d", a[0].note);
    struct mu_key unknown = {60, 0xff, 0, 0};
    t.p[MU_EUCL][E_DEGR] = 0;
    mu_render(&t, &unknown, a);
    CHECK(a[0].note == 60, "unknown root uses NOTE: %d", a[0].note);

    /* Raising FILL only adds ghost hits, never moves a hit */
    mu_defaults(&t);
    t.model = MU_EUCL;
    mu_render(&t, &key, b);
    for (int f = 1; f <= 100; f++) {
        t.p[MU_EUCL][E_FILL] = (uint8_t)f;
        mu_render(&t, &key, a);
        for (int i = 0; i < MU_STEPS; i++)
            CHECK(!b[i].trig || a[i].trig, "FILL %d removed a trig at %d", f, i);
        memcpy(b, a, sizeof a);
    }
    CHECK(count(a, MU_STEPS) == MU_STEPS, "FILL 100 fills every step");

    /* Raising DENS only adds trigs, and an existing trig keeps its note and velocity */
    mu_defaults(&t);
    t.model = MU_TURI;
    t.p[MU_TURI][T_DENS] = 0;
    t.p[MU_TURI][T_ACNT] = 40;
    mu_render(&t, &key, b);
    CHECK(count(b, MU_STEPS) == 0, "DENS 0 fires");
    for (int d = 1; d <= 100; d++) {
        t.p[MU_TURI][T_DENS] = (uint8_t)d;
        mu_render(&t, &key, a);
        for (int i = 0; i < MU_STEPS; i++) {
            CHECK(!b[i].trig || a[i].trig, "DENS %d removed a trig at %d", d, i);
            CHECK(a[i].note == b[i].note && a[i].vel == b[i].vel, "DENS %d changed step %d", d, i);
        }
        memcpy(b, a, sizeof a);
    }
    CHECK(count(a, MU_STEPS) == MU_STEPS, "DENS 100 fires every step");

    /* ACNT is the accent's depth: one level at 0, 127 against 40 at 100, in both models */
    for (int m = MU_EUCL; m <= MU_TURI; m++) {
        int ak = m == MU_EUCL ? E_ACNT : T_ACNT, lo = 255, hi = 0;
        mu_defaults(&t);
        t.model = (uint8_t)m;
        if (m == MU_EUCL)
            t.p[m][E_FILL] = 0;
        else
            t.p[m][1] = 100; /* DENS */
        for (int v = 0; v <= 100; v += 100) {
            t.p[m][ak] = (uint8_t)v;
            mu_render(&t, &key, a);
            lo = 255, hi = 0;
            for (int i = 0; i < MU_STEPS; i++)
                if (a[i].trig) {
                    lo = a[i].vel < lo ? a[i].vel : lo;
                    hi = a[i].vel > hi ? a[i].vel : hi;
                }
            if (v == 0)
                CHECK(lo == 100 && hi == 100, "model %d ACNT 0: %d-%d", m, lo, hi);
            else
                CHECK(lo == 40 && hi == 127, "model %d ACNT 100: %d-%d", m, lo, hi);
        }
    }

    /* the notes are in the scale and within RANG octaves, and the loop repeats */
    uint8_t ivs[12];
    uint32_t n = mu_scale_notes(0, ivs);
    t.p[MU_TURI][T_RANG] = 2;
    mu_render(&t, &key, a);
    for (int i = 0; i < MU_STEPS; i++) {
        int pc = ((int)a[i].note - 60 + 1200) % 12, ok = 0;
        for (uint32_t k = 0; k < n; k++)
            ok |= pc == ivs[k];
        CHECK(ok, "note %d is out of the scale", a[i].note);
        CHECK(a[i].note >= 60 && a[i].note < 60 + 24, "note %d out of 2 octaves", a[i].note);
        CHECK(a[i].note == a[i % 8].note, "loop of 8 repeats at %d", i);
    }
    /* TRNS moves every note up by degrees; ROT is a cyclic shift of the loop */
    t.p[MU_TURI][T_RANG] = 1;
    mu_render(&t, &key, b);
    t.p[MU_TURI][T_TRNS] = 2;
    mu_render(&t, &key, a);
    CHECK(a[0].note - b[0].note == 3 || a[0].note - b[0].note == 4, "TRNS by 2 degrees moved %d",
          a[0].note - b[0].note);
    t.p[MU_TURI][T_TRNS] = 0;
    t.p[MU_TURI][T_ROT] = 3;
    mu_render(&t, &key, a);
    same = 1;
    for (int i = 0; i < 8; i++)
        same &= a[i].note == b[(i + 3) % 8].note;
    CHECK(same, "turing ROT is a cyclic shift");
    t.p[MU_TURI][T_LOOP] = 2; /* LOOP does not drag ROT with it either */
    mu_clamp(&t);
    CHECK(t.p[MU_TURI][T_ROT] == 3, "LOOP moved ROT: %d", t.p[MU_TURI][T_ROT]);
    t.p[MU_TURI][T_LOOP] = 8;

    /* RIFF: 0 is every hit on DEGR; up adds melodic hits one by one, each keeps its note, and the
     * melody stays in the scale and within an octave of DEGR; FILL does not touch it */
    {
        uint8_t iv2[12];
        uint32_t n2 = mu_scale_notes(0, iv2);
        mu_defaults(&t);
        t.model = MU_EUCL;
        t.p[MU_EUCL][E_ACNT] = 0;
        t.p[MU_EUCL][E_FILL] = 0;
        mu_render(&t, &key, b); /* RIFF 0 */
        for (int i = 0; i < MU_STEPS; i++)
            CHECK(b[i].note == 60, "RIFF 0 note at %d is %d", i, b[i].note);
        for (int r = 1; r <= 100; r++) {
            t.p[MU_EUCL][E_RIFF] = (uint8_t)r;
            mu_render(&t, &key, a);
            for (int i = 0; i < MU_STEPS; i++) {
                CHECK(!b[i].trig || a[i].trig, "RIFF %d removed a hit at %d", r, i);
                if (b[i].note != 60) /* a hit that had a note keeps it */
                    CHECK(a[i].note == b[i].note, "RIFF %d moved the note at %d", r, i);
                int pc = ((int)a[i].note - 60 + 1200) % 12, ok = 0;
                for (uint32_t k2 = 0; k2 < n2; k2++)
                    ok |= pc == iv2[k2];
                CHECK(ok, "RIFF %d note %d is out of the scale", r, a[i].note);
                CHECK(a[i].note >= 60 && a[i].note <= 72, "RIFF %d note %d is out of the octave", r,
                      a[i].note);
            }
            memcpy(b, a, sizeof a);
        }
        for (int sd = 0; sd < 40; sd++) { /* more sources: the count never falls */
            int prev = -1;
            mu_defaults(&t);
            t.model = MU_EUCL;
            t.p[MU_EUCL][E_FILL] = 0;
            for (int k2 = 0; k2 < sd; k2++)
                mu_reroll(&t);
            for (int r = 0; r <= 100; r += 5) {
                int mel = 0;
                t.p[MU_EUCL][E_RIFF] = (uint8_t)r;
                mu_render(&t, &key, a);
                for (int i = 0; i < MU_STEPS; i++)
                    mel += a[i].trig && a[i].note != 60;
                CHECK(mel >= prev, "source %d RIFF %d: %d melodic hits, was %d", sd, r, mel, prev);
                prev = mel;
            }
        }
        /* FILL does not change which hits are melodic, RIFF does not change which steps fire */
        mu_defaults(&t);
        t.model = MU_EUCL;
        t.p[MU_EUCL][E_RIFF] = 60;
        mu_render(&t, &key, b);
        t.p[MU_EUCL][E_FILL] = 100;
        mu_render(&t, &key, a);
        for (int i = 0; i < MU_STEPS; i++)
            CHECK(a[i].note == b[i].note, "FILL moved the melody at %d", i);
    }

    /* TRK: the length control's top position + 1 follows the track's length, as far as the model
     * can hold it (EUCL 64, TURI its 16-step source, TB3P the applet's 32), else it falls back */
    {
        struct mu_key k12 = {60, 60, 0, 12}, k64 = {60, 60, 0, 64};
        struct mu_key k0 = {60, 60, 0, 0};
        int lit;
        mu_defaults(&t);
        t.model = MU_EUCL;
        t.p[MU_EUCL][E_STEP] = 65; /* TRK */
        mu_render(&t, &k12, a);
        for (lit = 0; lit < MU_STEPS; lit++)
            CHECK(a[lit].trig == a[lit % 12].trig, "EUCL TRK: step %d does not follow 12", lit);
        mu_render(&t, &k64, a);
        CHECK(count(a, 64) == count(a, 64), "EUCL TRK 64 renders");
        mu_render(&t, &k0, a); /* unknown length: the model's own 16 */
        for (lit = 0; lit < MU_STEPS; lit++)
            CHECK(a[lit].trig == a[lit % 16].trig, "EUCL TRK unknown: step %d", lit);
        t.p[MU_TURI][T_LOOP] = 17; /* TRK, capped at the 16-step source */
        t.model = MU_TURI;
        t.p[MU_TURI][T_DENS] = 100;
        mu_render(&t, &k64, b);
        for (lit = 0; lit < MU_STEPS; lit++)
            CHECK(b[lit].note == b[lit % 16].note, "TURI TRK: step %d does not cap at 16", lit);
        mu_render(&t, &k12, b);
        for (lit = 0; lit < MU_STEPS; lit++)
            CHECK(b[lit].note == b[lit % 12].note, "TURI TRK: step %d does not follow 12", lit);
        t.model = MU_TB3P;
        t.p[MU_TB3P][0] = 33; /* TRK, capped at 32 */
        mu_render(&t, &k64, a);
        for (lit = 0; lit < MU_STEPS; lit++)
            CHECK(a[lit].trig == a[lit % 32].trig, "TB3P TRK: step %d does not cap at 32", lit);
        mu_render(&t, &k12, a);
        for (lit = 0; lit < MU_STEPS; lit++)
            CHECK(a[lit].trig == a[lit % 12].trig, "TB3P TRK: step %d does not follow 12", lit);
        CHECK(mu_knob_max(MU_EUCL, 1) == 65 && mu_knob_max(MU_TURI, 1) == 17 &&
                  mu_knob_max(MU_TB3P, 1) == 33,
              "the length knobs' TRK positions");
    }

    /* RIFF's line: the note of a step comes from a walk, so it moves at most two scale degrees a
     * step, stays inside the scale, and does not depend on RIFF's own value */
    {
        uint8_t iv2[12], d100[MU_SRC], d0[MU_SRC];
        uint32_t n2 = mu_scale_notes(0, iv2);
        mu_defaults(&t);
        t.model = MU_EUCL;
        t.p[MU_EUCL][E_RIFF] = 100;
        mu_riff_degrees(&t, n2, d100);
        t.p[MU_EUCL][E_RIFF] = 0;
        mu_riff_degrees(&t, n2, d0);
        CHECK(memcmp(d100, d0, sizeof d100) == 0, "the walk follows RIFF");
        for (int i = 0; i < MU_SRC; i++) {
            CHECK(d100[i] < n2, "walk degree %d of %u", d100[i], n2);
            if (i) {
                int d = (int)d100[i] - (int)d100[i - 1];
                if (d < 0)
                    d = -d;
                if (d > (int)n2 / 2)
                    d = (int)n2 - d; /* wrapping round the octave is a small step too */
                CHECK(d <= 2, "walk step %d jumps %d degrees", i, d);
            }
        }
    }

    /* only TURI drifts: EUCL's H is RIFF and TB3P's is its seed */
    mu_defaults(&t);
    t.model = MU_EUCL;
    CHECK(MU_LOCK(&t) == 100, "EUCL has a lock");
    {
        struct mu_track e0 = t;
        mu_evolve(&e0, 5);
        CHECK(memcmp(&e0.src, &t.src, sizeof t.src) == 0, "EUCL evolved");
    }

    /* the accent is derived from the velocity, so no model grows its table */
    for (int m = MU_EUCL; m <= MU_TB3P; m++) {
        int ak = m == MU_TURI ? 3 : 4, acc = 0;
        mu_defaults(&t);
        t.model = (uint8_t)m;
        t.p[m][ak] = 100;
        if (m == MU_EUCL || m == MU_TURI)
            t.p[m][1] = m == MU_EUCL ? t.p[m][1] : 100; /* EUCL HITS 5; TURI DENS 100 */
        if (m == MU_TB3P) {
            t.p[m][1] = 7;
            t.p[m][2] = 8;
        }
        mu_render(&t, &key, a);
        for (int i = 0; i < MU_STEPS; i++) {
            CHECK(mu_step_accent(&a[i]) == (a[i].trig && a[i].vel > MU_VEL_FLAT), "accent %d", i);
            acc += mu_step_accent(&a[i]);
        }
        CHECK(acc > 0, "model %d has no accent at ACNT 100", m);
        t.p[m][ak] = 0;
        mu_render(&t, &key, a);
        for (int i = 0; i < MU_STEPS; i++)
            CHECK(!mu_step_accent(&a[i]), "model %d has an accent at ACNT 0", m);
    }

    /* LOCK 100 repeats exactly, however many passes */
    mu_defaults(&t);
    t.model = MU_TURI;
    t.p[MU_TURI][T_LOCK] = 100;
    mu_render(&t, &key, b);
    for (uint32_t pass = 1; pass <= 200; pass++)
        mu_evolve(&t, pass);
    mu_render(&t, &key, a);
    CHECK(memcmp(a, b, sizeof a) == 0, "LOCK 100 drifted");

    /* Lower LOCK drifts, a little at 90 and much more at 10, and it is deterministic */
    struct mu_track lo, hi, again;
    mu_defaults(&lo);
    lo.model = MU_TURI;
    hi = lo;
    lo.p[MU_TURI][T_LOCK] = 10;
    hi.p[MU_TURI][T_LOCK] = 90;
    again = lo;
    int changed_lo = 0, changed_hi = 0;
    for (uint32_t pass = 1; pass <= 20; pass++) {
        struct mu_track l0 = lo, h0 = hi;
        mu_evolve(&lo, pass);
        mu_evolve(&hi, pass);
        mu_evolve(&again, pass);
        for (int i = 0; i < MU_SRC; i++) {
            changed_lo += lo.src.g[i] != l0.src.g[i];
            changed_hi += hi.src.g[i] != h0.src.g[i];
        }
    }
    CHECK(memcmp(&lo, &again, sizeof lo) == 0, "evolve is not deterministic");
    CHECK(changed_lo > 3 * changed_hi && changed_hi > 0, "drift lo %d hi %d", changed_lo,
          changed_hi);

    /* REROLL draws a new source, a new seed gives a new loop, FILL on a fresh track is stable */
    mu_defaults(&t);
    t.model = MU_TURI;
    mu_render(&t, &key, a);
    mu_reroll(&t);
    mu_render(&t, &key, b);
    CHECK(memcmp(a, b, sizeof a) != 0, "REROLL changed nothing");
    struct mu_track u;
    mu_defaults(&u);
    u.model = MU_TURI;
    mu_render(&u, &key, a);
    mu_defaults(&t);
    t.model = MU_TURI;
    mu_render(&t, &key, b);
    CHECK(memcmp(a, b, sizeof a) == 0, "defaults are not deterministic");

    /* every scale yields 1..12 sorted in-octave notes; unknown scales are chromatic */
    for (int s = -1; s < 40; s++) {
        n = mu_scale_notes((int8_t)s, ivs);
        CHECK(n >= 1 && n <= 12, "scale %d has %u notes", s, n);
        for (uint32_t k = 1; k < n; k++)
            CHECK(ivs[k] > ivs[k - 1] && ivs[k] < 12, "scale %d unsorted", s);
    }
    CHECK(mu_scale_notes(5, ivs) == 7 && ivs[2] == 3, "AEOLIAN is minor");

    /* One effective length and one preview coordinate.
     * Every model's TRK position, at every track length, resolves to a real array length (never
     * a sentinel) and the picture's play bits are the render's own. A TURI loop can never reach
     * src[16]; an EUCL loop can be 64 and a TB3P loop 32, and neither picture shows more than 16
     * cells. */
    {
        static const uint8_t lens[] = {0, 1, 2, 8, 12, 16, 17, 31, 32, 48, 63, 64};
        struct mu_preview pv;
        for (int m = MU_EUCL; m <= MU_TB3P; m++) {
            uint32_t cap = mu_model_cap((uint8_t)m);
            CHECK(cap == (m == MU_EUCL   ? 64u
                          : m == MU_TURI ? 16u
                                         : 32u),
                  "model %d cap %u", m, cap);
            for (unsigned li = 0; li < sizeof lens; li++) {
                struct mu_key kk = {60, 60, 0, lens[li]};
                mu_defaults(&t);
                t.model = (uint8_t)m;
                t.p[m][0] = (uint8_t)(cap + 1);                        /* TRK */
                t.p[m][m == MU_TB3P ? 1 : 1] = m == MU_TB3P ? 7 : 100; /* gates on */
                mu_clamp(&t);
                uint32_t len = mu_loop_len((uint8_t)m, t.p[m][0], &kk);
                CHECK(len >= (m == MU_TURI ? 2u : 1u) && len <= cap,
                      "model %d TRK at track length %u gave loop %u", m, lens[li], len);
                mu_preview(&t, &kk, &pv);
                CHECK(pv.len == len && pv.cells <= 16 && pv.cells <= len, "preview %d/%u", pv.len,
                      len);
                mu_render(&t, &kk, a);
                for (int s = 0; s < MU_STEPS; s++) {
                    CHECK(((pv.play >> s & 1u) != 0) == (a[s].trig != 0),
                          "model %d len %u step %d: preview %d, render %d", m, len, s,
                          (int)(pv.play >> s & 1u), a[s].trig);
                    CHECK(((pv.accent >> s & 1u) != 0) == (mu_step_accent(&a[s]) != 0),
                          "model %d len %u step %d: accent", m, len, s);
                }
            }
            /* a hand-set length and the unknown-track fallback */
            struct mu_key unknown = {60, 60, 0, 0};
            CHECK(mu_loop_len((uint8_t)m, cap + 1, &unknown) == mu_model_default((uint8_t)m),
                  "model %d unknown TRK", m);
            CHECK(mu_loop_len((uint8_t)m, 1, &unknown) == (m == MU_TURI ? 2u : 1u),
                  "model %d shortest", m);
            CHECK(mu_loop_len((uint8_t)m, cap, &unknown) == cap, "model %d longest", m);
            CHECK(mu_loop_len((uint8_t)m, cap + 2, &unknown) ==
                      (mu_model_default((uint8_t)m) > cap ? cap : mu_model_default((uint8_t)m)),
                  "model %d past TRK follows the default", m);
        }
    }

    /* The rhythm picture is a literal fixture, not another copy of the formula: TURI LOOP 8,
     * ROT 1, DENS 50 over a source whose gates are 0 at the odd positions. */
    {
        struct mu_key k8 = {60, 60, 0, 0};
        struct mu_preview pv;
        mu_defaults(&t);
        t.model = MU_TURI;
        for (int i = 0; i < MU_SRC; i++)
            t.src.g[i] = (uint8_t)(i & 1 ? 0 : 255);
        t.p[MU_TURI][T_LOOP] = 8;
        t.p[MU_TURI][T_DENS] = 50;
        t.p[MU_TURI][T_ROT] = 1;
        mu_clamp(&t);
        mu_preview(&t, &k8, &pv);
        CHECK(pv.len == 8 && pv.cells == 8 && pv.gates == 0x00aa, "TURI gates %04x", pv.gates);
        CHECK(pv.start == 1, "TURI start %u", pv.start);
        mu_render(&t, &k8, a);
        pattern(a, 16, text);
        CHECK(strcmp(text, "1010101010101010") == 0, "TURI LOOP 8 ROT 1: %s", text);
        /* ROT 1 moves the loop one step later: position 1 (a gate) plays on step 0 */
        CHECK(pv.play & 1u, "TURI ROT 1 should start on a gate");
    }

    /* EUCL TRK 12 is E(5,12), written out by hand: positions 0, 3, 5, 8, 10; ROT 0 plays them
     * there, ROT 1 one step later. */
    {
        struct mu_key k12 = {60, 60, 0, 12};
        struct mu_preview pv;
        mu_defaults(&t);
        t.model = MU_EUCL;
        t.p[MU_EUCL][E_STEP] = 65;
        t.p[MU_EUCL][E_HITS] = 5;
        t.p[MU_EUCL][E_ROT] = 0;
        mu_clamp(&t);
        mu_preview(&t, &k12, &pv);
        CHECK(pv.len == 12 && pv.gates == 0x0529, "EUCL TRK 12 gates %04x", pv.gates);
        mu_render(&t, &k12, a);
        pattern(a, 12, text);
        CHECK(strcmp(text, "100101001010") == 0, "E(5,12): %s", text);
        t.p[MU_EUCL][E_ROT] = 1;
        mu_preview(&t, &k12, &pv);
        CHECK(pv.start == 11, "EUCL ROT 1 starts at position %u", pv.start);
        mu_render(&t, &k12, a);
        pattern(a, 12, text);
        CHECK(strcmp(text, "010010100101") == 0, "E(5,12) ROT 1: %s", text);
    }

    /* the whole parameter space stays in bounds and never crashes, evolve and reroll included */
    unsigned seed = 12345;
    for (int m = 1; m < MU_MODELS; m++) {
        for (int round = 0; round < 3000; round++) {
            mu_defaults(&t);
            t.model = (uint8_t)m;
            for (int i = 0; i < MU_PARAMS; i++) {
                seed = seed * 1103515245u + 12345u;
                t.p[m][i] = (uint8_t)(seed >> 16);
            }
            mu_clamp(&t);
            if (round & 1)
                mu_reroll(&t);
            mu_evolve(&t, (uint32_t)round);
            struct mu_key kk = {(uint8_t)(seed >> 8),
                                (uint8_t)((seed >> 4) & 1 ? 0xff : (seed & 127)),
                                (int8_t)((int)(seed >> 3) % 40 - 2), (uint8_t)(seed >> 1)};
            mu_render(&t, &kk, a);
            for (int i = 0; i < MU_STEPS; i++)
                if (a[i].trig)
                    CHECK(a[i].vel >= 1 && a[i].vel <= 127 && a[i].note <= 127, "range");
        }
    }
    /* TB3P: the TB-3PO applet's patterns. Golden vectors from the applet's own code (TB3PO.h at
     * Phazerville 684c952 with the Teensy core's random(), built by tools/tb3po_ref): scale size,
     * applet DENSITY 0-14, seed, then gates, slides, accents, octave ups, downs and the 32 notes.
     */
    {
        static const struct {
            uint32_t size, dens, seed, t4, m[5];
            uint8_t notes[32];
        } gold[] = {
            {5,
             7,
             65475,
             0,
             {0x40080100, 0x940a8088, 0x20002400, 0x29640104, 0x120a1252},
             {2, 3, 4, 1, 1, 4, 2, 0, 4, 3, 4, 3, 0, 1, 1, 2,
              3, 2, 4, 0, 0, 1, 4, 2, 0, 2, 4, 1, 1, 1, 2, 3}},
            {7,
             3,
             9700,
             0,
             {0xf039beb3, 0x2088000c, 0x00002a02, 0x00080030, 0x00a13649},
             {3, 3, 0, 0, 1, 1, 3, 3, 0, 1, 0, 3, 0, 0, 3, 3,
              3, 2, 0, 0, 2, 3, 3, 3, 0, 1, 1, 1, 0, 0, 1, 3}},
            {12, 0, 970, 0, {0xffffffff, 0x0491a402, 0x00400020, 0x00000004, 0x00000e40}, {0}},
            {12,
             12,
             0,
             0,
             {0xdcfeae7e, 0x64211442, 0x02080020, 0x20000800, 0x0e9c5482},
             {7, 2,  0, 1, 0, 11, 4, 1, 4, 6, 1, 7, 7, 9, 1, 5,
              3, 11, 4, 5, 6, 7,  0, 8, 8, 5, 4, 4, 3, 7, 7, 1}},
            {12,
             12,
             44038,
             0,
             {0x6ff73fef, 0x14500480, 0x4100c089, 0x02000408, 0x009a0804},
             {5,  8, 11, 9, 6, 8, 1, 7, 4,  3, 8, 3, 9, 0, 4, 4,
              10, 1, 3,  1, 4, 5, 7, 8, 11, 5, 5, 6, 8, 1, 8, 10}},
            {12,
             12,
             44038,
             1,
             {0xf7f37fae, 0xa0800804, 0x28400010, 0x04004000, 0x01348141},
             {5, 8, 11, 9, 6, 8, 1, 7,  4, 3, 8,  3, 9, 0, 4, 4,
              3, 5, 9,  7, 2, 1, 7, 10, 7, 3, 11, 2, 3, 2, 9, 6}},
        };
        for (unsigned g = 0; g < sizeof gold / sizeof gold[0]; g++) {
            struct mu_acid ac;
            uint32_t d = gold[g].dens, gd = d < 7 ? 7 - d : d - 7, pd = d > 8 ? 8 : d;
            mu_acid_generate((uint16_t)gold[g].seed, gd, pd, gold[g].size, (int)gold[g].t4, &ac);
            CHECK(ac.gates == gold[g].m[0] && ac.slides == gold[g].m[1] &&
                      ac.accents == gold[g].m[2] && ac.oct_ups == gold[g].m[3] &&
                      ac.oct_downs == gold[g].m[4],
                  "TB3P golden %u: masks", g);
            for (int i = 0; i < 32; i++)
                CHECK(ac.notes[i] == gold[g].notes[i], "TB3P golden %u: note %d", g, i);
        }
        /* on the Digitone: C minor from C4, LEN 16, the default seed */
        struct mu_key k3 = {60, 60, 5, 0};
        struct mu_acid ac;
        uint8_t ivs[12];
        uint32_t n = mu_scale_notes(5, ivs);
        mu_defaults(&t);
        t.model = MU_TB3P;
        t.p[MU_TB3P][1] = 7; /* GATE 7, PTCH 8: DENSITY +7 */
        t.p[MU_TB3P][2] = 8;
        mu_render(&t, &k3, a);
        mu_acid_generate(mu_acid_seed(&t), 7, 8, n, 0, &ac);
        for (int i = 0; i < MU_STEPS; i++) {
            int s = i % 16, pv = (s + 15) % 16, slid = ac.slides >> pv & 1u;
            int deg = ac.notes[s] + (ac.oct_ups >> s & 1u     ? (int)n
                                     : ac.oct_downs >> s & 1u ? -(int)n
                                                              : 0);
            int oct = deg < 0 ? -1 : deg / (int)n, note = 60 + ivs[deg - oct * (int)n] + 12 * oct;
            CHECK(a[i].trig == ((ac.gates >> s & 1u) || slid), "TB3P trig %d", i);
            CHECK(a[i].note == note, "TB3P note %d: %d, not %d", i, a[i].note, note);
            CHECK(a[i].slide == (slid ? MU_GLIDE_ON : MU_GLIDE_OFF), "TB3P slide %d", i);
            CHECK(a[i].vel == (ac.accents >> s & 1u ? 116 : 64), "TB3P velocity %d: %d", i,
                  a[i].vel);
            /* the note a slide leaves holds into the next; a held note's next step always sounds */
            CHECK(a[i].hold == (a[i].trig && (ac.slides >> s & 1u)), "TB3P hold %d", i);
            CHECK(!a[i].hold || a[(i + 1) % MU_STEPS].trig, "TB3P hold %d into a rest", i);
        }
        /* SLID 50 is the applet's slides, and turning it up only adds slides: 1-50 a growing part
         * of the applet's, 51-100 others too, up to about 60 % at 100 */
        for (int v = 0, prev = -1; v <= 100; v++) {
            int n_sl = 0;
            for (int s = 0; s < 32; s++) {
                int on = mu_acid_slides(&ac, (uint32_t)s, (uint32_t)v);
                if (v == 50)
                    CHECK(on == (int)(ac.slides >> s & 1u), "SLID 50 %d", s);
                if (v <= 50 && on)
                    CHECK(ac.slides >> s & 1u, "SLID %d slid %d, not an applet slide", v, s);
                if (on)
                    CHECK(v == 100 || mu_acid_slides(&ac, (uint32_t)s, (uint32_t)v + 1),
                          "SLID %d took %d away", v, s);
                n_sl += on;
            }
            CHECK(n_sl >= prev, "SLID %d: fewer slides", v);
            CHECK(v == 0 || mu_acid_ptim((uint32_t)v) > mu_acid_ptim((uint32_t)v - 1) ||
                      (v > 50 && mu_acid_ptim((uint32_t)v) == mu_acid_ptim((uint32_t)v - 1)),
                  "PTIM %d", v);
            prev = n_sl;
        }
        CHECK(mu_acid_ptim(0) == 0 && mu_acid_ptim(1) == 8 && mu_acid_ptim(50) == 64 &&
                  mu_acid_ptim(100) == 96,
              "PTIM ends");
        { /* the applet's slides come in gradually below 50: about half of them at 25 */
            int all = 0, half = 0;
            for (uint32_t sd = 0; sd < 400; sd++) {
                mu_acid_generate((uint16_t)(sd * 163u), 7, 8, n, 0, &ac);
                for (int s = 0; s < 32; s++) {
                    all += mu_acid_slides(&ac, (uint32_t)s, 50);
                    half += mu_acid_slides(&ac, (uint32_t)s, 25);
                }
            }
            CHECK(half * 10 > all * 3 && half * 10 < all * 7, "SLID 25: %d of %d slides", half,
                  all);
            mu_acid_generate(mu_acid_seed(&t), 7, 8, n, 0, &ac);
        }
        t.p[MU_TB3P][3] = 0; /* SLID 0: no glides, only gated steps sound */
        mu_render(&t, &k3, a);
        for (int i = 0; i < MU_STEPS; i++)
            CHECK(a[i].slide == MU_GLIDE_OFF && !a[i].hold &&
                      a[i].trig == (ac.gates >> (i % 16) & 1u),
                  "TB3P SLID OFF step %d", i);
        t.p[MU_TB3P][2] = 0; /* PTCH 0: the root alone, an octave away where flagged */
        t.p[MU_TB3P][5] = 9; /* TRNS +2: two degrees up, D... Eb from C */
        mu_render(&t, &k3, a);
        for (int i = 0; i < MU_STEPS; i++)
            CHECK(a[i].note == 63 || a[i].note == 51 || a[i].note == 75, "TB3P PTCH 0 played %d",
                  a[i].note);
        t.p[MU_TB3P][1] = 0; /* GATE 0: 10 % */
        t.p[MU_TB3P][0] = 32;
        mu_render(&t, &k3, a);
        CHECK(count(a, 32) > 0 && count(a, 32) < 12, "TB3P GATE 0: %d of 32", count(a, 32));
        uint16_t s0 = mu_acid_seed(&t);
        mu_reroll(&t);
        CHECK(mu_acid_seed(&t) != s0, "TB3P reroll kept the seed");
        mu_evolve(&t, 1);
        CHECK(MU_LOCK(&t) == 100, "TB3P evolves");
        mu_defaults(&t);
        t.model = MU_EUCL;
        mu_render(&t, &k3, a);
        for (int i = 0; i < MU_STEPS; i++)
            CHECK(a[i].slide == MU_GLIDE_NONE && !a[i].hold, "EUCL set a glide");
    }
    (void)hits;
#ifdef MU_ENGINE_PROBE
    /* One period is mapped, the rest is a bounded copy. Defaults: TURI 8, EUCL 16, TB3P 16. */
    {
        extern uint32_t mu_engine_mapped_steps;
        struct mu_track p;
        struct mu_key k = {60, 60, 5, 0};
        struct {
            uint8_t model, length_control;
            uint32_t want;
        } cases[6] = {
            {MU_TURI, 8, 8},   /* default LOOP */
            {MU_EUCL, 16, 16}, /* default STEP */
            {MU_TB3P, 16, 16}, /* default LEN */
            {MU_EUCL, 64, 64}, {MU_TURI, 2, 2}, {MU_TB3P, 32, 32},
        };
        uint32_t c;
        for (c = 0; c < 6; c++) {
            mu_defaults(&p);
            p.model = cases[c].model;
            p.p[p.model][0] = cases[c].length_control;
            mu_engine_mapped_steps = 0;
            mu_render(&p, &k, a);
            CHECK(mu_engine_mapped_steps == cases[c].want, "model %u mapped %u steps, want %u",
                  p.model, mu_engine_mapped_steps, cases[c].want);
            for (int i = 0; i < MU_STEPS; i++) {
                const struct mu_step *s = &a[(uint32_t)i % cases[c].want];
                CHECK(a[i].trig == s->trig && a[i].note == s->note && a[i].vel == s->vel &&
                          a[i].slide == s->slide && a[i].hold == s->hold,
                      "model %u step %d does not repeat its period", p.model, i);
            }
        }
        /* TRK follows the track length (TURI caps at its 16-step source) */
        mu_defaults(&p);
        p.model = MU_TURI;
        p.p[MU_TURI][0] = 17;
        k.len = 12;
        mu_engine_mapped_steps = 0;
        mu_render(&p, &k, a);
        CHECK(mu_engine_mapped_steps == 12, "TURI TRK mapped %u steps, want 12",
              mu_engine_mapped_steps);
        for (int i = 0; i < MU_STEPS; i++)
            CHECK(a[i].note == a[i % 12].note, "TURI TRK step %d does not repeat 12", i);
    }
#endif
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("engine tests passed\n");
    return 0;
}
