/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host tests of MUTINY's published playback generations and cycle counting.
 *
 * A latched TB3P step must answer with the accent and slide
 * metadata of its own generation even after ACNT/SLID changed and before a new generation is
 * published. */
#include "../mods/mutiny/playback.h"
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

static struct mu_playback pb;
static struct mu_acid_cache acid[2];
static struct mu_track recipe;
static struct mu_key key = {60, 60, 5 /* C minor */, 0};
static struct mu_base base = {1000, 8000};

static const struct mu_voice *read_step(uint8_t track, uint32_t step, int first) {
    mu_pb_latch(&pb, track, first);
    return mu_pb_step(&pb, track, step);
}

static int has_lock(const struct mu_voice *v, uint16_t slot, int16_t *value) {
    uint32_t i;
    for (i = 0; i < v->nlock; i++)
        if (v->slot[i] == slot) {
            if (value)
                *value = v->value[i];
            return 1;
        }
    return 0;
}

/* The step whose generated note is accented, and whether it slides. */
static void tb3p_state(uint8_t acnt, uint8_t slid) {
    mu_defaults(&recipe);
    recipe.model = MU_TB3P;
    recipe.p[MU_TB3P][1] = 7;    /* GATE */
    recipe.p[MU_TB3P][2] = 8;    /* PTCH */
    recipe.p[MU_TB3P][3] = slid; /* SLID */
    recipe.p[MU_TB3P][4] = acnt; /* ACNT */
}

