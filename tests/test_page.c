/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host property tests of the page: every state draws inside the 128x64 display and inside its own
 * cell; the value and label texts are the state's; the trig keys are the loop the engine plays. */
#include "../mods/mutiny/page.h"
#include "../mods/mutiny/preview.h"
#include <stdio.h>
#include <string.h>

extern uint32_t mu_engine_acid_generations, mu_engine_previews, mu_engine_renders;

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

static uint8_t pix[64][128];
static int outside;
static char last_text[8][64];
static int text_count;

/* A text's width: the stock fonts advance 4 px (font5), 5 px (label) and 6 px (title). */
static int text_width(uint8_t font, const char *s) {
    return (font == MU_FONT_TITLE ? 6 : font == MU_FONT_LABEL ? 5 : 4) * (int)strlen(s);
}

static void t_fill(void *ctx, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t colour) {
    int32_t x, y;
    (void)ctx;
    if (x0 > x1) {
        int32_t t = x0;
        x0 = x1;
        x1 = t;
    }
    if (y0 > y1) {
        int32_t t = y0;
        y0 = y1;
        y1 = t;
    }
    if (x0 < 0 || x1 > 127 || y0 < 0 || y1 > 63) {
        outside++;
        return;
    }
    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++)
            pix[y][x] = colour == 0 ? 0 : 1;
}

static void t_frame(void *ctx, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t colour) {
    t_fill(ctx, x0, y0, x1, y0, colour);
    t_fill(ctx, x0, y1, x1, y1, colour);
    t_fill(ctx, x0, y0, x0, y1, colour);
    t_fill(ctx, x1, y0, x1, y1, colour);
}

static void t_text(void *ctx, uint8_t font, int32_t x, int32_t y, const char *s) {
    int32_t i, w;
    (void)ctx;
    if (text_count < 8)
        snprintf(last_text[text_count], sizeof last_text[0], "%s", s);
    text_count++;
    w = text_width(font, s);
    if (x < 15 || x + w > 127 || y < 0 || y + 5 > 63)
        outside++;
    for (i = 0; i < w && x + i < 128; i++)
        if (x + i >= 0 && y >= 0 && y < 64)
            pix[y][x + i] = 1;
}

static const struct mu_canvas CANVAS = {0, t_fill, t_frame, t_text};

static struct mu_preview_cache previews;
static struct mu_acid_cache acids[4];

/* A picture is its seven fields; padding is not part of the musical result. */
static int preview_equal(const struct mu_preview *a, const struct mu_preview *b) {
    return a->gates == b->gates && a->play == b->play && a->accent == b->accent &&
           a->slide == b->slide && a->len == b->len && a->cells == b->cells && a->start == b->start;
}

static uint8_t other(uint8_t v, int32_t lo, int32_t hi) {
    return (uint8_t)(v < hi ? v + 1 : v > lo ? v - 1 : v);
}

/* A genuine input change must build exactly once, match the pure engine and then be reusable. */
static void expect_rebuild(const struct mu_track *base, const struct mu_key *base_key,
                           const struct mu_track *changed, const struct mu_key *changed_key,
                           const char *what) {
    struct mu_preview reference;
    const struct mu_preview *prepared;
    mu_preview_cache_reset(&previews);
    CHECK(mu_preview_cache_prepare(&previews, 0, base, base_key, &acids[0]) == 1, "%s: base prep",
          what);
    CHECK(mu_preview_cache_prepare(&previews, 0, base, base_key, &acids[0]) == 0, "%s: base reuse",
          what);
    CHECK(mu_preview_cache_prepare(&previews, 0, changed, changed_key, &acids[0]) == 1,
          "%s: no rebuild", what);
    mu_preview(changed, changed_key, &reference);
    prepared = mu_preview_cache_ready(&previews, 0, changed, changed_key);
    CHECK(prepared && preview_equal(prepared, &reference), "%s: cached picture differs", what);
    CHECK(mu_preview_cache_prepare(&previews, 0, changed, changed_key, &acids[0]) == 0,
          "%s: rebuilt twice", what);
}

static void clear(void) {
    memset(pix, 0, sizeof pix);
    outside = 0;
    text_count = 0;
    memset(last_text, 0, sizeof last_text);
}

