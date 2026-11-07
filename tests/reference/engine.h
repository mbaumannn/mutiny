/* SPDX-License-Identifier: GPL-2.0-or-later */
/* MUTINY engine: pure, allocation-free generators for the Digitone mk1 mod.
 *
 * No firmware address, no libc, no global state: the same file builds for the host
 * (tests) and for the m68k mod. A model turns its settings and the track's key into a
 * table of 64 steps; the mod's step-fire handlers only read that table.
 * EUCL and TURI are original implementations from the described behaviour of the Euclidean rhythm
 * and the Turing Machine shift register. TB3P's generator is a C port of the TB-3PO applet's
 * (MIT, (c) 2020 Logarhythm, modified by djphazer; see engine.c). */
#ifndef MUTINY_ENGINE_H
#define MUTINY_ENGINE_H

#include <stdint.h>

#define MU_STEPS 64
#define MU_KNOBS 8  /* A is MODEL; B..H are the model's seven controls */
#define MU_PARAMS 7 /* controls B..H; H is LOCK on every model */
#define MU_SRC 16   /* steps of source memory: the project's data gap is only 480 bytes */

enum mu_model { MU_OFF = 0, MU_EUCL = 1, MU_TURI = 2, MU_TB3P = 3, MU_MODELS = 4 };

/* The random numbers a track's loop is made of. The knobs are transforms applied to them live
 * (a threshold on g gives the density, p picks the pitch, a the accents), so a knob turn moves
 * the music continuously and nothing re-rolls except on request. */
struct mu_source {
    uint8_t g[MU_SRC]; /* gate */
    uint8_t p[MU_SRC]; /* pitch */
    uint8_t a[MU_SRC]; /* accent */
};

/* One track's state: the model, a set of seven values per model (switching the model and back
 * restores the controls), the seed counter, the source memory and TB3P's 16-bit seed (high byte
 * first). 80 bytes, all uint8_t. */
struct mu_track {
    uint8_t model;
    uint8_t p[MU_MODELS][MU_PARAMS];
    uint8_t seed;
    struct mu_source src;
    uint8_t acid[2];
};

/* TB3P's pattern, as the TB-3PO applet generates it: 32 steps, bit s of each mask is step s. */
struct mu_acid {
    uint32_t gates, slides, accents, oct_ups, oct_downs;
    uint8_t notes[32]; /* scale degrees from ROOT */
    uint8_t
        slide_r[32]; /* the random(100) each step's slide was drawn from (bit order as the masks) */
};
/* gate_dens 0-7 and pitch_dens 0-8 are the applet's DENSITY halves (DENSITY d gives |d-7| and
 * min(d, 8)); scale_size is the scale's note count; t4: the Teensy 4 build's single pass. */
void mu_acid_generate(uint16_t seed, uint32_t gate_dens, uint32_t pitch_dens, uint32_t scale_size,
                      int t4, struct mu_acid *a);
/* The scale degrees TB3P uses at pitch density pd for a scale of size notes: 0 to the result. */
uint32_t mu_acid_range(uint32_t pd, uint32_t size);
/* TB3P's SLID 0-100: does step s slide into the next (0 none; 1-50 the applet's slides; above 50
 * also steps whose draw is under a rising threshold, so turning up only adds slides), and the PTIM
 * the slides' notes lock (0: none), longer as SLID rises to 50. */
int mu_acid_slides(const struct mu_acid *a, uint32_t s, uint32_t slid);
uint32_t mu_acid_ptim(uint32_t slid);
uint16_t mu_acid_seed(const struct mu_track *t);
void mu_acid_set_seed(struct mu_track *t, uint32_t seed);

/* The track's key, as seq-events reports it (sq_track_info). scale: -1 chromatic, from 0 the
 * Digitone's scales in the order of the manual (APPENDIX D) after CHROMATIC. */
struct mu_key {
    uint8_t note; /* the track's NOTE: the register the notes are placed around */
    uint8_t root; /* keyboard ROOT, a MIDI note; 0xff when unknown (NOTE is the root) */
    int8_t scale;
    uint8_t len; /* the track's length in steps (1-64); 0: unknown, a model falls back to its own */
};

/* slide: the step's portamento (the TRIG page's PORT) as a lock: MU_GLIDE_NONE leaves the
 * track's own, MU_GLIDE_ON glides into the note from the one before, MU_GLIDE_OFF does not. */
enum { MU_GLIDE_NONE = 0, MU_GLIDE_ON = 1, MU_GLIDE_OFF = 2 };
#define MU_VEL_FLAT                                                                                \
    100u /* ACNT 0: every hit plays at this velocity, the Digitone's default VEL                   \
          */
