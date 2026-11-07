/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host tests of the commit plan and its undo, against the measured OS 1.44 track layout and a fake
 * device. */
#include "../mods/mutiny/commit.h"
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

#define TR_VEL 0x80
#define TR_COND 0x140
#define TR_LEN 0xc0
#define TR_NOTES 0x1c0
#define LK_STEP 0xa0
#define LK_COUNT 0x9f
#define LK_USED 0x2800

struct fake {
    uint8_t data[MU_NATIVE_BYTES];
    uint32_t id;
    int corrupt_at; /* put() flips this offset once, then clears it */
    int id_calls;
    int id_change_at; /* identity() changes from this call on, when nonzero */
};

static uint8_t fget(void *ctx, uint32_t off) { return ((struct fake *)ctx)->data[off]; }
static void fput(void *ctx, uint32_t off, uint8_t value) {
    struct fake *f = ctx;
    if ((int)off == f->corrupt_at) {
        value ^= 0x01;
        f->corrupt_at = -1;
    }
    f->data[off] = value;
}
static uint32_t fid(void *ctx) {
    struct fake *f = ctx;
    f->id_calls++;
    if (f->id_change_at && f->id_calls >= f->id_change_at)
        f->id = f->id + 1u;
    return f->id;
}
static struct mu_device DEV = {0, fget, fput, fid};

/* an empty track as the firmware has it: zero step words, 0xff per-step data and notes,
 * 0xffff locks, zero counts and in-use flags */
static void init_native(struct mu_native *n) {
    uint32_t i;
    memset(n->b, 0, sizeof n->b);
    memset(n->b + TR_VEL, 0xff, 64);
    memset(n->b + TR_LEN, 0xff, 64);
    memset(n->b + TR_COND, 0xff, 64);
    memset(n->b + TR_NOTES, 0xff, 8 * 64);
    memset(n->b + MU_TRACK_BYTES, 0xff, MU_TRACK_LOCKS);
    for (i = 0; i < 64; i++)
        n->b[MU_TRACK_BYTES + LK_STEP * i + LK_COUNT] = 0;
    memset(n->b + MU_TRACK_BYTES + LK_USED, 0, 79);
    n->b[0x3c2] = 16;
}

static void note(struct mu_native *n, uint32_t step, uint8_t pitch, uint8_t vel) {
    n->b[2 * step + 1] |= 0x01;
    n->b[TR_NOTES + 8 * step] = pitch;
    for (int k = 1; k < 8; k++)
        n->b[TR_NOTES + 8 * step + k] = 0;
    n->b[TR_VEL + step] = vel;
    n->b[TR_COND + step] = 0xff;
}

static void lock(struct mu_native *n, uint32_t step, uint32_t slot, uint16_t value) {
    uint8_t *b = n->b + MU_TRACK_BYTES + LK_STEP * step;
    if (b[2 * slot] == 0xff && b[2 * slot + 1] == 0xff)
        b[LK_COUNT]++;
    b[2 * slot] = (uint8_t)(value >> 8);
    b[2 * slot + 1] = (uint8_t)value;
    n->b[MU_TRACK_BYTES + LK_USED + slot] = 1;
}

static uint16_t lock_value(const struct mu_native *n, uint32_t step, uint32_t slot) {
    const uint8_t *b = n->b + MU_TRACK_BYTES + LK_STEP * step;
    return (uint16_t)(b[2 * slot] << 8 | b[2 * slot + 1]);
}

static uint8_t lock_count(const struct mu_native *n, uint32_t step) {
    return n->b[MU_TRACK_BYTES + LK_STEP * step + LK_COUNT];
}