/* Picture bands: a picture may only use its row's band, and text only its label band. */
static int bad_band(void) {
    int x, y;
    for (y = 0; y < 64; y++)
        for (x = 0; x < 128; x++) {
            if (!pix[y][x])
                continue;
            if (x < 15)
                return 1;
            if (y >= 54)
                continue; /* the title bar */
            if (y <= 4 || (y >= 27 && y <= 31))
                continue; /* labels */
            if (y >= 5 && y <= 25)
                continue; /* row 1's pictures */
            if (y >= 32 && y <= 52)
                continue; /* row 0's pictures */
            return 1;
        }
    return 0;
}

/* The production preparation/consumption pair the page glue uses: build only when the actual
 * recipe/key changed, then copy the prepared picture into the state the page draws. */
static void take_preview(struct mu_page_state *st, const struct mu_key *key, uint8_t track) {
    const struct mu_preview *prepared = mu_preview_cache_ready(&previews, track, &st->recipe, key);
    if (!prepared) {
        mu_preview_cache_prepare(&previews, track, &st->recipe, key, &acids[track]);
        prepared = mu_preview_cache_ready(&previews, track, &st->recipe, key);
    }
    if (!prepared) {
        failures++;
        printf("FAIL %s:%d: the production cache returned no preview\n", __FILE__, __LINE__);
        return;
    }
    st->preview = *prepared;
}

static void fixture(struct mu_page_state *st, uint8_t model, int8_t scale, uint32_t track_len,
                    uint32_t pages) {
    struct mu_key key = {60, 60, scale, (uint8_t)(track_len > 64 ? 64 : track_len)};
    memset(st, 0, sizeof *st);
    mu_defaults(&st->recipe);
    st->recipe.model = model;
    st->scale = scale;
    st->track_len = (uint8_t)(track_len > 64 ? 64 : track_len);
    st->pages = pages;
    st->visible = 1;
    st->touched = 0xff;
    mu_preview_cache_reset(&previews);
    take_preview(st, &key, 0);
    for (int k = 0; k < MU_KNOBS; k++)
        st->value[k] = k ? st->recipe.p[model][k - 1] : model;
    st->seed = mu_acid_seed(&st->recipe);
}

