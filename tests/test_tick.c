/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host tests of MUTINY's UI tick at its production boundary.
 *
 * The fixture links the real tick, playback, runtime, engine and record modules and binds only the
 * two device reads the tick needs (pattern observation, sound base) to counting fakes. It therefore
 * exercises the code the firmware runs: one observation per relevant track per tick, the record
 * written once for a batch's final semantic state and never for an output-only refresh, and a
 * failed observation keeping both the last complete generation and the retry.
 */
#include "../mods/mutiny/tick.h"
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

#define TRACKS MU_TICK_TRACKS

#ifdef MU_ENGINE_PROBE
extern uint32_t mu_engine_acid_generations, mu_engine_previews, mu_engine_renders;
#endif

static int preview_equal(const struct mu_preview *a, const struct mu_preview *b) {
    return a->gates == b->gates && a->play == b->play && a->accent == b->accent &&
           a->slide == b->slide && a->len == b->len && a->cells == b->cells && a->start == b->start;
}

/* The device boundary the fixture binds */
static struct mu_track_info dev_info[TRACKS];
static uint32_t dev_calls[TRACKS];
static int dev_fail[TRACKS];
static int16_t dev_freq[TRACKS], dev_vol[TRACKS];
static int dev_kit = 1;

int32_t mu_dev_track_info(int32_t track, struct mu_track_info *out) {
    if (track < 0 || (uint32_t)track >= TRACKS)
        return -1;
    dev_calls[track]++;
    if (dev_fail[track])
        return -1;
    *out = dev_info[track];
    return 0;
}

int32_t mu_dev_sound_base(struct mu_base out[TRACKS]) {
    uint32_t t;
    if (!dev_kit)
        return -1;
    for (t = 0; t < TRACKS; t++) {
        out[t].freq = dev_freq[t];
        out[t].vol = dev_vol[t];
    }
    return 0;
}

/* The mod's state, as mutiny.c owns it */
static struct mu_rt rt;
static struct mu_playback pb;
static struct mu_store save;
static struct mu_cycle cyc[TRACKS];
static struct mu_evo evo[TRACKS];
static struct mu_base base[TRACKS];
static struct mu_key seen[TRACKS];
static uint32_t passes[TRACKS];
static uint8_t seen_ok[TRACKS], dirty[TRACKS], coalesced[TRACKS];

static struct mu_tick tick = {.rt = &rt,
                              .pb = &pb,
                              .save = &save,
                              .cyc = cyc,
                              .evo = evo,
                              .base = base,
                              .seen = seen,
                              .passes = passes,
                              .seen_ok = seen_ok,
                              .dirty = dirty,
                              .coalesced = coalesced};

static struct mu_tick_clock clk;

static void fresh(void) {
    uint32_t t;
    mu_rt_init(&rt);
    mu_pb_init(&pb);
    mu_store_defaults(&save);
    memset(&clk, 0, sizeof clk);
    clk.hold_track = 0xff;
    for (t = 0; t < TRACKS; t++) {
        mu_cycle_reset(&cyc[t]);
        mu_evo_reset(&evo[t]);
        passes[t] = 0;
        seen_ok[t] = 0;
        dirty[t] = 1;
        coalesced[t] = 0;
        base[t].freq = 0;
        base[t].vol = 0;
        dev_info[t].len = 16;
        dev_info[t].master = 16;
        dev_info[t].note = 60;
        dev_info[t].root = 0;
        dev_info[t].scale = -1;
        dev_calls[t] = 0;
        dev_fail[t] = 0;
        dev_freq[t] = 0;
        dev_vol[t] = 0;
    }
    dev_kit = 1;
    mu_tick_reset(&tick);
}

static void clear_calls(void) {
    uint32_t t;
    for (t = 0; t < TRACKS; t++)
        dev_calls[t] = 0;
    mu_store_probe_saves = 0;
}

/* One tick, the batch's preview preparation phase and its single serialization point. The test
 * uses the same production boundary as the firmware: prepare after the tick's commands. */
static uint32_t run_batch(uint8_t page_track) {
    uint32_t rearm = mu_tick_run(&tick, &clk, page_track);
    mu_tick_prepare(&tick, page_track, 1);
    mu_tick_flush(&tick);
    return rearm;
}

/* The same batch while the page is closed: no preview work may be moved onto hidden playback. */
static uint32_t run_batch_hidden(uint8_t page_track) {
    uint32_t rearm = mu_tick_run(&tick, &clk, page_track);
    mu_tick_prepare(&tick, page_track, 0);
    mu_tick_flush(&tick);
    return rearm;
}

static uint32_t observations(void) {
    uint32_t t, total = 0;
    for (t = 0; t < TRACKS; t++)
        total += dev_calls[t];
    return total;
}

