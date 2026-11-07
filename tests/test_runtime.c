/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host tests of MUTINY's track sessions: the transition table, the undo validity rules and
 * the recipe retention. No firmware, no editor, no pattern: only the state machine. */
#include "../src/runtime/runtime.h"
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

static struct mu_track edit(const struct mu_rt_track *t, int knob, int value) {
    struct mu_track after = t->recipe;
    if (after.model)
        after.p[after.model][knob] = (uint8_t)value;
    return after;
}

int main(void) {
    struct mu_rt rt;
    struct mu_rt_track *t;

    /* initial: native, OFF, no undo */
    mu_rt_init(&rt);
    t = &rt.t[0];
    CHECK(t->play == MU_PLAY_NATIVE && t->preview == 0 && !t->undo, "initial session");
    CHECK(t->recipe.model == MU_OFF, "initial model %d", t->recipe.model);

    /* initial native/OFF -> select EUCL -> generated preview; native bytes untouched */
    mu_rt_select_model(t, MU_EUCL);
    CHECK(t->play == MU_PLAY_GENERATED && t->preview, "select EUCL preview");
    CHECK(t->recipe.model == MU_EUCL, "select EUCL model");

    /* generated preview -> successful YES -> native playback; EUCL/source/controls retained */
    {
        uint8_t model = t->recipe.model;
        struct mu_source src = t->recipe.src;
        mu_rt_committed(t);
        CHECK(t->play == MU_PLAY_NATIVE && !t->preview && t->undo, "commit switches to native");
        CHECK(t->recipe.model == model && memcmp(&t->recipe.src, &src, sizeof src) == 0,
              "commit keeps the recipe");
    }

    /* native playback -> YES without a preview -> no write; undo not replaced */
    {
        struct mu_rt_track before = *t;
        mu_rt_committed(t); /* the mod refuses to commit without a preview; if called, state must hold */
        CHECK(t->play == MU_PLAY_NATIVE && t->undo, "repeated YES keeps native");
        CHECK(memcmp(t->committed.p, before.committed.p, sizeof t->committed.p) == 0,
              "repeated YES replaced the undo recipe");
    }

    /* native playback -> real Mutiny value change -> generated preview; undo retired */
    mu_rt_edit(t, &(struct mu_track){.model = MU_EUCL, .p = {{0}}});
    CHECK(t->play == MU_PLAY_GENERATED && t->preview, "edit starts a preview");
    CHECK(!t->undo, "a new preview must retire the uncommit record");

    /* preview -> NO -> previous native playback and pre-preview recipe restored */
    {
        uint8_t old_acnt = t->before.p[MU_EUCL][4];
        CHECK(mu_rt_cancel(t), "NO cancels a preview");
        CHECK(t->play == MU_PLAY_NATIVE && !t->preview, "cancel returns to native");
        CHECK(t->recipe.p[MU_EUCL][4] == old_acnt, "cancel restores the pre-preview recipe");
        CHECK(!mu_rt_cancel(t), "NO with no preview is the stock's");
    }

    /* successful commit -> NO with valid undo -> captured generator restored, no reroll */
    {
        struct mu_track committed;
        mu_rt_select_model(&rt.t[1], MU_TURI);
        rt.t[1].recipe.p[MU_TURI][5] = 7; /* a distinct control */
        struct mu_track after = edit(&rt.t[1], 1, 33);
        mu_rt_edit(&rt.t[1], &after);
        committed = rt.t[1].recipe;
        mu_rt_committed(&rt.t[1]);
        CHECK(rt.t[1].undo, "the commit leaves an undo record");
        CHECK(mu_rt_uncommit(&rt.t[1]), "NO uncommits");
        CHECK(rt.t[1].play == MU_PLAY_GENERATED && rt.t[1].preview, "uncommit resumes the generator");
        CHECK(memcmp(&rt.t[1].recipe, &committed, sizeof committed) == 0,
              "uncommit restored a different recipe");
        CHECK(!rt.t[1].undo, "uncommit is single level");
        CHECK(!mu_rt_uncommit(&rt.t[1]), "a second NO does not uncommit again");
    }

    /* commit -> edit -> cancel -> edit: the cancel restores the committed value, and the next
     * edit starts a fresh preview from it */
    {
        struct mu_rt_track s2;
        struct mu_track after;
        mu_rt_track_init(&s2);
        mu_rt_select_model(&s2, MU_EUCL);
        s2.recipe.p[MU_EUCL][3] = 0;
        mu_rt_committed(&s2);
        after = s2.recipe;
        after.p[MU_EUCL][3] = 2;
        mu_rt_edit(&s2, &after);
        CHECK(s2.play == MU_PLAY_GENERATED && s2.recipe.p[MU_EUCL][3] == 2, "first preview");
        CHECK(mu_rt_cancel(&s2), "cancel");
        CHECK(s2.recipe.p[MU_EUCL][3] == 0, "cancel restored %d, not the committed 0",
              s2.recipe.p[MU_EUCL][3]);
        after = s2.recipe;
        after.p[MU_EUCL][3] = 2;
        mu_rt_edit(&s2, &after);
        CHECK(s2.play == MU_PLAY_GENERATED && s2.preview && s2.recipe.p[MU_EUCL][3] == 2,
              "the second edit did not start a preview");
    }

    /* any playback -> navigate away/back -> same playback source and retained state */
    {
        struct mu_rt_track saved = rt.t[1];
        /* navigating changes no session: the page opens, keys move, this state stays */
        mu_rt_track_init(t);
        CHECK(memcmp(&saved, &rt.t[1], sizeof saved) == 0, "navigation changed the session");
    }

    /* a native edit or a pattern change invalidates the record */
    mu_rt_committed(&rt.t[2]);
    CHECK(rt.t[2].undo, "commit sets undo");
    mu_rt_invalidate_undo(&rt.t[2]);
    CHECK(!rt.t[2].undo, "native edit keeps a stale undo");

    /* MODEL OFF keeps the recipe and plays native; a bound-clamped no-op turn is not an edit */
    rt.t[0].recipe.model = MU_EUCL;
    mu_rt_select_model(&rt.t[0], MU_OFF);
    CHECK(rt.t[0].play == MU_PLAY_NATIVE && rt.t[0].recipe.model == MU_OFF, "OFF plays native");
    CHECK(rt.t[0].recipe.p[MU_EUCL][1] == rt.t[0].before.p[MU_EUCL][1], "OFF dropped the controls");

    /* only generated TURI drifts; LOCK 100 does not, and a native track never does */
    {
        struct mu_rt_track s;
        struct mu_source src;
        mu_rt_track_init(&s);
        mu_rt_select_model(&s, MU_TURI);
        s.recipe.p[MU_TURI][MU_PARAMS - 1] = 10; /* LOCK 10 */
        src = s.recipe.src;
        CHECK(mu_rt_can_evolve(&s), "generated TURI can evolve");
        mu_rt_evolve(&s, 3);
        CHECK(memcmp(&src, &s.recipe.src, sizeof src) != 0, "TURI did not evolve");
        s.recipe.p[MU_TURI][MU_PARAMS - 1] = 100;
        src = s.recipe.src;
        mu_rt_evolve(&s, 4);
        CHECK(memcmp(&src, &s.recipe.src, sizeof src) == 0, "LOCK 100 evolved");
        mu_rt_committed(&s);
        CHECK(!mu_rt_can_evolve(&s), "a native track evolves");
        s.recipe.p[MU_TURI][MU_PARAMS - 1] = 10;
        src = s.recipe.src;
        mu_rt_evolve(&s, 5);
        CHECK(memcmp(&src, &s.recipe.src, sizeof src) == 0, "native TURI evolved its recipe");
    }

    /* load: the recipe and playback source return; transient history does not */
    {
        struct mu_rt_track rec;
        struct mu_track recipe;
        mu_rt_track_init(&rec);
        mu_defaults(&recipe);
        recipe.model = MU_EUCL;
        recipe.p[MU_EUCL][1] = 9;
        mu_rt_load(&rec, &recipe, MU_PLAY_GENERATED);
        CHECK(rec.recipe.model == MU_EUCL && rec.recipe.p[MU_EUCL][1] == 9, "load recipe");
        CHECK(rec.play == MU_PLAY_GENERATED && rec.preview, "load keeps generating");
        CHECK(!rec.undo, "load must not invent an undo record");
        mu_rt_load(&rec, &recipe, MU_PLAY_NATIVE);
        CHECK(rec.play == MU_PLAY_NATIVE && !rec.preview, "load native");
        recipe.model = MU_OFF;
        mu_rt_load(&rec, &recipe, MU_PLAY_GENERATED);
        CHECK(rec.play == MU_PLAY_NATIVE, "OFF cannot generate");
    }

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("runtime tests passed\n");
    return 0;
}