int main(void) {
    struct mu_native prior, plan;
    struct mu_voice voice[MU_STEPS];
    struct mu_commit c;
    struct fake dev;

    /* The extent follows the active track length, never the generator's loop */
    CHECK(mu_commit_extent(16) == 16 && mu_commit_extent(64) == 64, "a plain track length");
    CHECK(mu_commit_extent(1) == 1 && mu_commit_extent(0) == 0, "the ends of the range");
    CHECK(mu_commit_extent(200) == 64, "a length past the buffer was not clamped");
    CHECK(mu_commit_extent(-3) == 0, "a negative length was accepted");

    /* A page commit's transaction is the page's 16 steps, clamped to the track */
    {
        uint32_t first = 99, end = 99;
        CHECK(!mu_commit_page(0, 0, &first, &end), "an empty track accepted a page");
        CHECK(mu_commit_page(0, 64, &first, &end) && first == 0 && end == 16, "page 1 is 0..16");
        CHECK(mu_commit_page(1, 64, &first, &end) && first == 16 && end == 32, "page 2 is 16..32");
        CHECK(mu_commit_page(2, 40, &first, &end) && first == 32 && end == 40,
              "page 3 did not clamp to the length");
        CHECK(!mu_commit_page(2, 32, &first, &end), "page 3 past the length was accepted");
        CHECK(!mu_commit_page(1, 16, &first, &end), "page 2 of a 16-step track was accepted");
        CHECK(mu_commit_page(3, 64, &first, &end) && first == 48 && end == 64, "page 4 is 48..64");
        CHECK(!mu_commit_page(4, 64, &first, &end), "a fifth page was accepted");
    }

    /* A page commit writes that page and leaves every other step byte for byte */
    {
        uint32_t first, end, i;
        init_native(&prior);
        memset(voice, 0, sizeof voice);
        for (i = 0; i < 64; i++) {
            voice[i].trig = 1;
            voice[i].note = (uint8_t)(60 + i);
            voice[i].vel = 100;
        }
        CHECK(mu_commit_page(1, 64, &first, &end), "page 2 missing");
        mu_commit_plan(&plan, &prior, voice, first, end, 0);
        {
            struct mu_native expected;
            struct mu_voice restricted[MU_STEPS];
            memcpy(restricted, voice, sizeof restricted);
            for (i = 0; i < MU_STEPS; i++)
                if (i < first || i >= end)
                    memset(&restricted[i], 0, sizeof restricted[i]);
            mu_commit_plan(&expected, &prior, restricted, first, end, 0);
            CHECK(memcmp(&plan, &expected, sizeof plan) == 0, "outside voices changed image");
        }
        for (i = 0; i < MU_NATIVE_BYTES; i++) {
            int allowed = (i >= 2 * first && i < 2 * end) ||
                          (i >= MU_PORT_TR_NOTES + 8 * first && i < MU_PORT_TR_NOTES + 8 * end) ||
                          (i >= MU_PORT_TR_VEL + first && i < MU_PORT_TR_VEL + end) ||
                          (i >= MU_PORT_TR_COND + first && i < MU_PORT_TR_COND + end);
            CHECK(allowed || plan.b[i] == prior.b[i], "byte %u changed outside page", i);
        }
        for (i = 0; i < 64; i++) {
            int inside = i >= first && i < end;
            uint16_t word = (uint16_t)(plan.b[2 * i] << 8 | plan.b[2 * i + 1]);
            CHECK((word & 1u) == (inside ? 1u : 0u), "step %u trig bit", i);
            CHECK(plan.b[TR_NOTES + 8 * i] == (inside ? (uint8_t)(60 + i) : 0xff), "step %u note",
                  i);
            CHECK(plan.b[TR_VEL + i] == (inside ? 100 : 0xff), "step %u velocity", i);
            CHECK(memcmp(plan.b + 2 * i, prior.b + 2 * i, 2) == 0 || inside,
                  "step %u word changed outside the page", i);
        }
    }

    memset(voice, 0, sizeof voice);
    init_native(&prior);

    /* A plain generated note over a hand-made FREQ/VOL lock keeps that lock */
    {
        note(&prior, 0, 60, 90);
        lock(&prior, 0, 55, 0x1200); /* FREQ */
        lock(&prior, 0, 72, 0x2200); /* VOL */
        voice[0].trig = 1;
        voice[0].note = 64;
        voice[0].vel = 100;
        CHECK(lock_count(&prior, 0) == 2, "fixture count %u", lock_count(&prior, 0));
        mu_commit_plan(&plan, &prior, voice, 0, 16, 0);
        CHECK(plan.b[TR_NOTES] == 64 && plan.b[TR_VEL] == 100, "the note did not land");
        CHECK(lock_value(&plan, 0, 55) == 0x1200 && lock_value(&plan, 0, 72) == 0x2200,
              "KEEP deleted a manual FREQ/VOL lock");
        CHECK(lock_count(&plan, 0) == 2, "lock count moved to %u", lock_count(&plan, 0));
        CHECK(plan.b[MU_TRACK_BYTES + LK_USED + 55] == 1, "the FREQ slot lost its in-use flag");
    }

    /* an accent writes its own two locks, exactly where it plays */
    {
        memset(voice, 0, sizeof voice);
        voice[1].trig = 1;
        voice[1].note = 67;
        voice[1].vel = 127;
        voice[1].nlock = 2;
        voice[1].slot[0] = 55;
        voice[1].value[0] = 0x3456;
        voice[1].slot[1] = 72;
        voice[1].value[1] = 0x4567;
        mu_commit_plan(&plan, &prior, voice, 0, 16, 0);
        CHECK(lock_value(&plan, 1, 55) == 0x3456 && lock_value(&plan, 1, 72) == 0x4567,
              "the accent's locks did not land");
        CHECK(lock_count(&plan, 1) == 2, "accent count %u", lock_count(&plan, 1));
        CHECK(lock_value(&plan, 0, 55) == 0x1200, "another step's lock moved");
    }

    /* TB3P's slide writes PORT/PTIM and a held gate lengthens LEN */
    {
        memset(voice, 0, sizeof voice);
        voice[2].trig = 1;
        voice[2].note = 60;
        voice[2].vel = 100;
        voice[2].slide = 1; /* MU_GLIDE_ON */
        voice[2].hold = 1;
        voice[2].nlock = 2;
        voice[2].slot[0] = 77;
        voice[2].value[0] = 64 << 8;
        voice[2].slot[1] = 78;
        voice[2].value[1] = 0x4000;
        prior.b[TR_LEN + 2] = 0xff;
        prior.b[0x3c2] = 16; /* the track's LEN */
        mu_commit_plan(&plan, &prior, voice, 0, 16, 0);
        CHECK(lock_value(&plan, 2, 77) == 64 << 8 && lock_value(&plan, 2, 78) == 0x4000,
              "the glide's locks did not land");
        CHECK(plan.b[TR_LEN + 2] != 0xff && plan.b[TR_LEN + 2] > 16, "the held gate kept LEN %u",
              plan.b[TR_LEN + 2]);
    }

    /* a generated rest clears the note but keeps a lock trig and its locks, as playback did */
    {
        memset(voice, 0, sizeof voice);
        note(&prior, 3, 62, 88);
        prior.b[2 * 3 + 1] |= 0x02; /* a lock trig */
        lock(&prior, 3, 18, 0x0abc);
        mu_commit_plan(&plan, &prior, voice, 0, 16, 0);
        CHECK((plan.b[2 * 3 + 1] & 0x01) == 0, "the note trig survived a rest");
        CHECK((plan.b[2 * 3 + 1] & 0x02) != 0, "the lock trig was removed with the note");
        CHECK(plan.b[TR_NOTES + 8 * 3] == 0xff && plan.b[TR_VEL + 3] == 0xff &&
                  plan.b[TR_COND + 3] == 0xff && plan.b[TR_LEN + 3] == 0xff,
              "the rest left stale per-step data");
        CHECK(lock_value(&plan, 3, 18) == 0x0abc, "the rest dropped a stored lock");
    }

    /* past the track's length: not one byte moves, and another track is not part of this image */
    {
        memset(voice, 0, sizeof voice);
        note(&prior, 20, 50, 80);
        lock(&prior, 20, 55, 0x1111);
        prior.b[2 * 20 + 1] |= 0x02;
        mu_commit_plan(&plan, &prior, voice, 0, 16, 0);
        CHECK(plan.b[TR_NOTES + 8 * 20] == 50 && plan.b[TR_VEL + 20] == 80 &&
                  (plan.b[2 * 20 + 1] & 0x03) == 0x03,
              "a step past the length was touched");
        CHECK(lock_value(&plan, 20, 55) == 0x1111, "a lock past the length was touched");
    }

    /* apply writes the plan; undo puts every byte of the prior image back */
    {
        struct mu_native after;
        memset(&dev, 0, sizeof dev);
        memset(&c, 0, sizeof c);
        DEV.ctx = &dev;
        dev.corrupt_at = -1;
        dev.id = 0x1234;
        memcpy(dev.data, prior.b, MU_NATIVE_BYTES);
        memset(voice, 0, sizeof voice);
        for (int i = 0; i < 64; i++) {
            voice[i].trig = 1;
            voice[i].note = (uint8_t)(60 + i % 12);
            voice[i].vel = 100;
        }
        note(&prior, 0, 60, 90);
        mu_commit_plan(&plan, &prior, voice, 0, 64, 0); /* a whole track: all 64 steps */
        CHECK(mu_commit_prepare(&DEV, &c, voice, 0, 64, &plan, 0), "prepare failed");
        CHECK(mu_commit_apply(&DEV, &c, &plan), "apply failed");
        CHECK(memcmp(dev.data, plan.b, MU_NATIVE_BYTES) == 0, "the device does not hold the plan");
        CHECK(c.valid && c.identity == 0x1234 && c.len == 64, "the undo record is incomplete");
        memcpy(after.b, dev.data, MU_NATIVE_BYTES);
        CHECK(mu_commit_undo(&DEV, &c), "undo failed");
        CHECK(memcmp(dev.data, prior.b, MU_NATIVE_BYTES) == 0,
              "undo did not restore the prior image");
        CHECK(!c.valid, "undo left the record valid");
        CHECK(!mu_commit_undo(&DEV, &c), "a second undo acted");
        (void)after;
    }

    /* a native edit after the commit makes the record stale: NO must not overwrite that edit */
    {
        memset(&dev, 0, sizeof dev);
        memset(&c, 0, sizeof c);
        DEV.ctx = &dev;
        dev.corrupt_at = -1;
        dev.id = 21;
        memcpy(dev.data, prior.b, MU_NATIVE_BYTES);
        CHECK(mu_commit_prepare(&DEV, &c, voice, 0, 64, &plan, 0), "prepare failed");
        CHECK(mu_commit_apply(&DEV, &c, &plan), "apply failed");
        dev.data[TR_NOTES + 8] = 0x42; /* a hand edit to a step the commit wrote */
        CHECK(!mu_commit_undo(&DEV, &c), "undo overwrote a native edit");
        CHECK(dev.data[TR_NOTES + 8] == 0x42, "the native edit was lost");
        CHECK(!c.valid, "a stale undo record stayed valid");
    }

    /* a readback that disagrees restores the prior bytes and reports failure */
    {
        memset(&dev, 0, sizeof dev);
        memset(&c, 0, sizeof c);
        DEV.ctx = &dev;
        dev.corrupt_at = -1;
        dev.id = 7;
        memcpy(dev.data, prior.b, MU_NATIVE_BYTES);
        dev.corrupt_at = TR_NOTES + 5;
        CHECK(mu_commit_prepare(&DEV, &c, voice, 0, 64, &plan, 0), "prepare failed");
        CHECK(!mu_commit_apply(&DEV, &c, &plan), "a corrupt readback reported success");
        CHECK(memcmp(dev.data, prior.b, MU_NATIVE_BYTES) == 0,
              "the failed write was not rolled back");
        CHECK(!c.valid, "a failed commit kept an undo record");
    }

    /* a pattern change during the write is never restored over: no rollback into new music */
    {
        memset(&dev, 0, sizeof dev);
        memset(&c, 0, sizeof c);
        DEV.ctx = &dev;
        dev.corrupt_at = -1;
        dev.id = 9;
        memcpy(dev.data, prior.b, MU_NATIVE_BYTES);
        dev.id_change_at = 2; /* changes between the prior read and the readback */
        CHECK(!mu_commit_apply(&DEV, &c, &plan), "an identity change reported success");
        CHECK(!c.valid, "an identity change kept an undo record");
    }

    /* undo after the identity moved does not touch the new pattern */
    {
        struct mu_native keep;
        memset(&dev, 0, sizeof dev);
        memset(&c, 0, sizeof c);
        DEV.ctx = &dev;
        dev.corrupt_at = -1;
        dev.id = 11;
        memcpy(dev.data, prior.b, MU_NATIVE_BYTES);
        CHECK(mu_commit_prepare(&DEV, &c, voice, 0, 64, &plan, 0), "prepare failed");
        CHECK(mu_commit_apply(&DEV, &c, &plan), "apply failed");
        memcpy(keep.b, dev.data, MU_NATIVE_BYTES);
        dev.id = 12;
        CHECK(!mu_commit_undo(&DEV, &c), "undo wrote into another pattern");
        CHECK(memcmp(dev.data, keep.b, MU_NATIVE_BYTES) == 0,
              "undo changed the new pattern's bytes");
    }

    /* A manual per-step LEN is kept, and repeated commits do not compound */
    {
        struct mu_len_memo memo;
        struct mu_native a1, a2;
        memset(voice, 0, sizeof voice);
        init_native(&prior);
        prior.b[0x3c2] = 14;    /* the track's default LEN index */
        prior.b[0xc0 + 0] = 62; /* the step's own, longer LEN */
        voice[0].trig = 1;
        voice[0].note = 60;
        voice[0].vel = 100;
        voice[0].hold = 1;
        mu_len_memo_init(&memo);
        mu_commit_plan(&a1, &prior, voice, 0, 16, &memo);
        CHECK(a1.b[0xc0] == 62, "a manual LEN 62 became %u", a1.b[0xc0]);
        /* the default case still extends once, and a second commit does not extend again */
        prior.b[0xc0] = 0xff;
        mu_len_memo_init(&memo);
        mu_len_memo_bind(&memo, 0x0a0a);
        mu_commit_plan(&a1, &prior, voice, 0, 16, &memo);
        CHECK(a1.b[0xc0] == (uint8_t)mu_held_len(14), "the default LEN became %u", a1.b[0xc0]);
        mu_commit_plan(&a2, &a1, voice, 0, 16, &memo);
        CHECK(a2.b[0xc0] == a1.b[0xc0], "the second commit compounded LEN %u -> %u", a1.b[0xc0],
              a2.b[0xc0]);
        /* a manual edit after a commit is adopted as the new base */
        a1.b[0xc0] = 20;
        mu_commit_plan(&a2, &a1, voice, 0, 16, &memo);
        CHECK(a2.b[0xc0] == (uint8_t)mu_held_len(20), "a manual edit after commit was ignored: %u",
              a2.b[0xc0]);
    }

    /* LEN provenance belongs to a native pattern, not to a byte value that happens to match
     */
    {
        struct mu_len_memo memo;
        struct mu_native a1, a2;
        memset(voice, 0, sizeof voice);
        init_native(&prior);
        prior.b[0x3c2] = 14;
        voice[0].trig = 1;
        voice[0].note = 60;
        voice[0].vel = 100;
        voice[0].hold = 1;
        mu_len_memo_init(&memo);
        mu_len_memo_bind(&memo, 0x1111);
        mu_commit_plan(&a1, &prior, voice, 0, 16, &memo);
        CHECK(a1.b[0xc0] == (uint8_t)mu_held_len(14), "pattern A's default extension became %u",
              a1.b[0xc0]);
        /* pattern B: a distinct buffer whose step carries a manual LEN equal to A's written byte */
        prior.b[0xc0] = a1.b[0xc0];
        mu_len_memo_bind(&memo, 0x2222);
        CHECK(memo.base[0] == 0xff, "a memo from another pattern kept its entries");
        mu_commit_plan(&a2, &prior, voice, 0, 16, &memo);
        CHECK(a2.b[0xc0] == (uint8_t)mu_held_len(prior.b[0xc0]),
              "pattern B's manual LEN was read as Mutiny's own: %u, expected %u", a2.b[0xc0],
              (uint8_t)mu_held_len(prior.b[0xc0]));
        /* the same identity still keeps its record, so a repeated commit does not compound */
        a2.b[0xc0] = 22;
        mu_len_memo_init(&memo);
        mu_len_memo_bind(&memo, 0x3333);
        mu_commit_plan(&a1, &prior, voice, 0, 16, &memo);
        mu_len_memo_bind(&memo, 0x3333);
        mu_commit_plan(&a2, &a1, voice, 0, 16, &memo);
        CHECK(a2.b[0xc0] == a1.b[0xc0], "a same-pattern second commit compounded LEN %u -> %u",
              a1.b[0xc0], a2.b[0xc0]);
        /* prepare binds the memo to the device's verified identity and drops foreign entries */
        mu_len_memo_init(&memo);
        mu_len_memo_bind(&memo, 0x9999);
        mu_held_record(&memo, 0, 14);
        memset(&dev, 0, sizeof dev);
        memcpy(dev.data, prior.b, MU_NATIVE_BYTES);
        dev.id = 0x4444;
        dev.corrupt_at = -1;
        DEV.ctx = &dev;
        CHECK(mu_commit_prepare(&DEV, &c, voice, 0, 16, &plan, &memo), "prepare failed");
        CHECK(memo.identity == 0x4444 && plan.b[0xc0] == (uint8_t)mu_held_len(22),
              "prepare used a memo from another pattern (id %08x, LEN %u)", memo.identity,
              plan.b[0xc0]);
        /* the step fire only trusts a memo bound to the pattern it is reading */
        CHECK(mu_len_memo_for(&memo, 0x4444) == &memo, "a bound memo was refused");
        CHECK(mu_len_memo_for(&memo, 0x5555) == 0, "a memo from another pattern was trusted");
        CHECK(mu_len_memo_for(&memo, 0) == 0, "an unbound read was trusted");
    }

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("commit tests passed\n");
    return 0;
}