struct mu_step {
    uint8_t trig;  /* 1: the step fires */
    uint8_t note;  /* MIDI note, 0-127 */
    uint8_t vel;   /* 1-127 */
    uint8_t slide; /* MU_GLIDE_* */
    uint8_t hold;  /* 1: the note holds into the next step's, as a 303's gate before a slide */
};

/* An accented step: above the flat velocity. The models do not store the accent bit, so this is
 * how the mod finds the steps that take the accent's locks. 0 at ACNT 0 (every hit is flat). */
int mu_step_accent(const struct mu_step *s);

/* ---- one length and one preview coordinate ----------------------------------------------------
 */
/* Every consumer (render, cycle scheduling, ruler, hit masks, rotation, LEDs, drawing) asks these
 * rather than re-deriving a loop: TRK is a control position, never an array length, and a model's
 * cap is never exceeded. */
uint32_t mu_model_cap(uint8_t model);     /* EUCL 64, TURI 16, TB3P 32, OFF 0 */
uint32_t mu_model_default(uint8_t model); /* EUCL 16, TURI 8, TB3P 16, OFF 0 */
/* The loop length the model plays. v is the length control's raw value (1..cap), or cap + 1 for
 * TRK: follow the key's track length as far as the model holds it. TURI never goes below 2. An
 * unknown track length (0) uses the model's default, so a loop is never 0. */
uint32_t mu_loop_len(uint8_t model, uint32_t v, const struct mu_key *key);
/* The loop position display step `step` plays, 0..len-1: the one rotation convention, so a picture
 * cannot turn a different way from the music. */
uint32_t mu_loop_pos(uint8_t model, uint32_t len, uint32_t rot, uint32_t step);

/* The loop's gates and the position each display step plays, for a picture or the trig keys.
 * gates bit p is loop position p (the first 16 positions a cell grid can show); play bit s is
 * display step s in play order (0..63), the same bits the render plays. Both go through
 * mu_loop_pos, so a picture cannot turn another way than the music. */
struct mu_preview {
    uint16_t gates;  /* bit p: loop position p fires (p < cells) */
    uint64_t play;   /* bit s: display step s fires, in play order */
    uint64_t accent; /* bit s: display step s plays an accented note */
    uint64_t slide;  /* bit s: display step s glides into the next (TB3P) */
    uint8_t len;     /* the effective loop length, 1..64 */
    uint8_t cells;   /* positions the picture can show: min(len, 16) */
    uint8_t start;   /* the loop position display step 0 plays: ROT's needle */
};
void mu_preview(const struct mu_track *t, const struct mu_key *key, struct mu_preview *out);
/* x of 255 against a percentage, the models' one threshold rule (100 admits every value). */
int mu_below(uint32_t x, uint32_t pct);

/* Knob metadata for the page. k: 0 is MODEL, 1..7 are B..H. step: detents one unit takes (4
 * counts is a detent); fast: what a turn of more than one detent multiplies that step by. */
int32_t mu_knob_min(uint8_t model, uint8_t k);
int32_t mu_knob_max(uint8_t model, uint8_t k);
int32_t mu_knob_step(uint8_t model, uint8_t k);
int32_t mu_knob_default(uint8_t model, uint8_t k);
int32_t mu_knob_fast(uint8_t model, uint8_t k);
const char *mu_knob_label(uint8_t model, uint8_t k);
const char *mu_model_name(uint8_t model);

/* Set one track's controls to the defaults; make a value valid after a change. */
void mu_defaults(struct mu_track *t);
void mu_clamp(struct mu_track *t);

/* Number of notes in a scale (1..12) and its semitones (0..11). */
uint32_t mu_scale_notes(int8_t scale, uint8_t out[12]);

/* LOCK is TURI's knob H (index MU_PARAMS - 1 in p[model]); 100 keeps the source. No other model
 * drifts: EUCL's H is RIFF (a melody, not a refresh) and TB3P's is its seed, which only a turn or
 * a push changes. */
#define MU_LOCK(t) ((t)->model == MU_TURI ? (t)->p[MU_TURI][MU_PARAMS - 1] : 100u)

/* Draw a whole new source from the seed counter. REROLL advances the counter first. */
void mu_fill(struct mu_track *t);
void mu_reroll(struct mu_track *t);
/* One pass of the pattern has played: replace each source step with probability 100-LOCK %. */
void mu_evolve(struct mu_track *t, uint32_t pass);

/* RIFF's line: one scale degree (0 to n-1) a source step, from a walk over the 16 source steps,
 * so consecutive hits are a step or two apart instead of independent draws. The same source and
 * scale always give the same degrees, so raising RIFF only adds hits and never moves a note. */
void mu_riff_degrees(const struct mu_track *t, uint32_t n, uint8_t out[MU_SRC]);

/* Render the 64-step table for the track's current model; all zero for MU_OFF. */
void mu_render(const struct mu_track *t, const struct mu_key *key, struct mu_step out[MU_STEPS]);

#endif
