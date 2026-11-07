/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host tests of MUTINY's command ownership: a command carries the track and the native context it
 * was made in, and a held key is captured at its press, not at its release. */
#include "../mods/mutiny/cmd.h"
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

static uint8_t pat_a, pat_b, proj_a, proj_b;

int main(void) {
    struct mu_cmd c, out;
    struct mu_gesture g;

    /* a command holds only while its own track context is still the live one */
    mu_cmd_arm(&c, &pat_a, &proj_a, 2, 0);
    CHECK(mu_cmd_holds(&c, &pat_a, &proj_a), "a fresh command did not hold");
    CHECK(!mu_cmd_holds(&c, &pat_b, &proj_a), "a command held across a pattern change");
    CHECK(!mu_cmd_holds(&c, &pat_a, &proj_b), "a command held across a project change");
    CHECK(!mu_cmd_holds(&c, &pat_a, &proj_a) || c.track == 2, "the track was not kept");
    mu_cmd_clear(&c);
    CHECK(!mu_cmd_holds(&c, &pat_a, &proj_a), "a cleared command still held");

    /* no pattern is no context */
    mu_cmd_arm(&c, 0, &proj_a, 0, 0);
    CHECK(!mu_cmd_holds(&c, 0, &proj_a), "a command without a pattern held");

    /* a held gesture captures at the press; the release only activates it */
    mu_gesture_press(&g, &pat_a, &proj_a, 1, 0);
    CHECK(g.owned && g.cmd.req && g.cmd.track == 1, "the press was not captured");
    CHECK(mu_gesture_release(&g, &pat_a, &proj_a, &out) && out.req && out.track == 1,
          "the release did not activate its own context");
    CHECK(!g.owned && !g.cmd.req, "the gesture stayed owned after its release");

    /* a pattern change between press and release expires the gesture (RN01) */
    mu_gesture_press(&g, &pat_a, &proj_a, 1, 0);
    out.req = 0;
    CHECK(!mu_gesture_release(&g, &pat_b, &proj_a, &out), "a cross-pattern release activated");
    CHECK(!out.req, "a refused activation still armed the command");
    CHECK(!g.owned && !g.cmd.req, "a refused gesture left state behind");

    /* the same for a project change */
    mu_gesture_press(&g, &pat_a, &proj_a, 1, 0);
    CHECK(!mu_gesture_release(&g, &pat_a, &proj_b, &out), "a cross-project release activated");

    /* a press that ends without an action leaves nothing behind */
    mu_gesture_press(&g, &pat_a, &proj_a, 3, 0);
    mu_gesture_end(&g);
    CHECK(!g.owned && !g.cmd.req, "an ended gesture stayed armed");
    CHECK(!mu_gesture_release(&g, &pat_a, &proj_a, &out), "an ended gesture activated");

    /* a track selected after the press is not in the command at all */
    mu_gesture_press(&g, &pat_a, &proj_a, 0, 0);
    CHECK(mu_gesture_release(&g, &pat_a, &proj_a, &out) && out.track == 0,
          "the release moved the gesture's track");

    /* the page the sequencer keys were on travels with the gesture (page commit) */
    mu_gesture_press(&g, &pat_a, &proj_a, 1, 2);
    CHECK(g.cmd.page == 2, "the press lost its page");
    CHECK(mu_gesture_release(&g, &pat_a, &proj_a, &out) && out.page == 2,
          "the release lost the gesture's page");
    mu_cmd_arm(&c, &pat_a, &proj_a, 1, 9);
    CHECK(c.page == 3, "a page past the fourth was not clamped");

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("command tests passed\n");
    return 0;
}
