#include "light.h"
#include "test.h"

#include <stdint.h>

/* --- helpers ------------------------------------------------------------ */

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xffff); put16(p + 2, v >> 16); }

/* Write a 54-byte header for a 24-bit BI_RGB bitmap. Returns data offset. */
static size_t bmp_header(uint8_t *b, int32_t w, int32_t h, size_t data_len)
{
    memset(b, 0, 54);
    b[0] = 'B';
    b[1] = 'M';
    put32(b + 2, (uint32_t)(54 + data_len));
    put32(b + 10, 54);
    put32(b + 14, 40);
    put32(b + 18, (uint32_t)w);
    put32(b + 22, (uint32_t)h);
    put16(b + 26, 1);
    put16(b + 28, 24);
    return 54;
}

static fixed4_t level_of_uniform(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t px[4 * 3 * 3];
    for (size_t i = 0; i < sizeof px; i += 3) {
        px[i] = b;
        px[i + 1] = g;
        px[i + 2] = r;
    }
    fixed4_t out = -1;
    CHECK_EQ(light_from_bgr24(px, 4, 3, 12, &out), LIGHT_OK);
    return out;
}

/* --- light math --------------------------------------------------------- */

static void test_uniform_images(void)
{
    CHECK_EQ(level_of_uniform(0, 0, 0), 0);
    CHECK_EQ(level_of_uniform(255, 255, 255), 1000000);
    /* grey 128: 128000 / 255000 * 1e6 = 501960.784... -> 501961 */
    CHECK_EQ(level_of_uniform(128, 128, 128), 501961);
    /* pure red: 299 * 255 / 255000 = 29.9% exactly */
    CHECK_EQ(level_of_uniform(255, 0, 0), 299000);
}

/*
 * 3x2 image: row stride is 9 bytes of pixels + 3 bytes padding = 12.
 * Padding is filled with 0xFF so reading it by mistake changes the answer.
 *   lumas: red 76245, green 149685, blue 29070, white 255000, black 0,
 *          grey128 128000 -> sum 638000 over 6 px
 *   level = 638000 * 200 / (6 * 51) = 416993.46... -> 416993
 */
static size_t build_tiny_bmp(uint8_t *buf, int32_t h_sign)
{
    static const uint8_t rows[2][9] = {
        { 0, 0, 255,   0, 255, 0,   255, 0, 0 },      /* red, green, blue (BGR) */
        { 255, 255, 255,   0, 0, 0,   128, 128, 128 }, /* white, black, grey */
    };
    size_t off = bmp_header(buf, 3, 2 * h_sign, 24);
    for (int y = 0; y < 2; y++) {
        memcpy(buf + off + (size_t)y * 12, rows[y], 9);
        memset(buf + off + (size_t)y * 12 + 9, 0xFF, 3);
    }
    return off + 24;
}

static void test_tiny_bmp_with_padding(void)
{
    uint8_t buf[54 + 24];
    fixed4_t out = -1;

    size_t len = build_tiny_bmp(buf, 1);   /* bottom-up */
    CHECK_EQ(light_from_bmp(buf, len, &out), LIGHT_OK);
    CHECK_EQ(out, 416993);

    len = build_tiny_bmp(buf, -1);         /* top-down, as rpicam-still writes */
    out = -1;
    CHECK_EQ(light_from_bmp(buf, len, &out), LIGHT_OK);
    CHECK_EQ(out, 416993);

    /* Last row's padding omitted is still fine... */
    out = -1;
    CHECK_EQ(light_from_bmp(buf, len - 3, &out), LIGHT_OK);
    CHECK_EQ(out, 416993);
    /* ...but losing a real pixel byte is not. */
    CHECK_EQ(light_from_bmp(buf, len - 4, &out), LIGHT_E_TRUNCATED);
}