static void check_one_observation(const char *what) {
    uint32_t t;
    for (t = 0; t < TRACKS; t++)
        CHECK(dev_calls[t] <= 1, "%s: T%u was observed %u times in one tick", what, t + 1,
              dev_calls[t]);
}

static int published(uint8_t t) {
    const struct mu_snapshot *s = &pb.buf[t][pb.cur[t]];
    return s->valid && s->gen == pb.gen[t] && pb.on[t];
}

/* Every field of all 64 published steps must still be the pure render of this tick's recipe/key.
 * This is the production path (tick -> publish), not a legacy render entry point. */
static int published_matches_render(uint8_t track) {
    const struct mu_key *key = mu_tick_key(&tick, track);
    struct mu_step pure[MU_STEPS];
    const struct mu_snapshot *s = &pb.buf[track][pb.cur[track]];
    uint32_t i;
    if (!key || !published(track))
        return 0;
    mu_render(&rt.t[track].recipe, key, pure);
    for (i = 0; i < MU_STEPS; i++)
        if (s->step[i].trig != pure[i].trig || s->step[i].note != pure[i].note ||
            s->step[i].vel != pure[i].vel || s->step[i].slide != pure[i].slide ||
            s->step[i].hold != pure[i].hold)
            return 0;
    return 1;
}

static int record_matches_runtime(void) {
    struct mu_store want;
    uint32_t writes = mu_store_probe_saves;
    mu_store_save(&want, &rt);
    mu_store_probe_saves = writes; /* the reference serialization is not a batch write */
    return memcmp(&save, &want, sizeof save) == 0;
}

static struct mu_rt loaded_record;

static int load_record(void) { return mu_store_decode(&loaded_record, save.bytes, MU_STORE_BYTES); }

static void select_all(uint8_t model0, uint8_t model1, uint8_t model2, uint8_t model3) {
    mu_rt_select_model(&rt.t[0], model0);
    mu_rt_select_model(&rt.t[1], model1);
    mu_rt_select_model(&rt.t[2], model2);
    mu_rt_select_model(&rt.t[3], model3);
}

