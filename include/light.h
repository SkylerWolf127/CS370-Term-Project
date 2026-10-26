#ifndef PLANTSENSE_LIGHT_H
#define PLANTSENSE_LIGHT_H

/*
 * Pure light-level math: image buffer -> light level. No I/O.
 *
 * Every light value is a fixed-point integer in units of 0.0001, so
 * 0 == 0.0000 (dark) and 1,000,000 == 100.0000 (saturated white).
 * No floating point anywhere, so no round-off creeps in.
 */

#include <stddef.h>
#include <stdint.h>

typedef int64_t fixed4_t;

#define FIXED4_SCALE    ((fixed4_t)10000)
#define LIGHT_LEVEL_MAX ((fixed4_t)1000000)  /* 100.0000 */
#define LIGHT_DEADBAND  ((fixed4_t)50000)    /* 5.0000: "close enough" band */

/* Luma of one pixel = 299R + 587G + 114B, so white is 255 * 1000. */
#define LIGHT_LUMA_MAX  ((uint64_t)255000)

/* Largest image we accept (64 Mpx). Keeps the rounding math in uint64_t. */
#define LIGHT_MAX_PIXELS ((uint64_t)1 << 26)

/* Big enough for any int64_t as "-ddd.dddd" plus NUL. */
#define FIXED4_STR_LEN 32

enum light_status {
    LIGHT_OK = 0,
    LIGHT_E_ARG,          /* null pointer, zero size, or sum out of range */
    LIGHT_E_TOO_BIG,      /* more than LIGHT_MAX_PIXELS */
    LIGHT_E_TRUNCATED,    /* buffer shorter than the header says */
    LIGHT_E_BAD_MAGIC,    /* not a "BM" bitmap */
    LIGHT_E_UNSUPPORTED,  /* bitmap, but not uncompressed 24-bit */
};

enum light_verdict {
    VERDICT_OK,
    VERDICT_MORE,
    VERDICT_LESS,
};

/* Running history: exact integer sum plus count, never a float average. */
struct light_stats {
    int64_t sum;
    int64_t count;
};

const char *light_strerror(int status);

/* Level from a summed luma over `pixels` pixels, rounded half up once. */
int light_level_from_sum(uint64_t luma_sum, uint64_t pixels, fixed4_t *out);

/* Level from BGR pixel rows that start `stride` bytes apart. */
int light_from_bgr24(const uint8_t *px, uint32_t width, uint32_t height,
                     size_t stride, fixed4_t *out);

/* Level from a whole BMP file in memory. Only uncompressed 24-bit. */
int light_from_bmp(const uint8_t *buf, size_t len, fixed4_t *out);

/* "42.5" -> 425000. Rejects garbage, signs, >4 decimals, and >100. */
int light_parse_level(const char *s, fixed4_t *out);

/* 425000 -> "42.5000". Returns buf. */
char *fixed4_format(fixed4_t v, char *buf, size_t len);

/* |avg - optimal| <= deadband is OK; otherwise more or less light. */
enum light_verdict light_verdict(fixed4_t avg, fixed4_t optimal,
                                 fixed4_t deadband);
const char *light_verdict_str(enum light_verdict v);

void light_stats_init(struct light_stats *st);
int light_stats_add(struct light_stats *st, fixed4_t level);
int light_stats_avg(const struct light_stats *st, fixed4_t *out);

#endif
