/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host tests of MUTINY's input contract: stock precedence first, one owner per gesture, and no
 * fractional encoder counts leaking across a track or model change. */
#include "../mods/mutiny/input.h"
#include <stdio.h>

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

static struct mu_input in;
static uint8_t track;

static uint32_t key(uint32_t k, uint32_t flags, int open, uint8_t generated, uint8_t synth) {
    return mu_in_key(&in, k, flags, (uint8_t)open, &track, synth, generated);
}

int main(void) {
    uint32_t a;

    /* entry: one ordinary TRIG press opens; the next press hands the page back to the stock */
    mu_in_init(&in);
    track = 2;
    a = key(MU_KEY_TRIGPAGE, MU_FLAG_PRESS, 0, 1, 1);
    CHECK((a & MU_ACT_OPEN) && (a & MU_ACT_TAKE), "TRIG from the stock page did not open");
    a = key(MU_KEY_TRIGPAGE, MU_FLAG_PRESS, 1, 1, 1);
    CHECK((a & MU_ACT_CLOSE_PASS) && !(a & MU_ACT_TAKE), "TRIG on the page did not hand it back");

    /* a MIDI context never opens a synth page */
    a = key(MU_KEY_TRIGPAGE, MU_FLAG_PRESS, 0, 1, 0);
    CHECK(a == MU_ACT_PASS, "a MIDI track opened the page");

    /* FUNC+NO is the stock's temporary reload, not an uncommit; plain NO is the page's */
    mu_in_init(&in);
    a = key(MU_KEY_NO, MU_FLAG_PRESS | MU_FLAG_FUNC, 1, 1, 1);
    CHECK(a == MU_ACT_PASS, "FUNC+NO was taken");
    a = key(MU_KEY_NO, MU_FLAG_RELEASE, 1, 1, 1);
    CHECK(a == MU_ACT_PASS, "FUNC+NO's release was taken");
    mu_in_init(&in);
    key(MU_KEY_NO, MU_FLAG_PRESS, 1, 1, 1);
    a = key(MU_KEY_NO, MU_FLAG_RELEASE, 1, 1, 1);
    CHECK(a & MU_ACT_CANCEL, "plain NO did not cancel");

    /* FUNC's release after a combination arrives without any flag: it must end the combination */
    mu_in_init(&in);
    key(MU_KEY_FUNC, MU_FLAG_PRESS, 1, 1, 1);
    a = key(MU_KEY_FUNC, 0, 1, 1, 1);
    CHECK(in.func == 0 && a == MU_ACT_PASS, "a zero-flag FUNC release stayed latched");

    /* a NO held while an encoder turns is the stock reset: release acts on nothing */
    mu_in_init(&in);
    key(MU_KEY_NO, MU_FLAG_PRESS, 1, 1, 1);
    mu_in_detents(&in, 0, 4, 4, 0, MU_EUCL); /* the encoder turns while NO is held */
    a = key(MU_KEY_NO, MU_FLAG_RELEASE, 1, 1, 1);
    CHECK(!(a & MU_ACT_CANCEL), "encoder+NO became an uncommit");

    /* an encoder press outside Mutiny stays entirely the stock's: the release is not eaten */
    mu_in_init(&in);
    a = key(MU_KEY_PUSH_A + 1, MU_FLAG_PRESS, 0, 1, 1);
    CHECK(a == MU_ACT_PASS, "a closed-page encoder press was taken");
    a = key(MU_KEY_PUSH_A + 1, MU_FLAG_RELEASE, 0, 1, 1);
    CHECK(a == MU_ACT_PASS, "a closed-page encoder release was taken");
    /* the page's own encoder press keeps its release */
    mu_in_init(&in);
    key(MU_KEY_PUSH_A + 1, MU_FLAG_PRESS, 1, 1, 1);
    a = key(MU_KEY_PUSH_A + 1, MU_FLAG_RELEASE, 1, 1, 1);
    CHECK(a == MU_ACT_TAKE, "the page's own encoder release was not kept");

    /* encoder push + NO resets that knob, with no turn required */
    mu_in_init(&in);
    key(MU_KEY_PUSH_A + 1, MU_FLAG_PRESS, 1, 1, 1);
    CHECK(in.push == 1, "the held encoder was not recorded");
    key(MU_KEY_NO, MU_FLAG_PRESS, 1, 1, 1);
    a = key(MU_KEY_NO, MU_FLAG_RELEASE, 1, 1, 1);
    CHECK((a & MU_ACT_RESET) && !(a & MU_ACT_CANCEL), "push+NO did not request a reset (0x%x)", a);
    key(MU_KEY_PUSH_A + 1, MU_FLAG_RELEASE, 1, 1, 1);
    CHECK(in.push == 0xff, "the held encoder stayed down");

    /* measured on OS 1.44: an encoder push's release arrives with no flags at all, and so does a NO
     * released after a track key. A release is "not a press", or the gesture stays latched. */
    mu_in_init(&in);
    key(MU_KEY_PUSH_A + 1, MU_FLAG_PRESS, 1, 1, 1);
    a = key(MU_KEY_PUSH_A + 1, 0, 1, 1, 1);
    CHECK(a == MU_ACT_TAKE && in.push == 0xff, "a zero-flag encoder release stayed held");
    mu_in_init(&in);
    key(MU_KEY_PUSH_A + 1, MU_FLAG_PRESS, 1, 1, 1);
    key(MU_KEY_NO, MU_FLAG_PRESS, 1, 1, 1);
    key(MU_KEY_PUSH_A + 1, 0, 1, 1, 1);
    a = key(MU_KEY_NO, 0, 1, 1, 1);
    CHECK((a & MU_ACT_RESET) && !(a & MU_ACT_CANCEL), "a zero-flag NO release lost the reset");
    /* a plain NO whose release has no flags is still a cancel, not a kept gesture */
    mu_in_init(&in);
    key(MU_KEY_NO, MU_FLAG_PRESS, 1, 1, 1);
    a = key(MU_KEY_NO, 0, 1, 1, 1);
    CHECK((a & MU_ACT_CANCEL) && in.no_down == 0, "a zero-flag bare NO stayed latched");

    /* The reset belongs to the track it formed on, in every order */
    mu_in_init(&in);
    track = 1;
    key(MU_KEY_PUSH_A + 1, MU_FLAG_PRESS, 1, 1, 1);
    key(MU_KEY_NO, MU_FLAG_PRESS, 1, 1, 1);
    track = 3; /* a track key selects another track while the combination is held */
    a = key(MU_KEY_NO, MU_FLAG_RELEASE, 1, 1, 1);
    CHECK((a & MU_ACT_RESET) && in.reset_track == 1, "the reset followed the later selection: track %u",
          in.reset_track);
    mu_in_init(&in);
    track = 2;
    key(MU_KEY_NO, MU_FLAG_PRESS, 1, 1, 1);
    key(MU_KEY_PUSH_A + 2, MU_FLAG_PRESS, 1, 1, 1);
    track = 0;
    a = key(MU_KEY_NO, MU_FLAG_RELEASE, 1, 1, 1);
    CHECK((a & MU_ACT_RESET) && in.reset_track == 2, "NO-first reset followed the later selection: %u",
          in.reset_track);
    /* a release re-arms the latch: the next plain NO press starts from no track */
    mu_in_init(&in);
    track = 1;
    key(MU_KEY_NO, MU_FLAG_PRESS, 1, 1, 1);
    key(MU_KEY_PUSH_A + 1, MU_FLAG_PRESS, 1, 1, 1);
    key(MU_KEY_NO, MU_FLAG_RELEASE, 1, 1, 1);
    key(MU_KEY_PUSH_A + 1, MU_FLAG_RELEASE, 1, 1, 1);
    key(MU_KEY_NO, MU_FLAG_PRESS, 1, 1, 1);
    CHECK(in.reset == 0xff && in.reset_track == 0xff, "a bare NO kept the previous reset track");

    /* the reset is latched when the combination forms: releasing the encoder first still resets */
    mu_in_init(&in);
    track = 1;
    key(MU_KEY_PUSH_A + 1, MU_FLAG_PRESS, 1, 1, 1);
    key(MU_KEY_NO, MU_FLAG_PRESS, 1, 1, 1);
    key(MU_KEY_PUSH_A + 1, MU_FLAG_RELEASE, 1, 1, 1);
    CHECK(in.reset_track == 1, "the encoder release lost the gesture's track");
    a = key(MU_KEY_PUSH_A + 1, MU_FLAG_RELEASE, 1, 1, 1);
    CHECK(!(a & MU_ACT_CANCEL), "the encoder release became a cancel");
    a = key(MU_KEY_NO, MU_FLAG_RELEASE, 1, 1, 1);
    CHECK((a & MU_ACT_RESET) && !(a & MU_ACT_CANCEL),
          "encoder-first release order cancelled the preview");
    /* and NO first: the encoder is still held at NO's release */
    mu_in_init(&in);
    key(MU_KEY_NO, MU_FLAG_PRESS, 1, 1, 1);
    key(MU_KEY_PUSH_A + 1, MU_FLAG_PRESS, 1, 1, 1);
    a = key(MU_KEY_NO, MU_FLAG_RELEASE, 1, 1, 1);
    CHECK((a & MU_ACT_RESET) && !(a & MU_ACT_CANCEL), "NO-first release order lost the reset");
    /* a bare NO is still a cancel */
    mu_in_init(&in);
    key(MU_KEY_NO, MU_FLAG_PRESS, 1, 1, 1);
    a = key(MU_KEY_NO, MU_FLAG_RELEASE, 1, 1, 1);
    CHECK((a & MU_ACT_CANCEL) && !(a & MU_ACT_RESET), "a bare NO stopped cancelling");

    /* a gesture the page owns is released even after the page closed */
    mu_in_init(&in);
    key(MU_KEY_PUSH_H, MU_FLAG_PRESS, 1, 1, 1);
    key(MU_KEY_PAGE_LO, MU_FLAG_PRESS, 1, 1, 1); /* the page closes */
    a = key(MU_KEY_PUSH_H, MU_FLAG_RELEASE, 0, 1, 1);
    CHECK(in.h_down == 0 && !(a & MU_ACT_REROLL), "H stayed down after the page closed");
    mu_in_init(&in);
    key(MU_KEY_TRIG1, MU_FLAG_PRESS, 1, 1, 1);
    key(MU_KEY_PAGE_LO, MU_FLAG_PRESS, 1, 1, 1);
    a = key(MU_KEY_TRIG1, MU_FLAG_RELEASE, 0, 1, 1);
    CHECK(in.trig_down == 0 && a == MU_ACT_PASS, "a trig release leaked after the page closed");

    /* a held trig's YES is the stock preview */
    mu_in_init(&in);
    key(MU_KEY_TRIG1, MU_FLAG_PRESS, 1, 1, 1);
    a = key(MU_KEY_YES, MU_FLAG_PRESS, 1, 1, 1);
    CHECK(!(a & MU_ACT_COMMIT) && (a == MU_ACT_PASS), "held-trig YES became a commit");
    key(MU_KEY_TRIG1, MU_FLAG_RELEASE, 1, 1, 1);
    a = key(MU_KEY_YES, MU_FLAG_PRESS, 1, 1, 1);
    CHECK(a & MU_ACT_COMMIT, "YES with no trig held did not commit");

    /* YES always queues the commit decision; the UI tick refuses it while native */
    mu_in_init(&in);
    a = key(MU_KEY_YES, MU_FLAG_PRESS, 1, 0, 1);
    CHECK((a & MU_ACT_TAKE) && (a & MU_ACT_COMMIT), "YES did not queue a commit decision");
    /* native playback: the trig keys still edit as stock */
    mu_in_init(&in);
    a = key(MU_KEY_TRIG1, MU_FLAG_PRESS, 1, 0, 1);
    CHECK(a == MU_ACT_PASS, "native GRID RECORDING lost its trig key");

    /* FUNC + a track key mutes: it must not move the page's track */
    mu_in_init(&in);
    track = 1;
    a = key(MU_KEY_T1, MU_FLAG_PRESS, 1, 1, 1);
    CHECK((a & MU_ACT_SELECT) && track == 0 && a != MU_ACT_TAKE, "a plain track key did not select");
    mu_in_init(&in);
    track = 1;
    a = key(MU_KEY_T1, MU_FLAG_PRESS | MU_FLAG_FUNC, 1, 1, 1);
    CHECK(a == MU_ACT_PASS && track == 1, "FUNC+track moved the page's track");

    /* H is bound to the track it began on, and a consumed turn suppresses the tap reroll */
    mu_in_init(&in);
    track = 3;
    a = key(MU_KEY_PUSH_H, MU_FLAG_PRESS, 1, 1, 1);
    CHECK(a == MU_ACT_TAKE && in.h_track == 3, "H did not bind to its track");
    track = 0; /* a track key while H is held */
    a = key(MU_KEY_PUSH_H, MU_FLAG_RELEASE, 1, 1, 1);
    CHECK((a & MU_ACT_REROLL) && in.h_track == 3, "H's release lost its own track");
    mu_in_init(&in);
    track = 2;
    key(MU_KEY_PUSH_H, MU_FLAG_PRESS, 1, 1, 1);
    mu_in_turn_used(&in);
    a = key(MU_KEY_PUSH_H, MU_FLAG_RELEASE, 1, 1, 1);
    CHECK(!(a & MU_ACT_REROLL), "a reroll followed a consumed turn");

    /* a hold is a hold only where TURI drifts */
    mu_in_init(&in);
    mu_in_init(&in);
    key(MU_KEY_PUSH_H, MU_FLAG_PRESS, 1, 1, 1);
    for (int i = 0; i < 9; i++)
        a = mu_in_tick(&in, MU_EUCL);
    CHECK(in.holding == 0, "EUCL reported a hold");
    mu_in_init(&in);
    key(MU_KEY_PUSH_H, MU_FLAG_PRESS, 1, 1, 1);
    for (int i = 0; i < 9; i++)
        a = mu_in_tick(&in, MU_TURI);
    CHECK(in.holding == 1 && (a & MU_ACT_HOLD), "TURI did not report a hold");

    /* fractional encoder counts do not leak across a track or model change */
    mu_in_init(&in);
    CHECK(mu_in_detents(&in, 2, 2, 8, 0, MU_EUCL) == 0, "a half unit became a detent");
    CHECK(mu_in_detents(&in, 2, 2, 8, 0, MU_EUCL) == 0, "four counts are half a detent here");
    CHECK(mu_in_detents(&in, 2, 4, 8, 0, MU_EUCL) == 1, "eight counts did not make a unit");
    mu_in_detents(&in, 2, 2, 8, 0, MU_EUCL); /* two counts pending */
    CHECK(mu_in_detents(&in, 2, 2, 8, 1, MU_EUCL) == 0, "the count survived a track change");
    CHECK(mu_in_detents(&in, 2, 2, 8, 1, MU_TURI) == 0, "the count survived a model change");

    /* HITS 64 at a 16-step length edits from 16: the first detent down is 15 */
    CHECK(mu_in_effective(64, 16) == 16, "HITS 64/16 reads %d", mu_in_effective(64, 16));
    CHECK(mu_in_effective(5, 16) == 5, "HITS under the length moved");
    CHECK(mu_in_effective(5, 0) == 5, "an unknown length clamped HITS");

    /* A turn down starts at the count it plays; a turn up starts at the stored value, so the
     * knob can leave the loop the length caps instead of dying one step above it */
    CHECK(mu_in_hits_start(30, 16, -1) == 16, "a turn down did not start at the count it plays");
    CHECK(mu_in_hits_start(30, 16, 1) == 30, "a turn up lost the stored count");
    CHECK(mu_in_hits_start(5, 16, 1) == 5, "a turn up under the length moved");
    CHECK(mu_in_hits_start(5, 16, -1) == 5, "a turn down under the length moved");

    /* A track key held while the page is open is the core's Mod Menu; its release never
     * arrives, so the tick closes the page after the key repeat. A tap, FUNC and a page-closed
     * hold do not. */
    mu_in_init(&in);
    track = 0;
    a = key(MU_KEY_T1 + 1, MU_FLAG_PRESS, 1, 1, 1);
    CHECK(a & MU_ACT_SELECT, "a held page track key did not select");
    a = key(MU_KEY_T1 + 1, MU_FLAG_RELEASE, 1, 1, 1);
    for (int i = 0; i < 30; i++)
        a |= mu_in_tick(&in, MU_EUCL);
    CHECK(!(a & MU_ACT_MENU), "a tap closed the page");
    mu_in_init(&in);
    key(MU_KEY_T1 + 1, MU_FLAG_PRESS, 1, 1, 1);
    a = 0;
    for (int i = 0; i < MU_MENU_TICKS; i++)
        a |= mu_in_tick(&in, MU_EUCL);
    CHECK((a & MU_ACT_MENU) && in.track_down == 0xff, "a held track key left the page open");
    CHECK((a & MU_ACT_MENU) == 0 || !(mu_in_tick(&in, MU_EUCL) & MU_ACT_MENU),
          "the menu action repeated");
    mu_in_init(&in);
    key(MU_KEY_T1 + 2, MU_FLAG_PRESS | MU_FLAG_FUNC, 1, 1, 1);
    for (int i = 0; i < 30; i++)
        a = mu_in_tick(&in, MU_EUCL);
    CHECK(!(a & MU_ACT_MENU), "FUNC + a track key closed the page");
    mu_in_init(&in);
    key(MU_KEY_T1, MU_FLAG_PRESS, 0, 1, 1);
    for (int i = 0; i < 30; i++)
        a = mu_in_tick(&in, MU_EUCL);
    CHECK(!(a & MU_ACT_MENU), "a track key held with the page closed armed the menu");

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("input tests passed\n");
    return 0;
}