static void test_bmp_rejects(void)
{
    uint8_t buf[54 + 24];
    fixed4_t out = 0;
    size_t len;

    len = build_tiny_bmp(buf, 1);
    buf[0] = 'X';
    CHECK_EQ(light_from_bmp(buf, len, &out), LIGHT_E_BAD_MAGIC);

    len = build_tiny_bmp(buf, 1);
    put16(buf + 28, 32);                  /* 32-bit */
    CHECK_EQ(light_from_bmp(buf, len, &out), LIGHT_E_UNSUPPORTED);

    len = build_tiny_bmp(buf, 1);
    put16(buf + 28, 8);                   /* palettized */
    CHECK_EQ(light_from_bmp(buf, len, &out), LIGHT_E_UNSUPPORTED);

    len = build_tiny_bmp(buf, 1);
    put32(buf + 30, 1);                   /* BI_RLE8 */
    CHECK_EQ(light_from_bmp(buf, len, &out), LIGHT_E_UNSUPPORTED);

    len = build_tiny_bmp(buf, 1);
    put16(buf + 26, 2);                   /* planes != 1 */
    CHECK_EQ(light_from_bmp(buf, len, &out), LIGHT_E_UNSUPPORTED);

    len = build_tiny_bmp(buf, 1);
    put32(buf + 14, 12);                  /* OS/2 core header */
    CHECK_EQ(light_from_bmp(buf, len, &out), LIGHT_E_UNSUPPORTED);

    len = build_tiny_bmp(buf, 1);
    put32(buf + 18, 0);                   /* zero width */
    CHECK_EQ(light_from_bmp(buf, len, &out), LIGHT_E_UNSUPPORTED);

    len = build_tiny_bmp(buf, 1);
    put32(buf + 18, (uint32_t)-3);        /* negative width */
    CHECK_EQ(light_from_bmp(buf, len, &out), LIGHT_E_UNSUPPORTED);

    len = build_tiny_bmp(buf, 1);
    put32(buf + 22, 0);                   /* zero height */
    CHECK_EQ(light_from_bmp(buf, len, &out), LIGHT_E_UNSUPPORTED);

    len = build_tiny_bmp(buf, 1);
    put32(buf + 22, 0x80000000u);         /* INT32_MIN height */
    CHECK_EQ(light_from_bmp(buf, len, &out), LIGHT_E_UNSUPPORTED);

    len = build_tiny_bmp(buf, 1);
    put32(buf + 10, 20);                  /* data inside the header */
    CHECK_EQ(light_from_bmp(buf, len, &out), LIGHT_E_UNSUPPORTED);

    len = build_tiny_bmp(buf, 1);
    put32(buf + 10, 1000);                /* data past end of file */
    CHECK_EQ(light_from_bmp(buf, len, &out), LIGHT_E_TRUNCATED);

    len = build_tiny_bmp(buf, 1);
    put32(buf + 18, 100000);              /* huge claimed size */
    put32(buf + 22, 100000);
    CHECK_EQ(light_from_bmp(buf, len, &out), LIGHT_E_TOO_BIG);

    len = build_tiny_bmp(buf, 1);
    put32(buf + 18, 1000);                /* plausible size, no data */
    CHECK_EQ(light_from_bmp(buf, len, &out), LIGHT_E_TRUNCATED);

    CHECK_EQ(light_from_bmp(buf, 1, &out), LIGHT_E_TRUNCATED);
    CHECK_EQ(light_from_bmp(buf, 53, &out), LIGHT_E_TRUNCATED);
    CHECK_EQ(light_from_bmp(NULL, 10, &out), LIGHT_E_ARG);
}

/* Exact value lands on x.xxxx5 (half a unit) -> must round up, once. */
static void test_rounding_fourth_decimal(void)
{
    fixed4_t out = -1;

    /* 51 * 200 / (400 * 51) = 0.5 units = 0.00005 -> 0.0001 */
    CHECK_EQ(light_level_from_sum(51, 400, &out), LIGHT_OK);
    CHECK_EQ(out, 1);
    /* 0.49 units -> 0.0000 */
    CHECK_EQ(light_level_from_sum(50, 400, &out), LIGHT_OK);
    CHECK_EQ(out, 0);
    /* 1.5 units -> 0.0002 (half up, not banker's) */
    CHECK_EQ(light_level_from_sum(153, 400, &out), LIGHT_OK);
    CHECK_EQ(out, 2);
    /* 2.5 units -> 0.0003 */
    CHECK_EQ(light_level_from_sum(255, 400, &out), LIGHT_OK);
    CHECK_EQ(out, 3);
    /* one blue-ish pixel: 114 * 200 / 51 = 447.06 -> 447 */
    CHECK_EQ(light_level_from_sum(114, 1, &out), LIGHT_OK);
    CHECK_EQ(out, 447);
    /* just under full white */
    CHECK_EQ(light_level_from_sum(255000 - 1, 1, &out), LIGHT_OK);
    CHECK_EQ(out, 999996);  /* 999996.08 */
}

