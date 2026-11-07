/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host tests of MUTINY's OS 1.44 port boundary that need no firmware.
 *
 * These pin the raw hook-event field offsets and the native track/lock offset mapping used by the
 * commit device. They do not test firmware calls, the LEN assembly, lifecycle identity or timing.
 */
#include "../mods/mutiny/port144.h"
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

int main(void) {
    uint32_t ev[5] = {0};
    uint32_t last;

    ev[3] = 0x11223344u;
    ev[4] = 0xa1b2c3d4u;
    CHECK(mu_port_ev_code(ev) == 0x11223344u, "key/encoder field");
    CHECK(mu_port_ev_flags(ev) == 0xa1b2c3d4u, "key flags field");
    CHECK(mu_port_ev_delta(ev) == (int32_t)0xa1b2c3d4u, "encoder delta field is signed");

    /* 976 bytes of track data per track, then 0x284f bytes of locks per track. */
    CHECK(MU_PORT_TRACK_STRIDE == 976u, "track stride");
    CHECK(MU_PORT_NATIVE_BYTES == 976u + 0x284fu, "native extent");
    CHECK(mu_port_native_offset(0, 0) == 0u, "track 0 data");
    CHECK(mu_port_native_offset(1, 0) == 976u, "track 1 data");
    CHECK(mu_port_native_offset(3, 975) == 3u * 976u + 975u, "track 3's last data byte");
    CHECK(mu_port_native_offset(0, MU_PORT_TRACK_STRIDE) == MU_PORT_LOCK_BASE, "track 0 locks");
    CHECK(mu_port_native_offset(3, MU_PORT_TRACK_STRIDE) ==
              MU_PORT_LOCK_BASE + 3u * MU_PORT_TRACK_LOCKS,
          "track 3 locks");
    last = (uint32_t)(MU_PORT_NATIVE_BYTES - 1u);
    CHECK(mu_port_native_offset(3, last) == MU_PORT_LOCK_BASE + 4u * MU_PORT_TRACK_LOCKS - 1u,
          "the last native byte");

    /* The measured fields the layout checks bind. */
    CHECK(MU_PORT_TR_LEN_OFFSET == 0x3c2u && MU_PORT_TR_LEN_OFFSET < MU_PORT_TRACK_STRIDE,
          "track LEN offset");
    CHECK(MU_PORT_TR_NOTES + 64u * 8u == MU_PORT_TR_NOTE, "notes end at NOTE");
    CHECK(MU_PORT_LK_USED + MU_PORT_LK_SLOTS == MU_PORT_TRACK_LOCKS, "lock area extent");
    CHECK(MU_PORT_LK_STEP * 64u == MU_PORT_LK_USED, "64 lock blocks");
    CHECK(MU_PORT_LEN_SITE == 0x40094688u, "LEN site");
    CHECK(strcmp(MU_PORT_LEN_SITE_STOCK, "7f2900c0b287") == 0, "LEN stock bytes");

    if (failures)
        return 1;
    printf("port tests passed\n");
    return 0;
}