int main(void) {
    uint32_t t, r;

    /* Four dirty tracks: one observation each, one record write for the batch */
    fresh();
    select_all(MU_EUCL, MU_TURI, MU_TB3P, MU_EUCL);
    for (t = 0; t < TRACKS; t++)
        dirty[t] = 1;
    clear_calls();
    r = run_batch(0);
    CHECK(r == 0xfu, "four dirty tracks must rearm all four, got %#x", r);
    for (t = 0; t < TRACKS; t++) {
        CHECK(dev_calls[t] == 1, "T%u observed %u times, expected 1", t + 1, dev_calls[t]);
        CHECK(dirty[t] == 0, "T%u stayed dirty after a successful render", t + 1);
        CHECK(published(t), "T%u has no complete published generation", t + 1);
        CHECK(mu_tick_observed(&tick, (uint8_t)t), "T%u observation was not kept", t + 1);
    }
    CHECK(mu_store_probe_saves == 1, "the batch wrote the record %u times, expected 1",
          mu_store_probe_saves);
    CHECK(record_matches_runtime(), "the written record is not the runtime state");

    /* No dirty tracks: still one observation per track, nothing written */
    clear_calls();
    r = run_batch(0);
    CHECK(r == 0, "an idle tick must not rearm, got %#x", r);
    CHECK(observations() == 4, "an idle tick made %u observations, expected 4", observations());
    check_one_observation("idle");
    CHECK(mu_store_probe_saves == 0, "an idle tick wrote the record %u times",
          mu_store_probe_saves);

    /* FREQ/VOL-only refresh: republish, never serialize */
    {
        struct mu_track after = rt.t[0].recipe;
        after.p[MU_EUCL][4] = 60; /* ACNT: the track's accents follow the sound's FREQ/VOL */
        mu_rt_edit(&rt.t[0], &after);
        dirty[0] = 1;
        clear_calls();
        run_batch(0);
        CHECK(mu_store_probe_saves == 1, "the ACNT edit must be serialized once, got %u",
              mu_store_probe_saves);
    }
    {
        uint32_t gen0 = pb.gen[0], gen1 = pb.gen[1];
        struct mu_store written = save;
        dev_freq[0] = 0x1000;
        clear_calls();
        r = run_batch(0);
        CHECK(r == 0x1u, "a base refresh rearms its own track only, got %#x", r);
        CHECK(pb.gen[0] != gen0 && published(0), "T1 was not republished from the new base");
        CHECK(pb.gen[1] == gen1, "T2 was republished by another track's base");
        CHECK(mu_store_probe_saves == 0, "a base-only refresh wrote the record %u times",
              mu_store_probe_saves);
        CHECK(memcmp(&written, &save, sizeof save) == 0, "the record changed on a base refresh");
        check_one_observation("base refresh");
    }

    /* A failed observation keeps the last generation and the retry */
    dev_fail[1] = 1;
    dirty[1] = 0;
    {
        struct mu_snapshot before = pb.buf[1][pb.cur[1]];
        clear_calls();
        r = run_batch(0);
        CHECK(dirty[1] == 1, "a failed observation must keep the track dirty for a retry");
        CHECK(pb.buf[1][pb.cur[1]].gen == before.gen && published(1),
              "a failed observation replaced the last complete generation");
        CHECK((r & 0x2u) == 0, "a failed observation must not rearm the track");
        CHECK(mu_store_probe_saves == 0, "a failed observation wrote the record %u times",
              mu_store_probe_saves);
        check_one_observation("failed observation");
        dev_fail[1] = 0;
        clear_calls();
        r = run_batch(0);
        CHECK(dirty[1] == 0 && (r & 0x2u) == 0x2u, "the retry did not render and rearm T2");
        CHECK(published(1), "the retry left no complete generation");
        CHECK(mu_store_probe_saves == 0, "the retry wrote the record %u times",
              mu_store_probe_saves);
    }

    /* An edit and YES in one batch: one serialization of the final state */
    fresh();
    select_all(MU_EUCL, MU_TURI, MU_TB3P, MU_EUCL);
    for (t = 0; t < TRACKS; t++)
        dirty[t] = 1;
    run_batch(0); /* settle: the runtime and the record agree before the batch under test */
    CHECK(record_matches_runtime(), "the settled record is not the runtime state");
    {
        struct mu_track after = rt.t[2].recipe;
        after.p[MU_TB3P][1] = 3; /* LEN */
        mu_rt_edit(&rt.t[2], &after);
        dirty[2] = 1;
        CHECK(mu_tick_dirty(&tick, 2) == 1, "a pending render must block the commit");
        clear_calls();
        r = mu_tick_run(&tick, &clk, 0); /* the edit is rendered in this batch ... */
        CHECK(mu_tick_dirty(&tick, 2) == 0, "the pending render was not resolved before YES");
        CHECK(published(2), "the render before YES left no complete generation");
        CHECK((r & 0x4u) == 0x4u, "the edited track was not rearmed");
        mu_rt_committed(&rt.t[2]); /* ... and YES, in the same batch, switches to native */
        dirty[2] = 1;
        CHECK(mu_tick_flush(&tick) == 1, "the batch's final state was not written");
        CHECK(mu_store_probe_saves == 1, "the batch wrote the record %u times, expected 1",
              mu_store_probe_saves);
        CHECK(load_record(), "the saved record is invalid");
        CHECK(loaded_record.t[2].play == MU_PLAY_NATIVE, "the record kept the old playback source");
        CHECK(loaded_record.t[2].recipe.p[MU_TB3P][1] == 3, "the record kept the old recipe");
        CHECK(record_matches_runtime(), "the batch's record is not the runtime state");
        CHECK(mu_tick_flush(&tick) == 0 && mu_store_probe_saves == 1,
              "a second flush wrote again (%u)", mu_store_probe_saves);
        clear_calls();
        r = run_batch(0);
        CHECK((r & 0x4u) == 0x4u && !pb.on[2], "the committed track did not switch output off");
        CHECK(mu_store_probe_saves == 0, "switching output off wrote the record %u times",
              mu_store_probe_saves);
    }

    /* NO: cancel and undo are stored, not silently dropped */
    {
        CHECK(mu_rt_cancel(&rt.t[0]) == 1, "the preview did not cancel");
        dirty[0] = 1;
        clear_calls();
        CHECK(mu_tick_flush(&tick) == 1, "the cancel was not written");
        CHECK(mu_store_probe_saves == 1 && load_record() &&
                  loaded_record.t[0].play == MU_PLAY_NATIVE,
              "the cancel left the record stale");
        CHECK(record_matches_runtime(), "the cancel's record is not the runtime state");
        clear_calls();
        run_batch(0);
        CHECK(!pb.on[0], "the canceled track kept generated output playing");
        CHECK(mu_store_probe_saves == 0, "the canceled track's render wrote the record %u times",
              mu_store_probe_saves);
    }

    /* TURI evolution changes transient source, not the project record */
    fresh();
    mu_rt_select_model(&rt.t[1], MU_TURI);
    rt.t[1].recipe.p[MU_TURI][6] = 0; /* LOCK 0: every pass changes the source */
    dirty[1] = 1;
    clk.playing = 1;
    for (t = 0; t < TRACKS; t++) {
        clk.last[t] = 0;
        clk.parity[t] = 0;
    }
    clear_calls();
    run_batch(0); /* establish the clock baseline and publish */
    CHECK(passes[1] == 0, "the baseline tick must not evolve");
    {
        uint32_t gen = pb.gen[1];
        clk.last[1] = 0;
        clk.parity[1] = 1; /* a wrap: the loop boundary passes */
        clear_calls();
        r = run_batch(0);
        CHECK(passes[1] == 1, "TURI performed %u passes at the boundary, expected 1", passes[1]);
        CHECK(dirty[1] == 0 && published(1), "the evolved track was not republished");
        CHECK(pb.gen[1] > gen, "the evolved track kept its generation");
        CHECK(mu_store_probe_saves == 0, "evolution wrote the record %u times, expected 0",
              mu_store_probe_saves);
        CHECK(record_matches_runtime(), "the evolved record is not the runtime state");
        CHECK((r & 0x2u) == 0x2u, "the evolved track was not rearmed");
    }

    /* Switching a track OFF, or looking at another track, drops its observation */
    fresh();
    mu_rt_select_model(&rt.t[0], MU_EUCL);
    for (t = 0; t < TRACKS; t++)
        dirty[t] = 1;
    run_batch(0);
    CHECK(mu_tick_observed(&tick, 0) && mu_tick_track_len(&tick, 0) == 16,
          "T1 was not observed while it generated");
    mu_rt_select_model(&rt.t[0], MU_OFF);
    dirty[0] = 1;
    clear_calls();
    run_batch(1);
    CHECK(dev_calls[0] == 0, "the OFF track was observed %u times after leaving the page",
          dev_calls[0]);
    CHECK(!mu_tick_observed(&tick, 0), "an OFF track still reports an observation");
    CHECK(mu_tick_track_len(&tick, 0) == -1, "an OFF track still reports its old length");
    CHECK(dev_calls[1] == 1, "the page's own track was observed %u times", dev_calls[1]);

    /* The page's own track is observed even with MODEL OFF */
    fresh();
    mu_rt_select_model(&rt.t[2], MU_EUCL);
    for (t = 0; t < TRACKS; t++)
        dirty[t] = 1;
    clear_calls();
    run_batch(3);
    CHECK(dev_calls[3] == 1, "the page's OFF track was observed %u times", dev_calls[3]);
    CHECK(dev_calls[2] == 1, "the generating track was observed %u times", dev_calls[2]);
    CHECK(dev_calls[0] == 0 && dev_calls[1] == 0, "an unrelated OFF track was observed (%u, %u)",
          dev_calls[0], dev_calls[1]);
    CHECK(mu_tick_observed(&tick, 3) && mu_tick_track_len(&tick, 3) == 16,
          "the page's own observation is missing");
    CHECK(mu_tick_track_len(&tick, 0) == -1, "an unobserved track reported a length");

    /* Without a kit there is no base and no observation of it */
    fresh();
    select_all(MU_EUCL, MU_EUCL, MU_EUCL, MU_EUCL);
    dev_kit = 0;
    clear_calls();
    run_batch(0);
    for (t = 0; t < TRACKS; t++)
        CHECK(published(t), "T%u lost its generation when the kit disappeared", t + 1);
    dev_kit = 1;

    /* The page picture is prepared by the tick, reused across animation */
    {
        const struct mu_key *observed;
        uint32_t acid, builds, renders;
        struct mu_track after;
        fresh();
        mu_rt_select_model(&rt.t[2], MU_TB3P);
        for (t = 0; t < TRACKS; t++)
            dirty[t] = 1;
        mu_engine_acid_generations = 0;
        mu_engine_previews = 0;
        mu_engine_renders = 0;
        run_batch_hidden(2);
        observed = mu_tick_key(&tick, 2);
        CHECK(observed != NULL, "the page's TB3P key was not observed");
        CHECK(mu_engine_previews == 0, "a hidden tick built %u previews", mu_engine_previews);
        CHECK(mu_tick_preview(&tick, 2, &rt.t[2].recipe, observed) == NULL,
              "a hidden tick prepared a page picture");
        run_batch(2);
        observed = mu_tick_key(&tick, 2);
        CHECK(observed != NULL, "the page's TB3P key was not observed");
        CHECK(mu_tick_preview(&tick, 2, &rt.t[2].recipe, observed) != NULL,
              "the page's TB3P preview was not prepared");
        CHECK(mu_engine_previews == 1, "the initial preparation built %u previews",
              mu_engine_previews);
        CHECK(mu_engine_acid_generations >= 1, "the initial preparation made no TB3P pattern");
        acid = mu_engine_acid_generations;
        builds = mu_engine_previews;
        renders = mu_engine_renders;
        clear_calls();
        run_batch(2); /* an animation-only tick: no recipe, key or track change */
        CHECK(mu_engine_acid_generations == acid, "an animation tick made %u TB3P generations",
              mu_engine_acid_generations - acid);
        CHECK(mu_engine_previews == builds, "an animation tick rebuilt %u previews",
              mu_engine_previews - builds);
        CHECK(mu_engine_renders == renders, "an animation tick made %u recipe renders",
              mu_engine_renders - renders);
        CHECK(dev_calls[2] == 1, "the animation tick observed the page track %u times",
              dev_calls[2]);

        /* A genuine edit rebuilds exactly once before the next applicable draw. */
        after = rt.t[2].recipe;
        after.p[MU_TB3P][0] = (uint8_t)(after.p[MU_TB3P][0] == 8 ? 16 : 8);
        mu_rt_edit(&rt.t[2], &after);
        dirty[2] = 1;
        run_batch(2);
        CHECK(mu_engine_previews == builds + 1, "the edit built %u previews",
              mu_engine_previews - builds);
        acid = mu_engine_acid_generations;
        builds = mu_engine_previews;
        run_batch(2);
        CHECK(mu_engine_previews == builds && mu_engine_acid_generations == acid,
              "an unchanged redraw rebuilt the edited picture");

        /* A committed native track shows its retained recipe with this tick's key, not the key of
         * the last generated render. */
        mu_rt_committed(&rt.t[2]);
        dev_info[2].scale = 5;
        acid = mu_engine_acid_generations;
        run_batch(2);
        CHECK(mu_engine_acid_generations == acid + 1,
              "the native key change made %u TB3P generations", mu_engine_acid_generations - acid);
        observed = mu_tick_key(&tick, 2);
        CHECK(observed != NULL && observed->scale == 5, "the native track kept a stale key");
        CHECK(mu_tick_preview(&tick, 2, &rt.t[2].recipe, observed) != NULL,
              "the committed track lost its retained preview");
        {
            struct mu_preview reference;
            const struct mu_preview *prepared;
            mu_preview(&rt.t[2].recipe, observed, &reference);
            prepared = mu_tick_preview(&tick, 2, &rt.t[2].recipe, observed);
            CHECK(prepared && preview_equal(prepared, &reference),
                  "the native track's preview is not its retained recipe");
        }
    }

    /* Native A->B->A: the prepared key is validated, not seen[] */
    {
        const struct mu_key *observed;
        uint32_t builds;
        fresh();
        mu_rt_select_model(&rt.t[2], MU_TB3P);
        for (t = 0; t < TRACKS; t++)
            dirty[t] = 1;
        run_batch(2); /* generated: seen[] is key A (scale 0) */
        mu_rt_committed(&rt.t[2]);
        dev_info[2].scale = 5; /* B */
        builds = mu_engine_previews;
        run_batch(2);
        CHECK(mu_engine_previews == builds + 1, "native A->B built %u previews",
              mu_engine_previews - builds);
        dev_info[2].scale = 0; /* back to A, which equals seen[] */
        builds = mu_engine_previews;
        run_batch(2);
        CHECK(mu_engine_previews == builds + 1, "native B->A built %u previews",
              mu_engine_previews - builds);
        observed = mu_tick_key(&tick, 2);
        CHECK(observed != NULL && observed->scale == 0, "native B->A kept a stale key");
        CHECK(mu_tick_current_preview(&tick, 2, &rt.t[2].recipe) != NULL,
              "native B->A left the prepared picture stale");
    }

    /* Loaded native with no generated key: every observed key change prepares */
    {
        const struct mu_key *observed;
        uint32_t builds;
        fresh();
        mu_rt_select_model(&rt.t[2], MU_TB3P);
        mu_rt_committed(&rt.t[2]); /* a retained native recipe, as loaded */
        seen_ok[2] = 0;            /* no generated render ever supplied seen[] */
        for (t = 0; t < TRACKS; t++)
            dirty[t] = 1;
        run_batch(2);
        builds = mu_engine_previews;
        dev_info[2].scale = 5;
        run_batch(2);
        CHECK(mu_engine_previews == builds + 1, "loaded native key change built %u previews",
              mu_engine_previews - builds);
        observed = mu_tick_key(&tick, 2);
        CHECK(observed != NULL && observed->scale == 5, "the loaded native key was not observed");
        CHECK(mu_tick_current_preview(&tick, 2, &rt.t[2].recipe) != NULL,
              "the loaded native key change left the prepared picture stale");
        CHECK(seen_ok[2] == 0, "a native observation was labelled as a generated key");
    }

    /* An unknown observation keeps the last picture but never claims it is current */
    {
        struct mu_key saved;
        const struct mu_key *observed;
        uint32_t builds;
        fresh();
        mu_rt_select_model(&rt.t[2], MU_TB3P);
        for (t = 0; t < TRACKS; t++)
            dirty[t] = 1;
        run_batch(2);
        observed = mu_tick_key(&tick, 2);
        CHECK(observed != NULL, "the initial key was not observed");
        saved = *observed;
        builds = mu_engine_previews;
        dev_fail[2] = 1;
        run_batch(2); /* the observation fails: no fresh key, no new picture */
        CHECK(mu_tick_key(&tick, 2) == NULL, "a failed observation produced a key");
        CHECK(mu_tick_current_preview(&tick, 2, &rt.t[2].recipe) == NULL,
              "a fallback key was accepted as a fresh observation");
        CHECK(mu_engine_previews == builds, "a failed observation built %u previews",
              mu_engine_previews - builds);
        CHECK(mu_tick_preview(&tick, 2, &rt.t[2].recipe, &saved) != NULL,
              "a failed observation discarded the last valid picture");
        dev_fail[2] = 0;
        dev_info[2].scale = 5; /* the key changed while unobserved */
        run_batch(2);
        observed = mu_tick_key(&tick, 2);
        CHECK(observed != NULL && observed->scale == 5, "the recovered key was not observed");
        CHECK(mu_tick_current_preview(&tick, 2, &rt.t[2].recipe) != NULL,
              "the recovered key did not prepare a current picture");
    }

    /* TURI evolution rebuilds the page picture without a knob change */
    {
        const struct mu_key *observed;
        uint32_t builds;
        fresh();
        mu_rt_select_model(&rt.t[1], MU_TURI);
        rt.t[1].recipe.p[MU_TURI][6] = 0; /* LOCK 0 */
        dirty[1] = 1;
        clk.playing = 1;
        for (t = 0; t < TRACKS; t++) {
            clk.last[t] = 0;
            clk.parity[t] = 0;
        }
        run_batch(1); /* baseline: prepare and publish */
        builds = mu_engine_previews;
        clk.parity[1] = 1; /* the loop boundary passes */
        run_batch(1);
        CHECK(passes[1] == 1, "the boundary did not evolve TURI");
        CHECK(mu_engine_previews == builds + 1, "evolution built %u previews",
              mu_engine_previews - builds);
        observed = mu_tick_key(&tick, 1);
        CHECK(observed && mu_tick_preview(&tick, 1, &rt.t[1].recipe, observed) != NULL,
              "the evolved preview is not prepared");
    }

    /* A track change or lifecycle reset invalidates the page picture */
    {
        const struct mu_key *observed;
        struct mu_key saved;
        uint32_t builds;
        fresh();
        select_all(MU_EUCL, MU_TURI, MU_TB3P, MU_EUCL);
        for (t = 0; t < TRACKS; t++)
            dirty[t] = 1;
        run_batch(0);
        builds = mu_engine_previews;
        run_batch(2); /* the page moved to another track */
        CHECK(mu_engine_previews == builds + 1, "a track change built %u previews",
              mu_engine_previews - builds);
        builds = mu_engine_previews;
        run_batch(2);
        CHECK(mu_engine_previews == builds, "the new track rebuilt an unchanged picture");
        observed = mu_tick_key(&tick, 2);
        CHECK(observed != NULL, "the new page track was not observed");
        saved = *observed;
        mu_tick_reset(&tick);
        CHECK(mu_tick_preview(&tick, 2, &rt.t[2].recipe, &saved) == NULL,
              "a lifecycle reset kept the page picture");
    }

    /* The shared phase after commands prepares a recipe they restored/changed */
    {
        struct mu_track after;
        fresh();
        mu_rt_select_model(&rt.t[0], MU_EUCL);
        for (t = 0; t < TRACKS; t++)
            dirty[t] = 1;
        run_batch(0);
        after = rt.t[0].recipe;
        after.p[MU_EUCL][1] = (uint8_t)(after.p[MU_EUCL][1] == 4 ? 6 : 4);
        mu_rt_edit(&rt.t[0], &after); /* a command after the tick, as YES/NO resolves */
        dirty[0] = 1;                 /* the production command path marks the track for output */
        CHECK(mu_tick_prepare(&tick, 0, 1) == 1, "a post-tick recipe change was not prepared");
        CHECK(mu_tick_prepare(&tick, 0, 1) == 0, "the post-command preparation rebuilt twice");
        CHECK(mu_tick_prepare(&tick, 0, 0) == 0, "a hidden page prepared a picture");
    }

    /* TB3P derived-base cache: production publish and preview share one generation */
#ifdef MU_ENGINE_PROBE
    {
        struct mu_track settled;
        uint32_t acid, builds, renders, i, accent_steps = 0;
        int16_t want_freq, want_vol;

        fresh();
        mu_rt_select_model(&rt.t[2], MU_TB3P);
        for (t = 0; t < TRACKS; t++)
            dirty[t] = 1;
        clear_calls();
        mu_engine_acid_generations = 0;
        mu_engine_previews = 0;
        mu_engine_renders = 0;
        run_batch(2); /* cold: publish and preview share the first base generation */
        CHECK(mu_engine_acid_generations == 1,
              "the cold TB3P batch made %u base generations, expected 1",
              mu_engine_acid_generations);
        CHECK(published(2), "the cold TB3P batch published nothing");
        settled = rt.t[2].recipe;

        /* A warm FREQ/VOL sweep changes only the accent lock values: no base generation, no
         * recipe/RNG change and no project serialization. */
        acid = mu_engine_acid_generations;
        builds = mu_engine_previews;
        renders = mu_engine_renders;
        for (i = 0; i < 100; i++) {
            uint32_t gen = pb.gen[2];
            dev_freq[2] = (int16_t)((i + 1u) & 0x7f);
            dev_vol[2] = (int16_t)((i * 3u + 1u) & 0x7f);
            clear_calls();
            run_batch(2);
            CHECK(memcmp(&rt.t[2].recipe, &settled, sizeof settled) == 0,
                  "FREQ/VOL edit %u changed the stored recipe", i);
            CHECK(mu_store_probe_saves == 0, "FREQ/VOL edit %u wrote the record %u times", i,
                  mu_store_probe_saves);
            CHECK(pb.gen[2] > gen, "FREQ/VOL edit %u did not republish the lock metadata", i);
            CHECK(published(2), "FREQ/VOL edit %u left no complete generation", i);
        }
        CHECK(mu_engine_acid_generations == acid, "100 FREQ/VOL edits made %u base generations",
              mu_engine_acid_generations - acid);
        CHECK(mu_engine_previews == builds, "100 FREQ/VOL edits rebuilt %u previews",
              mu_engine_previews - builds);
        CHECK(mu_engine_renders == renders + 100, "100 FREQ/VOL edits made %u renders",
              mu_engine_renders - renders);
        {
            const struct mu_snapshot *s = &pb.buf[2][pb.cur[2]];
            for (i = 0; i < MU_STEPS; i++) {
                const struct mu_voice *v = &s->step[i];
                uint32_t k;
                for (k = 0; k < v->nlock; k++) {
                    if (v->slot[k] == 55) { /* FLTR FREQ */
                        want_freq = (int16_t)(dev_freq[2] + 60 * 0x1800 / 100);
                        if (want_freq > 0x7f00)
                            want_freq = 0x7f00;
                        CHECK(v->value[k] == want_freq,
                              "step %u FREQ lock %d != %d after the sweep", i, v->value[k],
                              want_freq);
                        accent_steps++;
                    }
                    if (v->slot[k] == 72) { /* AMP VOL */
                        want_vol = (int16_t)(dev_vol[2] + 60 * 0x1000 / 100);
                        if (want_vol > 0x7f00)
                            want_vol = 0x7f00;
                        CHECK(v->value[k] == want_vol, "step %u VOL lock %d != %d after the sweep",
                              i, v->value[k], want_vol);
                    }
                }
            }
        }
        CHECK(accent_steps > 0, "the warm sweep found no accent locks to update");

        /* Mapping-only edits: no base generation, but the published steps still match the pure
         * render for every field. */
        {
            static const uint8_t knob[4] = {0, 3, 4, 5}; /* LEN, SLID, ACNT, TRNS */
            uint32_t j;
            for (j = 0; j < 4; j++) {
                struct mu_track after = rt.t[2].recipe;
                uint8_t old = after.p[MU_TB3P][knob[j]];
                uint8_t max = (uint8_t)mu_knob_max(MU_TB3P, (uint8_t)(knob[j] + 1));
                after.p[MU_TB3P][knob[j]] = (uint8_t)(old < max ? old + 1 : old - 1);
                mu_rt_edit(&rt.t[2], &after);
                dirty[2] = 1;
                acid = mu_engine_acid_generations;
                builds = mu_engine_previews;
                run_batch(2);
                CHECK(mu_engine_acid_generations == acid,
                      "mapping-only knob %u made %u base generations", knob[j] + 1,
                      mu_engine_acid_generations - acid);
                CHECK(mu_engine_previews == builds + 1, "mapping-only knob %u built %u previews",
                      knob[j] + 1, mu_engine_previews - builds);
                CHECK(published_matches_render(2),
                      "mapping-only knob %u published steps that differ from the pure engine",
                      knob[j] + 1);
            }
        }

        /* Base dependencies: each change generates exactly one base before use. */
        {
            struct {
                uint8_t kind; /* 0: seed, 1: GATE, 2: PTCH, 3: scale size */
                uint8_t value;
            } cases[4] = {{0, 1}, {1, 3}, {2, 5}, {3, 0}};
            uint32_t j;
            for (j = 0; j < 4; j++) {
                struct mu_track after = rt.t[2].recipe;
                acid = mu_engine_acid_generations;
                builds = mu_engine_previews;
                if (cases[j].kind == 0) {
                    mu_acid_set_seed(&after, (uint32_t)(mu_acid_seed(&after) + cases[j].value));
                } else if (cases[j].kind == 3) {
                    dev_info[2].scale =
                        0; /* 12-note chromatic -> 7-note scale: base size changes */
                } else {
                    after.p[MU_TB3P][cases[j].kind] = cases[j].value;
                }
                mu_rt_edit(&rt.t[2], &after);
                dirty[2] = 1;
                run_batch(2);
                CHECK(mu_engine_acid_generations == acid + 1,
                      "base dependency %u made %u generations, expected 1", j,
                      mu_engine_acid_generations - acid);
                CHECK(mu_engine_previews == builds + 1, "base dependency %u built %u previews", j,
                      mu_engine_previews - builds);
            }
        }

        /* The seed extremes and repeated rerolls through the production batch. */
        {
            static const uint16_t seeds[2] = {0, 0xffff};
            uint32_t j;
            for (j = 0; j < 2; j++) {
                struct mu_track after = rt.t[2].recipe;
                mu_acid_set_seed(&after, seeds[j]);
                mu_rt_edit(&rt.t[2], &after);
                dirty[2] = 1;
                acid = mu_engine_acid_generations;
                run_batch(2);
                CHECK(mu_engine_acid_generations == acid + 1, "seed %u made %u base generations",
                      seeds[j], mu_engine_acid_generations - acid);
                CHECK(published_matches_render(2), "seed %u published a different render",
                      seeds[j]);
            }
            for (j = 0; j < 10; j++) {
                struct mu_track after = rt.t[2].recipe;
                mu_reroll(&after);
                mu_rt_edit(&rt.t[2], &after);
                dirty[2] = 1;
                acid = mu_engine_acid_generations;
                run_batch(2);
                CHECK(mu_engine_acid_generations == acid + 1, "reroll %u made %u base generations",
                      j, mu_engine_acid_generations - acid);
                CHECK(published_matches_render(2), "reroll %u published a different render", j);
            }
        }

        /* Model switch away and back: the base survives unchanged inputs, and the returned
         * generation is still the pure render. */
        {
            uint32_t before;
            mu_rt_select_model(&rt.t[2], MU_EUCL);
            dirty[2] = 1;
            acid = mu_engine_acid_generations;
            run_batch(2);
            CHECK(mu_engine_acid_generations == acid, "EUCL touched the TB3P base");
            CHECK(published_matches_render(2), "EUCL published a different render");
            before = mu_engine_acid_generations;
            mu_rt_select_model(&rt.t[2], MU_TB3P);
            dirty[2] = 1;
            run_batch(2);
            CHECK(mu_engine_acid_generations == before,
                  "returning to TB3P with unchanged inputs made %u base generations",
                  mu_engine_acid_generations - before);
            CHECK(published_matches_render(2), "the returned TB3P render differs");
        }

        /* A lifecycle/session boundary invalidates deliberately: the next use generates once. */
        {
            mu_tick_reset(&tick);
            dirty[2] = 1;
            acid = mu_engine_acid_generations;
            run_batch(2);
            CHECK(mu_engine_acid_generations == acid + 1,
                  "a session reset made %u base generations", mu_engine_acid_generations - acid);
            CHECK(published_matches_render(2), "the post-reset render differs");
        }

        /* Project reload invalidates the cache and preserves the retained TB3P recipe. */
        {
            struct mu_store retained;
            mu_store_save(&retained, &rt);
            mu_store_load(&retained, &rt);
            mu_tick_reset(&tick);
            for (t = 0; t < TRACKS; t++)
                dirty[t] = 1;
            acid = mu_engine_acid_generations;
            run_batch(2);
            CHECK(rt.t[2].recipe.model == MU_TB3P, "reload lost the TB3P recipe");
            CHECK(mu_engine_acid_generations == acid + 1, "reload made %u base generations",
                  mu_engine_acid_generations - acid);
            CHECK(published_matches_render(2), "the reloaded render differs from the pure engine");
        }
    }
#endif

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("tick tests passed\n");
    return 0;
}