int main(void) {
    struct mu_page_state st;
    uint8_t keys[16];
    static const uint32_t lens[] = {1, 2, 8, 12, 16, 17, 32, 48, 64};

    /* every model, every known scale and length: no pixel or text leaves the display or its band */
    for (int m = MU_EUCL; m <= MU_TB3P; m++)
        for (int scale = -1; scale <= 34; scale++)
            for (unsigned li = 0; li < sizeof lens / sizeof lens[0]; li++)
                for (int trk = 0; trk <= 1; trk++) {
                    clear();
                    fixture(&st, (uint8_t)m, (int8_t)scale, lens[li], 1 + (uint32_t)(li % 4));
                    if (trk) {
                        struct mu_key key = {60, 60, (int8_t)scale,
                                             (uint8_t)(lens[li] > 64 ? 64 : lens[li])};
                        st.recipe.p[m][0] = (uint8_t)(mu_model_cap((uint8_t)m) + 1); /* TRK */
                        take_preview(&st, &key, 0);
                    }
                    mu_page_draw(&CANVAS, &st);
                    CHECK(outside == 0, "model %d scale %d len %u: drew outside the display", m,
                          scale, lens[li]);
                    CHECK(!bad_band(), "model %d scale %d len %u: a picture left its band", m,
                          scale, lens[li]);
                }

    /* the extremes of every knob, and HITS above STEP */
    for (int m = MU_EUCL; m <= MU_TB3P; m++)
        for (int k = 1; k <= MU_PARAMS; k++)
            for (int which = 0; which < 3; which++) {
                uint8_t model = (uint8_t)m;
                uint8_t *p;
                clear();
                fixture(&st, model, 0, 16, 2);
                p = st.recipe.p[m];
                p[k - 1] = (uint8_t)(which == 0   ? mu_knob_min(model, (uint8_t)k)
                                     : which == 1 ? mu_knob_max(model, (uint8_t)k)
                                                  : (mu_knob_min(model, (uint8_t)k) +
                                                     mu_knob_max(model, (uint8_t)k)) /
                                                        2);
                st.value[k] = p[k - 1];
                {
                    struct mu_key key = {60, 60, 0, 16};
                    take_preview(&st, &key, 0);
                }
                mu_page_draw(&CANVAS, &st);
                CHECK(outside == 0 && !bad_band(), "model %d knob %d value %d", m, k, p[k - 1]);
            }

    /* HITS 64 with STEP 16: the ring shows 16 cells and the count is 16, not 64 */
    clear();
    fixture(&st, MU_EUCL, 0, 16, 1);
    st.recipe.p[MU_EUCL][0] = 16;
    st.recipe.p[MU_EUCL][1] = 64;
    {
        struct mu_key key = {60, 60, 0, 16};
        take_preview(&st, &key, 0);
    }
    st.value[2] = 64;
    mu_page_draw(&CANVAS, &st);
    CHECK(st.preview.cells == 16, "the ring has %u cells", st.preview.cells);
    {
        uint32_t i, shown = 0;
        for (i = 0; i < st.preview.cells; i++)
            shown += st.preview.gates >> i & 1u;
        CHECK(shown == 16, "HITS 64/16 shows %u", shown);
    }

    /* the trig keys are the loop's own steps on the visible page */
    clear();
    fixture(&st, MU_EUCL, 0, 32, 2);
    mu_page_keys(&st, keys);
    {
        struct mu_key key = {60, 60, 0, 32};
        take_preview(&st, &key, 0);
    }
    for (int i = 0; i < 16; i++)
        CHECK((keys[i] != 0) == ((st.preview.play >> i & 1u) != 0), "page 1 key %d", i);
    st.page = 1;
    mu_page_keys(&st, keys);
    for (int i = 0; i < 16; i++)
        CHECK((keys[i] != 0) == ((st.preview.play >> (16 + i) & 1u) != 0), "page 2 key %d", i);
    st.page = 0;
    st.track_len = 12;
    mu_page_keys(&st, keys);
    for (int i = 12; i < 16; i++)
        CHECK(keys[i] == 0, "a key past the track's length is lit at %d", i);

    /* the title says the playback source and the available NO action */
    fixture(&st, MU_EUCL, 0, 16, 1);
    st.play = MU_PLAY_GENERATED;
    st.overload = 1;
    CHECK(strcmp(mu_page_title(&st), "Mutiny: Overload") == 0, "overload title");
    st.overload = 0;
    CHECK(strcmp(mu_page_title(&st), "Mutiny: Live") == 0, "generated title %s",
          mu_page_title(&st));
    st.play = MU_PLAY_NATIVE;
    st.undo = 1;
    CHECK(strcmp(mu_page_title(&st), "Mutiny: Undo") == 0, "undo title %s", mu_page_title(&st));
    st.undo = 0;
    CHECK(strcmp(mu_page_title(&st), "Mutiny: Native") == 0, "native title %s", mu_page_title(&st));
    st.holding = 1;
    CHECK(strcmp(mu_page_title(&st), "Mutiny: Hold") == 0, "hold title %s", mu_page_title(&st));
    st.holding = 0;
    st.kept = 1;
    CHECK(strcmp(mu_page_title(&st), "Mutiny: Kept") == 0, "kept title %s", mu_page_title(&st));
    st.kept = 2;
    CHECK(strcmp(mu_page_title(&st), "Mutiny: Not kept") == 0, "refused title %s",
          mu_page_title(&st));

    /* the title names the page YES writes once the track has more than one */
    fixture(&st, MU_EUCL, 0, 64, 4);
    st.play = MU_PLAY_GENERATED;
    st.key_page = 1;
    CHECK(strcmp(mu_page_title(&st), "Mutiny: Live P2") == 0, "live page title %s",
          mu_page_title(&st));
    st.kept = 1;
    st.kept_page = 3;
    CHECK(strcmp(mu_page_title(&st), "Mutiny: Kept P4") == 0, "kept page title %s",
          mu_page_title(&st));
    st.pages = 1;
    CHECK(strcmp(mu_page_title(&st), "Mutiny: Kept") == 0, "one page added a page suffix %s",
          mu_page_title(&st));
    {
        clear();
        fixture(&st, MU_EUCL, 0, 64, 4);
        st.play = MU_PLAY_NATIVE;
        st.key_page = 3;
        mu_page_draw(&CANVAS, &st);
        CHECK(outside == 0, "a page title leaves the bar");
        CHECK(19 + text_width(MU_FONT_TITLE, mu_page_title(&st)) <= 127, "page title is %d px",
              text_width(MU_FONT_TITLE, mu_page_title(&st)));
    }

    /* the title fits the bar for every state */
    {
        uint8_t states[][4] = {
            {1, 1, 0, 1}, {1, 1, 1, 0}, {0, 1, 0, 0}, {0, 0, 0, 0}, {2, 0, 0, 0}};
        for (unsigned i = 0; i < sizeof states / sizeof states[0]; i++) {
            clear();
            fixture(&st, MU_EUCL, 0, 16, 1);
            st.kept = states[i][0];
            st.holding = states[i][1];
            st.play = states[i][2];
            st.undo = states[i][3];
            mu_page_draw(&CANVAS, &st);
            CHECK(outside == 0, "title %s leaves the bar", mu_page_title(&st));
            CHECK(19 + text_width(MU_FONT_TITLE, mu_page_title(&st)) <= 127, "title %s is %d px",
                  mu_page_title(&st), text_width(MU_FONT_TITLE, mu_page_title(&st)));
        }
    }

    /* MODEL OFF shows the MODEL cell only (labels are the model's and the values are quiet) */
    clear();
    fixture(&st, MU_OFF, 0, 16, 1);
    st.recipe.model = MU_OFF;
    mu_page_draw(&CANVAS, &st);
    CHECK(outside == 0 && !bad_band(), "the OFF page left the display");

    /* One prepared TB3P picture, then 30 animation-only redraws: title, roll, playhead, commit and
     * preview states change, but no generator, preview build or recipe render may run. */
    {
        struct mu_key key = {60, 60, 0, 16};
        uint32_t acid, builds, renders;
        fixture(&st, MU_TB3P, 0, 16, 1);
        acid = mu_engine_acid_generations;
        builds = mu_engine_previews;
        renders = mu_engine_renders;
        for (int i = 0; i < 30; i++) {
            const struct mu_preview *prepared;
            st.touched = (uint8_t)(i % MU_KNOBS);
            st.shown = (uint8_t)(i & 1);
            st.roll = (int8_t)(i % 16);
            st.page = (uint8_t)(i % 4);
            st.step = (uint8_t)(i % 16);
            st.playing = 1;
            st.kept = (uint8_t)(i % 3);
            st.holding = (uint8_t)(i & 1);
            st.play = (i & 1) ? MU_PLAY_GENERATED : MU_PLAY_NATIVE;
            st.undo = (uint8_t)((i & 2) != 0);
            st.overload = (uint8_t)(i == 7);
            prepared = mu_preview_cache_ready(&previews, 0, &st.recipe, &key);
            CHECK(prepared != NULL, "redraw %d lost its prepared preview", i);
            if (prepared)
                st.preview = *prepared;
            clear();
            mu_page_draw(&CANVAS, &st);
            CHECK(outside == 0 && !bad_band(), "animation redraw %d left the display", i);
        }
        CHECK(mu_engine_acid_generations == acid, "animation rebuilt %u TB3P patterns",
              mu_engine_acid_generations - acid);
        CHECK(mu_engine_previews == builds, "animation rebuilt %u previews",
              mu_engine_previews - builds);
        CHECK(mu_engine_renders == renders, "animation made %u recipe renders",
              mu_engine_renders - renders);
    }

    /* Every actual input rebuilds once and matches the pure engine; a second use reuses it. */
    {
        struct mu_key key = {60, 60, 0, 16};
        struct mu_track t;
        for (int m = MU_EUCL; m <= MU_TB3P; m++) {
            mu_defaults(&t);
            t.model = (uint8_t)m;
            {
                struct mu_track a = t;
                a.model = (uint8_t)(m == MU_TB3P ? MU_EUCL : m + 1);
                expect_rebuild(&t, &key, &a, &key, "model");
            }
            {
                struct mu_track a = t;
                mu_acid_set_seed(&a, (uint32_t)mu_acid_seed(&a) + 1u);
                expect_rebuild(&t, &key, &a, &key, "seed");
            }
            {
                struct mu_track a = t;
                a.src.g[0] ^= 0x01;
                a.src.p[0] ^= 0x02;
                a.src.a[0] ^= 0x04;
                expect_rebuild(&t, &key, &a, &key, "source");
            }
            {
                struct mu_track a = t;
                a.p[m][0] =
                    other(a.p[m][0], mu_knob_min((uint8_t)m, 1), mu_knob_max((uint8_t)m, 1));
                expect_rebuild(&t, &key, &a, &key, "loop");
            }
            if (m == MU_EUCL || m == MU_TURI) {
                struct mu_track a = t;
                uint8_t k = (uint8_t)(m == MU_EUCL ? 2 : 4);
                a.p[m][k] = other(a.p[m][k], mu_knob_min((uint8_t)m, (uint8_t)(k + 1)),
                                  mu_knob_max((uint8_t)m, (uint8_t)(k + 1)));
                expect_rebuild(&t, &key, &a, &key, "rotation");
            }
            {
                struct mu_track a = t;
                a.p[m][1] =
                    other(a.p[m][1], mu_knob_min((uint8_t)m, 2), mu_knob_max((uint8_t)m, 2));
                expect_rebuild(&t, &key, &a, &key, "density");
            }
            {
                struct mu_track a = t;
                uint8_t k = (uint8_t)(m == MU_EUCL ? 4 : m == MU_TURI ? 3 : 4);
                a.p[m][k] = other(a.p[m][k], mu_knob_min((uint8_t)m, (uint8_t)(k + 1)),
                                  mu_knob_max((uint8_t)m, (uint8_t)(k + 1)));
                expect_rebuild(&t, &key, &a, &key, "accent");
            }
            if (m == MU_TB3P) {
                struct mu_track a = t;
                a.p[m][3] =
                    other(a.p[m][3], mu_knob_min((uint8_t)m, 4), mu_knob_max((uint8_t)m, 4));
                expect_rebuild(&t, &key, &a, &key, "slide");
            }
            {
                struct mu_key k2 = key;
                k2.scale = (int8_t)(key.scale == 0 ? 1 : 0);
                expect_rebuild(&t, &key, &t, &k2, "scale");
            }
            {
                struct mu_key k2 = key;
                k2.note = other(key.note, 0, 127);
                expect_rebuild(&t, &key, &t, &k2, "note");
            }
            {
                struct mu_key k2 = key;
                k2.root = other(key.root, 0, 127);
                expect_rebuild(&t, &key, &t, &k2, "root");
            }
            {
                struct mu_key k2 = key;
                k2.len = other(key.len, 1, 64);
                expect_rebuild(&t, &key, &t, &k2, "length");
            }
        }
    }

    /* TURI evolution and reroll change the source with the knobs untouched. */
    {
        struct mu_key key = {60, 60, 0, 16};
        struct mu_track t, base;
        mu_defaults(&t);
        t.model = MU_TURI;
        t.p[MU_TURI][6] = 0; /* LOCK 0: every pass replaces source steps */
        base = t;
        mu_evolve(&t, 1u);
        CHECK(memcmp(t.p, base.p, sizeof t.p) == 0, "TURI evolution moved a knob");
        expect_rebuild(&base, &key, &t, &key, "TURI evolution");
        base = t;
        mu_reroll(&t);
        CHECK(memcmp(t.p, base.p, sizeof t.p) == 0, "TURI reroll moved a knob");
        expect_rebuild(&base, &key, &t, &key, "TURI reroll");
    }

    /* Track context and lifecycle reset invalidate without a recipe edit. */
    {
        struct mu_key key = {60, 60, 0, 16};
        struct mu_track t;
        mu_defaults(&t);
        t.model = MU_EUCL;
        mu_preview_cache_reset(&previews);
        CHECK(mu_preview_cache_prepare(&previews, 0, &t, &key, &acids[0]) == 1, "track 0 prep");
        CHECK(mu_preview_cache_ready(&previews, 1, &t, &key) == NULL,
              "track 1 borrowed track 0's picture");
        CHECK(mu_preview_cache_prepare(&previews, 1, &t, &key, &acids[1]) == 1,
              "track change did not rebuild");
        CHECK(mu_preview_cache_prepare(&previews, 1, &t, &key, &acids[1]) == 0,
              "track context rebuilt twice");
        mu_preview_cache_reset(&previews);
        CHECK(mu_preview_cache_ready(&previews, 1, &t, &key) == NULL,
              "a lifecycle reset kept the picture");
        CHECK(mu_preview_cache_prepare(&previews, 1, &t, &key, &acids[1]) == 1,
              "a lifecycle reset did not rebuild");
    }

    /* Repeated reroll repaints through the prepared picture. */
    {
        struct mu_key key = {60, 60, 0, 16};
        fixture(&st, MU_TURI, 0, 16, 1);
        for (int i = 0; i < 3; i++) {
            mu_reroll(&st.recipe);
            take_preview(&st, &key, 0);
            clear();
            mu_page_draw(&CANVAS, &st);
            CHECK(outside == 0 && !bad_band(), "reroll repaint %d left the display", i);
        }
    }

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("page tests passed\n");
    return 0;
}