static void test_overflow_guard(void)
{
    fixed4_t out = -1;
    /* Biggest allowed all-white image must still be exactly 100.0000. */
    CHECK_EQ(light_level_from_sum(LIGHT_MAX_PIXELS * LIGHT_LUMA_MAX,
                                  LIGHT_MAX_PIXELS, &out), LIGHT_OK);
    CHECK_EQ(out, 1000000);
    CHECK_EQ(light_level_from_sum(0, LIGHT_MAX_PIXELS + 1, &out), LIGHT_E_TOO_BIG);
    CHECK_EQ(light_level_from_sum(255001, 1, &out), LIGHT_E_ARG);
    CHECK_EQ(light_level_from_sum(0, 0, &out), LIGHT_E_ARG);

    uint8_t px[3] = { 0 };
    CHECK_EQ(light_from_bgr24(px, 1u << 14, 1u << 13, 1u << 16, &out), LIGHT_E_TOO_BIG);
    CHECK_EQ(light_from_bgr24(px, 2, 1, 3, &out), LIGHT_E_ARG);  /* stride < width*3 */
}

/* --- fixed-point parse / print ----------------------------------------- */

static void test_parse_good(void)
{
    static const struct { const char *in; fixed4_t want; } cases[] = {
        { "42.5", 425000 },     { "0", 0 },             { "100", 1000000 },
        { "100.0000", 1000000 }, { " 7.0001\n", 70001 }, { "0.0001", 1 },
        { "050.25", 502500 },   { "99.9999", 999999 },  { "12.34\r\n", 123400 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        fixed4_t out = -1;
        CHECK_EQ(light_parse_level(cases[i].in, &out), 0);
        CHECK_EQ(out, cases[i].want);
    }
}

static void test_parse_bad(void)
{
    static const char *const bad[] = {
        "", "\n", "   ", "-1", "-0", "+5", "abc", "4x", "1.", ".5", "1.23456",
        "100.0001", "101", "1e2", "99999999999999999999999", "1..2", "1 2",
        "0x10", "nan", "inf", "5,5", "42.5abc",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        fixed4_t out = 12345;
        int rc = light_parse_level(bad[i], &out);
        if (rc == 0)
            fprintf(stderr, "accepted bad input \"%s\"\n", bad[i]);
        CHECK_EQ(rc, -1);
        CHECK_EQ(out, 12345);  /* untouched on failure */
    }
    fixed4_t out;
    CHECK_EQ(light_parse_level(NULL, &out), -1);
}

static void test_format(void)
{
    char buf[FIXED4_STR_LEN];
    CHECK_STR(fixed4_format(0, buf, sizeof buf), "0.0000");
    CHECK_STR(fixed4_format(1, buf, sizeof buf), "0.0001");
    CHECK_STR(fixed4_format(425000, buf, sizeof buf), "42.5000");
    CHECK_STR(fixed4_format(421337, buf, sizeof buf), "42.1337");
    CHECK_STR(fixed4_format(1000000, buf, sizeof buf), "100.0000");
    CHECK_STR(fixed4_format(-5, buf, sizeof buf), "-0.0005");
    CHECK_STR(fixed4_format(INT64_MIN, buf, sizeof buf), "-922337203685477.5808");
}

static void test_round_trip_every_value(void)
{
    char buf[FIXED4_STR_LEN];
    int mismatches = 0;
    for (fixed4_t v = 0; v <= LIGHT_LEVEL_MAX; v++) {
        fixed4_t back = -1;
        if (light_parse_level(fixed4_format(v, buf, sizeof buf), &back) != 0 || back != v)
            mismatches++;
    }
    CHECK_EQ(mismatches, 0);
}

/* --- verdict ------------------------------------------------------------ */

static void test_verdict_edges(void)
{
    const fixed4_t opt = 500000, db = LIGHT_DEADBAND;
    CHECK_EQ(light_verdict(449999, opt, db), VERDICT_MORE);
    CHECK_EQ(light_verdict(450000, opt, db), VERDICT_OK);   /* edge is inclusive */
    CHECK_EQ(light_verdict(450001, opt, db), VERDICT_OK);
    CHECK_EQ(light_verdict(500000, opt, db), VERDICT_OK);
    CHECK_EQ(light_verdict(549999, opt, db), VERDICT_OK);
    CHECK_EQ(light_verdict(550000, opt, db), VERDICT_OK);
    CHECK_EQ(light_verdict(550001, opt, db), VERDICT_LESS);
    CHECK_EQ(light_verdict(0, opt, db), VERDICT_MORE);
    CHECK_EQ(light_verdict(1000000, opt, db), VERDICT_LESS);

    /* Optimal at the extremes of the scale. */
    CHECK_EQ(light_verdict(0, 0, db), VERDICT_OK);
    CHECK_EQ(light_verdict(50001, 0, db), VERDICT_LESS);
    CHECK_EQ(light_verdict(1000000, 1000000, db), VERDICT_OK);
    CHECK_EQ(light_verdict(949999, 1000000, db), VERDICT_MORE);

    /* Zero deadband: only exact equality is OK. */
    CHECK_EQ(light_verdict(500000, opt, 0), VERDICT_OK);
    CHECK_EQ(light_verdict(499999, opt, 0), VERDICT_MORE);
    CHECK_EQ(light_verdict(500001, opt, 0), VERDICT_LESS);

    CHECK_STR(light_verdict_str(VERDICT_MORE), "NEEDS MORE LIGHT");
    CHECK_STR(light_verdict_str(VERDICT_LESS), "NEEDS LESS LIGHT");
    CHECK_STR(light_verdict_str(VERDICT_OK), "LIGHT OK");
}

/* --- running average ---------------------------------------------------- */

static void test_running_average(void)
{
    struct light_stats st;
    fixed4_t avg = -1;

    light_stats_init(&st);
    CHECK_EQ(light_stats_avg(&st, &avg), -1);  /* empty */

    /* The classic float killer: add 0.1 a million times. */
    int add_failures = 0;
    for (int i = 0; i < 1000000; i++)
        add_failures += light_stats_add(&st, 1000) != 0;
    CHECK_EQ(add_failures, 0);
    CHECK_EQ(st.sum, 1000000000LL);
    CHECK_EQ(light_stats_avg(&st, &avg), 0);
    CHECK_EQ(avg, 1000);

    light_stats_init(&st);
    for (int i = 0; i < 1000000; i++)
        light_stats_add(&st, 333333);
    light_stats_avg(&st, &avg);
    CHECK_EQ(avg, 333333);

    light_stats_init(&st);
    for (int i = 0; i < 1000000; i++)
        light_stats_add(&st, (i & 1) ? 1000000 : 0);
    light_stats_avg(&st, &avg);
    CHECK_EQ(avg, 500000);

    /* 1, 2 -> 1.5 -> 2 (half up); 1, 1, 2 -> 1.33 -> 1; 1, 2, 2 -> 1.67 -> 2 */
    light_stats_init(&st);
    light_stats_add(&st, 1);
    light_stats_add(&st, 2);
    light_stats_avg(&st, &avg);
    CHECK_EQ(avg, 2);
    light_stats_add(&st, 1);
    light_stats_avg(&st, &avg);
    CHECK_EQ(avg, 1);

    light_stats_init(&st);
    for (int i = 0; i < 300000; i++) {
        light_stats_add(&st, 1);
        light_stats_add(&st, 2);
        light_stats_add(&st, 2);
    }
    light_stats_avg(&st, &avg);
    CHECK_EQ(avg, 2);
    CHECK_EQ(st.count, 900000);

    /* Out-of-range readings are refused and don't disturb history. */
    CHECK_EQ(light_stats_add(&st, -1), -1);
    CHECK_EQ(light_stats_add(&st, 1000001), -1);
    CHECK_EQ(st.count, 900000);
}

int main(void)
{
    test_uniform_images();
    test_tiny_bmp_with_padding();
    test_bmp_rejects();
    test_rounding_fourth_decimal();
    test_overflow_guard();
    test_parse_good();
    test_parse_bad();
    test_format();
    test_round_trip_every_value();
    test_verdict_edges();
    test_running_average();
    return test_report("test_light");
}