int main(void) {
    struct mu_step table[MU_STEPS];
    uint32_t i, accented = MU_STEPS, slid_step = MU_STEPS;

    mu_pb_init(&pb);
    mu_defaults(&recipe);
    CHECK(mu_pb_publish(&pb, 0, &recipe, &key, &base, &acid[0]) == -1,
          "OFF published a generation");
    CHECK(pb.on[0] == 0 && mu_pb_step(&pb, 0, 0) == 0, "OFF left output on");

    /* a published EUCL generation carries the render's steps and its effective length */
    mu_defaults(&recipe);
    recipe.model = MU_EUCL;
    recipe.p[MU_EUCL][4] = 100; /* ACNT */
    recipe.p[MU_EUCL][0] = 65;  /* TRK */
    key.len = 12;
    CHECK(mu_pb_publish(&pb, 0, &recipe, &key, &base, &acid[0]) == 1, "first publish");
    CHECK((pb.buf[0][1].gen & 1u) == 0 && pb.buf[0][1].gen == 2, "gen %u", pb.buf[0][1].gen);
    CHECK(pb.buf[0][1].len == 12 && pb.buf[0][1].valid, "len %u", pb.buf[0][1].len);
    mu_render(&recipe, &key, table);
    mu_pb_latch(&pb, 0, 1);
    for (i = 0; i < MU_STEPS; i++) {
        const struct mu_voice *v = mu_pb_step(&pb, 0, i);
        CHECK(v && v->trig == table[i].trig && v->note == table[i].note && v->vel == table[i].vel,
              "step %u does not match the render", i);
        if (table[i].trig && mu_step_accent(&table[i]))
            CHECK(v->nlock == 2 && has_lock(v, 55, 0) && has_lock(v, 72, 0),
                  "accented step %u: %u lock(s)", i, v->nlock);
        else if (table[i].trig)
            CHECK(v->nlock == 0, "plain step %u carries %u lock(s)", i, v->nlock);
    }
    /* the next publication is the other buffer and a later generation */
    CHECK(mu_pb_publish(&pb, 0, &recipe, &key, &base, &acid[0]) == 0, "second publish");
    CHECK(pb.buf[0][0].gen == 4, "second gen %u", pb.buf[0][0].gen);

    /* Latch an accented TB3P step, then change ACNT and SLID without a new generation */
    tb3p_state(100, 80);
    mu_pb_publish(&pb, 1, &recipe, &key, &base, &acid[1]);
    mu_render(&recipe, &key, table);
    for (i = 0; i < MU_STEPS; i++)
        if (table[i].trig && mu_step_accent(&table[i])) {
            accented = i;
            break;
        }
    for (i = 0; i < MU_STEPS; i++)
        if (table[i].trig && table[i].slide == MU_GLIDE_ON) {
            slid_step = i;
            break;
        }
    CHECK(accented < MU_STEPS, "the TB3P fixture has no accent");
    {
        const struct mu_voice *v = read_step(1, accented, 1);
        int16_t freq = 0, vol = 0, ptim = 0;
        CHECK(v && v->nlock >= 2, "accented TB3P step: %u locks", v ? v->nlock : 0);
        CHECK(v && has_lock(v, 55, &freq) && has_lock(v, 72, &vol), "no accent locks");
        CHECK(freq > base.freq && vol > base.vol, "the accent did not raise the sound");
        /* the encoder update that used to leak into a latched step */
        recipe.p[MU_TB3P][4] = 0;
        recipe.p[MU_TB3P][3] = 0;
        v = mu_pb_step(&pb, 1, accented);
        CHECK(v && v->nlock >= 2 && has_lock(v, 55, 0) && has_lock(v, 72, 0),
              "the latched step changed with an unpublished edit");
        v = read_step(1, accented, 0); /* the fire's second read */
        CHECK(v && v->nlock >= 2, "the second read lost the generation");
        if (slid_step < MU_STEPS) {
            v = read_step(1, slid_step, 1);
            CHECK(v && has_lock(v, 77, &ptim) && has_lock(v, 78, 0), "the slide lost its locks");
            CHECK(ptim > 0, "PTIM 0");
        }
        /* a new publication carries the edit */
        mu_pb_publish(&pb, 1, &recipe, &key, &base, &acid[1]);
        v = read_step(1, accented, 1);
        CHECK(v == 0 || v->nlock == 0 || !has_lock(v, 55, 0), "the new generation kept the accent");
    }

    /* the cycle counter: a 16-step pattern with an 8-step loop gives one cycle every 8 steps */
    {
        struct mu_cycle c;
        int coalesced = 0;
        mu_cycle_reset(&c);
        CHECK(mu_cycle_advance(&c, 0, 0, 16, 8, 1, &coalesced) == 0, "first observation");
        CHECK(mu_cycle_advance(&c, 4, 0, 16, 8, 1, &coalesced) == 0, "half a loop");
        CHECK(mu_cycle_advance(&c, 8, 0, 16, 8, 1, &coalesced) == 1, "one loop");
        CHECK(mu_cycle_advance(&c, 9, 0, 16, 8, 1, &coalesced) == 0, "just past");
        CHECK(mu_cycle_advance(&c, 15, 0, 16, 8, 1, &coalesced) == 0, "before the wrap");
        CHECK(mu_cycle_advance(&c, 0, 1, 16, 8, 1, &coalesced) == 1, "the wrap completes it");
    }

    /* TURI's TRK position uses its 16-step loop, never the raw 17 */
    {
        struct mu_cycle c;
        int coal = 0;
        uint32_t len;
        mu_defaults(&recipe);
        recipe.model = MU_TURI;
        recipe.p[MU_TURI][0] = 17;
        key.len = 64;
        len = mu_loop_len(MU_TURI, 17, &key);
        CHECK(len == 16, "TURI TRK loop %u", len);
        mu_cycle_reset(&c);
        mu_cycle_advance(&c, 60, 0, 64, len, 1, &coal);
        CHECK(mu_cycle_advance(&c, 63, 0, 64, len, 1, &coal) == 0, "no cycle inside 16");
        CHECK(mu_cycle_advance(&c, 12, 1, 64, len, 1, &coal) == 1, "a full 16 after the wrap");
    }

    /* a stopped re-arm (7 -> 0, no wrap) is not a cycle; neither is anything while stopped */
    {
        struct mu_cycle c;
        int coal = 0;
        mu_cycle_reset(&c);
        mu_cycle_advance(&c, 7, 0, 16, 8, 1, &coal);
        CHECK(mu_cycle_advance(&c, 0, 0, 16, 8, 1, &coal) == 0, "re-arm counted a cycle");
        mu_cycle_advance(&c, 3, 0, 16, 8, 1, &coal);
        CHECK(mu_cycle_advance(&c, 9, 0, 16, 8, 0, &coal) == 0, "a stopped clock counted");
        CHECK(mu_cycle_advance(&c, 0, 0, 16, 8, 1, &coal) == 0, "a start dragged the old position");
    }

    /* a tick that missed seven loops still counts seven, and says so */
    {
        struct mu_cycle c;
        int coal = 0;
        uint32_t n;
        mu_cycle_reset(&c);
        mu_cycle_advance(&c, 1, 0, 64, 8, 1, &coal);
        n = mu_cycle_advance(&c, 63, 0, 64, 8, 1, &coal);
        CHECK(n == 7, "missed loops counted %u", n);
        CHECK(coal == 1, "a coalesced observation was not flagged");
        n = mu_cycle_advance(&c, 4, 1, 64, 8, 1, &coal);
        CHECK(n == 1 && coal == 0, "the wrap after step 63 counted %u", n);
    }

    /* repeated starts and stops are one baseline each, never a burst of cycles */
    {
        struct mu_cycle c;
        int coal = 0;
        mu_cycle_reset(&c);
        for (i = 0; i < 10; i++) {
            mu_cycle_advance(&c, 0, (uint32_t)(i & 1), 16, 8, 1, &coal);
            CHECK(mu_cycle_advance(&c, 4, (uint32_t)(i & 1), 16, 8, 1, &coal) == 0, "start %u", i);
            CHECK(mu_cycle_advance(&c, 7, (uint32_t)(i & 1), 16, 8, 0, &coal) == 0, "stop %u", i);
        }
    }

    /* A length change must rebaseline instead of subtracting a stale position across
     * configurations (an earlier version returned 4294967291 here). */
    {
        struct mu_cycle c;
        int coal = 0;
        mu_cycle_reset(&c);
        CHECK(c.fresh == 1, "reset must require a baseline");
        mu_cycle_advance(&c, 63, 0, 64, 8, 1, &coal);
        CHECK(c.fresh == 1, "the first observation establishes a baseline");
        CHECK(mu_cycle_advance(&c, 0, 1, 16, 8, 1, &coal) == 0,
              "length change must establish a new clock baseline");
        CHECK(c.fresh == 1, "the rebaseline must be observable");
        CHECK(mu_cycle_advance(&c, 4, 1, 16, 8, 1, &coal) == 0 && c.fresh == 0,
              "normal progress is not a baseline");
    }

    /* every configuration change rebaselines instead of carrying a stale position */
    {
        struct mu_cycle c;
        int coal = 0;
        mu_cycle_reset(&c);
        mu_cycle_advance(&c, 63, 0, 64, 8, 1, &coal);
        CHECK(mu_cycle_advance(&c, 0, 1, 1, 8, 1, &coal) == 0, "64->1 rebaseline");
        CHECK(mu_cycle_advance(&c, 0, 1, 1, 8, 1, &coal) == 0, "a 1-step track never wraps");
        mu_cycle_reset(&c);
        mu_cycle_advance(&c, 0, 0, 1, 8, 1, &coal);
        CHECK(mu_cycle_advance(&c, 1, 0, 64, 8, 1, &coal) == 0, "1->64 rebaseline");
        mu_cycle_reset(&c);
        mu_cycle_advance(&c, 15, 0, 16, 16, 1, &coal);
        CHECK(mu_cycle_advance(&c, 0, 1, 16, 2, 1, &coal) == 0, "loop 16->2 rebaseline");
        mu_cycle_reset(&c);
        mu_cycle_advance(&c, 8, 0, 64, 8, 1, &coal);
        CHECK(mu_cycle_advance(&c, 9, 0, 32, 8, 1, &coal) == 0, "track 64->32 rebaseline");
        mu_cycle_reset(&c);
        CHECK(mu_cycle_advance(&c, 64, 0, 64, 8, 1, &coal) == 0 && c.have == 0,
              "an invalid position resets the clock");
        CHECK(mu_cycle_advance(&c, MU_CLOCK_NONE, 0, 64, 8, 1, &coal) == 0 && c.have == 0,
              "the no-clock sentinel resets the clock");
    }

    /* no-wrap backward jump, a normal wrap and a long-running counter */
    {
        struct mu_cycle c;
        int coal = 0;
        uint32_t i, sum = 0;
        mu_cycle_reset(&c);
        mu_cycle_advance(&c, 7, 0, 16, 8, 1, &coal);
        CHECK(mu_cycle_advance(&c, 0, 0, 16, 8, 1, &coal) == 0 && c.have == 0,
              "a no-wrap backward jump resets");
        mu_cycle_reset(&c);
        mu_cycle_advance(&c, 0, 0, 8, 8, 1, &coal);
        CHECK(mu_cycle_advance(&c, 7, 0, 8, 8, 1, &coal) == 0, "still inside the loop");
        CHECK(mu_cycle_advance(&c, 0, 1, 8, 8, 1, &coal) == 1, "the wrap completes one loop");
        /* two loop boundaries inside one track pass are both counted */
        mu_cycle_reset(&c);
        mu_cycle_advance(&c, 0, 0, 16, 8, 1, &coal);
        CHECK(mu_cycle_advance(&c, 7, 0, 16, 8, 1, &coal) == 0, "before the first boundary");
        CHECK(mu_cycle_advance(&c, 0, 1, 16, 8, 1, &coal) == 2,
              "7 -> 0 on a 16-step track crosses two 8-step boundaries");
        mu_cycle_reset(&c);
        mu_cycle_advance(&c, 0, 0, 64, 8, 1, &coal);
        for (i = 1; i <= 100000u; i++) {
            uint32_t last = i % 64u, parity = (i / 64u) & 1u;
            sum += mu_cycle_advance(&c, last, parity, 64, 8, 1, &coal);
        }
        CHECK(sum == 100000u / 8u, "a long run counted %u cycles", sum);
    }

    /* Scheduler regression: a transient failed observation must not clear overload. */
    {
        struct mu_cycle c;
        struct mu_evo e;
        uint8_t coal = 0;
        mu_cycle_reset(&c);
        mu_evo_reset(&e);
        mu_evo_schedule(&c, &e, 1, 0, 0, 64, 1, 1, 1, 0, &coal);
        e.pending = MU_EVO_PENDING_MAX;
        mu_evo_schedule(&c, &e, 1, 1, 0, 64, 1, 1, 1, 0, &coal);
        CHECK(e.desync, "overflow latches");
        mu_evo_schedule(&c, &e, 0, 0, 0, 0, 0, 1, 1, 0, &coal);
        mu_evo_schedule(&c, &e, 1, 2, 0, 64, 1, 1, 1, 0, &coal);
        CHECK(e.desync, "observation recovery preserves overload");
    }
    /* Losing an active clock makes progress unknowable, even without prior overflow. */
    {
        struct mu_cycle c;
        struct mu_evo e;
        uint8_t coal = 0;
        mu_cycle_reset(&c);
        mu_evo_reset(&e);
        mu_evo_schedule(&c, &e, 1, 0, 0, 64, 1, 1, 1, 0, &coal);
        mu_evo_schedule(&c, &e, -1, 0, 0, 0, 0, 1, 1, 0, &coal);
        CHECK(e.desync, "lost active observation freezes unknown progress");
    }
    /* Four production scheduler instances: bound work and invalidate stale opportunities. */
    {
        struct mu_cycle c[4];
        struct mu_evo e[4];
        uint8_t coal[4] = {0};
        int i, pass = 0;
        for (i = 0; i < 4; i++) {
            mu_cycle_reset(&c[i]);
            mu_evo_reset(&e[i]);
            CHECK(!mu_evo_schedule(&c[i], &e[i], 1, 0, 0, 64, 1, 1, 1, 0, &coal[i]),
                  "first observation");
            pass += mu_evo_schedule(&c[i], &e[i], 1, 63, 0, 64, 1, 1, 1, 0, &coal[i]);
            CHECK(e[i].pending == 62, "bounded backlog");
        }
        CHECK(pass == 4, "at most one pass per track");
        CHECK(!mu_evo_schedule(&c[0], &e[0], 1, 0, 1, 16, 2, 1, 1, 0, &coal[0]) && !e[0].pending,
              "configuration change discards backlog");
        CHECK(!mu_evo_schedule(&c[1], &e[1], 1, 0, 1, 64, 1, 1, 1, 1, &coal[1]) && !e[1].pending,
              "hold discards backlog");
        CHECK(!mu_evo_schedule(&c[1], &e[1], 1, 0, 1, 64, 1, 1, 1, 0, &coal[1]),
              "release does not replay");
        CHECK(!mu_evo_schedule(&c[2], &e[2], 1, 0, 0, 64, 1, 0, 1, 0, &coal[2]) && !e[2].pending,
              "stop discards backlog");
        CHECK(!mu_evo_schedule(&c[2], &e[2], 1, 0, 0, 64, 1, 1, 1, 0, &coal[2]),
              "restart rebaselines");
        CHECK(!mu_evo_schedule(&c[3], &e[3], 1, 0, 1, 64, 1, 1, 0, 0, &coal[3]) && !e[3].pending,
              "native source discards backlog");
        CHECK(!mu_evo_schedule(&c[3], &e[3], 0, 0, 0, 0, 0, 1, 0, 0, &coal[3]),
              "OFF discards observation");
        mu_evo_reset(&e[3]);
        mu_cycle_reset(&c[3]);
        CHECK(!e[3].desync && !e[3].pending, "session initialization resets scheduler");
    }
    /* the evolution budget: one pass per tick, fixed backlog, deterministic overload */
    {
        struct mu_evo e;
        int i;
        mu_evo_reset(&e);
        CHECK(mu_evo_step(&e, 3, 1, 0) == 1, "the first pending pass");
        CHECK(e.pending == 2, "pending %u", e.pending);
        CHECK(mu_evo_step(&e, 0, 1, 0) == 1 && mu_evo_step(&e, 0, 1, 0) == 1, "drain");
        CHECK(mu_evo_step(&e, 0, 1, 0) == 0, "an empty backlog performs no pass");
        mu_evo_reset(&e);
        CHECK(mu_evo_step(&e, MU_EVO_PENDING_MAX, 1, 0) == 1 && e.pending == MU_EVO_PENDING_MAX - 1,
              "a full backlog is accepted");
        CHECK(mu_evo_step(&e, 2, 1, 0) == 0 && e.desync == 1, "overflow desynchronises");
        CHECK(mu_evo_step(&e, 0, 1, 0) == 0 && mu_evo_step(&e, 8, 1, 0) == 0,
              "a desynchronised tracker stays frozen");
        mu_evo_reset(&e);
        CHECK(mu_evo_step(&e, 1, 1, 0) == 1, "a reset establishes a new baseline");
        mu_evo_reset(&e);
        CHECK(mu_evo_step(&e, MU_EVO_PENDING_MAX + 1u, 1, 0) == 0 && e.desync == 1,
              "one oversized observation desynchronises");
        mu_evo_reset(&e);
        CHECK(mu_evo_step(&e, 5, 1, 1) == 0 && e.pending == 0, "HOLD drops opportunities");
        CHECK(mu_evo_step(&e, 1, 1, 0) == 1 && e.pending == 0, "no burst after HOLD");
        mu_evo_reset(&e);
        CHECK(mu_evo_step(&e, 5, 0, 0) == 0 && e.pending == 0,
              "a session that cannot evolve drops opportunities");
        {
            struct mu_evo four[4];
            int tick, pass, total;
            for (i = 0; i < 4; i++) {
                mu_evo_reset(&four[i]);
                CHECK(mu_evo_step(&four[i], MU_EVO_PENDING_MAX, 1, 0) == 1, "seed %d", i);
            }
            for (tick = 0; tick < 200; tick++) {
                pass = 0;
                for (i = 0; i < 4; i++)
                    pass += mu_evo_step(&four[i], 0, 1, 0);
                CHECK(pass <= 4, "tick %d performed %d passes", tick, pass);
                total = 0;
                for (i = 0; i < 4; i++)
                    total += four[i].pending;
                CHECK(total <= 256, "backlog reached %d entries", total);
            }
        }
    }

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("playback tests passed\n");
    return 0;
}
