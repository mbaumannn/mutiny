/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host test of the LEN memo's publication order (RN02).
 *
 * The step fire reads the memo at interrupt level, so mu_len_memo_bind() must never expose an entry
 * from the old pattern under the new pattern's identity. This test compiles commit.c with
 * -DMU_LEN_MEMO_PROBE so the probe runs exactly at the publication boundaries, as an interrupt
 * would: the invariant is that whenever the memo's identity would be accepted for the new pattern,
 * no entry of the old one is left. */
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

#define PROBE_POINTS 8
static uint32_t new_id;
static int observations, accepted_stale, saw_retired, saw_published;
static uint32_t seen_id[PROBE_POINTS];
static uint8_t seen_base[PROBE_POINTS];

void mu_len_memo_probe(const struct mu_len_memo *m) {
    if (observations < PROBE_POINTS) {
        seen_id[observations] = m->identity;
        seen_base[observations] = m->base[63];
    }
    observations++;
    if (m->identity == 0)
        saw_retired = 1;
    if (m->identity == new_id) {
        saw_published = 1;
        if (m->base[63] != 0xff)
            accepted_stale = 1; /* a reader would have trusted the old pattern's base here */
    }
}

int main(void) {
    const uint32_t a = 0x1111u, b = 0x2222u;
    struct mu_len_memo m;

    memset(&m, 0, sizeof m);
    mu_len_memo_init(&m);

    /* pattern A: step 63 starts from the default 14, so Mutiny wrote 22 */
    m.identity = a;
    m.base[63] = 14;
    /* A's 22 is Mutiny's own extension of the default 14, so it resolves back to 14 and holds at 22 */
    CHECK(mu_held_base(22, 14, 63, mu_len_memo_for(&m, a)) == 14,
          "pattern A's own entry was not used: base %d",
          (int)mu_held_base(22, 14, 63, mu_len_memo_for(&m, a)));
    CHECK(mu_held_len(mu_held_base(22, 14, 63, mu_len_memo_for(&m, a))) == 22,
          "pattern A compounded its own extension");

    /* pattern B holds an explicit 22 by hand: only a cleared memo extends it to 26 */
    new_id = b;
    mu_len_memo_bind(&m, b);

    CHECK(observations >= 3, "the publication boundaries were not observed (%d)", observations);
    CHECK(saw_retired, "the identity was never retired before the entries were cleared");
    CHECK(saw_published, "the new identity was never published");
    CHECK(!accepted_stale, "an interrupt at a boundary could trust the old pattern's entry");
    CHECK(m.identity == b, "the new identity was not published (%08x)", m.identity);
    CHECK(m.base[63] == 0xff, "an entry of the old pattern survived (%u)", m.base[63]);
    CHECK(mu_held_len(mu_held_base(22, 14, 63, mu_len_memo_for(&m, b))) == 26,
          "pattern B's explicit 22 was read as Mutiny's own: %d",
          (int)mu_held_len(mu_held_base(22, 14, 63, mu_len_memo_for(&m, b))));

    /* rebinding to the same identity keeps the entries: a repeated commit does not compound */
    mu_held_record(&m, 63, 22);
    mu_len_memo_bind(&m, b);
    CHECK(m.base[63] == 22, "a same-identity bind dropped its own entry");
    CHECK(mu_held_len(mu_held_base(26, 14, 63, mu_len_memo_for(&m, b))) == 26,
          "the recorded base did not reproduce the written length");

    /* and a bind to a third pattern clears it again */
    mu_len_memo_bind(&m, 0x3333u);
    CHECK(m.base[63] == 0xff && m.identity == 0x3333u, "a third pattern kept the old entry");

    for (int i = 0; i < observations && i < PROBE_POINTS; i++)
        printf("observation %d: id %08x base[63] %u\n", i, seen_id[i], seen_base[i]);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("memo publication tests passed\n");
    return 0;
}
