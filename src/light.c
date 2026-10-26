#include "light.h"

#include <ctype.h>
#include <stdio.h>

/*
 * level = luma_sum / (pixels * LUMA_MAX) * LEVEL_MAX
 *       = luma_sum * 200 / (pixels * 51)          (1e6 / 255000 = 200 / 51)
 * Rounded half up: (2 * num + den) / (2 * den)
 *       = (400 * luma_sum + 51 * pixels) / (102 * pixels).
 * Worst case numerator is pixels * (400 * LUMA_MAX + 51); prove it fits.
 */
static_assert(LIGHT_MAX_PIXELS <= UINT64_MAX / (400 * LIGHT_LUMA_MAX + 51),
              "light level numerator could overflow uint64_t");

/* light_stats_avg computes 2 * sum + count; cap count so that fits. */
#define STATS_MAX_COUNT (INT64_MAX / (2 * LIGHT_LEVEL_MAX + 1))

const char *light_strerror(int status)
{
    switch (status) {
    case LIGHT_OK:            return "ok";
    case LIGHT_E_ARG:         return "invalid argument";
    case LIGHT_E_TOO_BIG:     return "image too large";
    case LIGHT_E_TRUNCATED:   return "image data truncated";
    case LIGHT_E_BAD_MAGIC:   return "not a BMP file";
    case LIGHT_E_UNSUPPORTED: return "unsupported BMP (need uncompressed 24-bit)";
    default:                  return "unknown error";
    }
}

int light_level_from_sum(uint64_t luma_sum, uint64_t pixels, fixed4_t *out)
{
    if (!out || pixels == 0)
        return LIGHT_E_ARG;
    if (pixels > LIGHT_MAX_PIXELS)
        return LIGHT_E_TOO_BIG;
    if (luma_sum > pixels * LIGHT_LUMA_MAX)
        return LIGHT_E_ARG;

    uint64_t num = 400 * luma_sum + 51 * pixels;
    uint64_t den = 102 * pixels;
    *out = (fixed4_t)(num / den);
    return LIGHT_OK;
}

int light_from_bgr24(const uint8_t *px, uint32_t width, uint32_t height,
                     size_t stride, fixed4_t *out)
{
    if (!px || !out || width == 0 || height == 0 || stride < (size_t)width * 3)
        return LIGHT_E_ARG;

    uint64_t pixels = (uint64_t)width * height;
    if (pixels > LIGHT_MAX_PIXELS)
        return LIGHT_E_TOO_BIG;

    uint64_t sum = 0;
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *row = px + (size_t)y * stride;
        for (uint32_t x = 0; x < width; x++) {
            const uint8_t *p = row + (size_t)x * 3;  /* B, G, R */
            sum += 114u * p[0] + 587u * p[1] + 299u * p[2];
        }
    }
    return light_level_from_sum(sum, pixels, out);
}

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }

#define BMP_FILE_HDR 14
#define BMP_INFO_HDR 40  /* BITMAPINFOHEADER; V4/V5 headers are larger supersets */

int light_from_bmp(const uint8_t *buf, size_t len, fixed4_t *out)
{
    if (!buf || !out)
        return LIGHT_E_ARG;
    if (len < 2)
        return LIGHT_E_TRUNCATED;
    if (buf[0] != 'B' || buf[1] != 'M')
        return LIGHT_E_BAD_MAGIC;
    if (len < BMP_FILE_HDR + BMP_INFO_HDR)
        return LIGHT_E_TRUNCATED;

    uint32_t data_off = rd32(buf + 10);
    uint32_t info_size = rd32(buf + 14);
    int32_t width = (int32_t)rd32(buf + 18);
    int32_t height = (int32_t)rd32(buf + 22);  /* negative = top-down rows */
    uint32_t planes = rd16(buf + 26);
    uint32_t bpp = rd16(buf + 28);
    uint32_t compression = rd32(buf + 30);

    if (info_size < BMP_INFO_HDR || planes != 1 || bpp != 24 || compression != 0)
        return LIGHT_E_UNSUPPORTED;
    if (width <= 0 || height == 0 || height == INT32_MIN)
        return LIGHT_E_UNSUPPORTED;

    uint64_t w = (uint64_t)width;
    uint64_t h = height < 0 ? (uint64_t)-(int64_t)height : (uint64_t)height;
    if (w * h > LIGHT_MAX_PIXELS)
        return LIGHT_E_TOO_BIG;

    /* Each row is padded to a multiple of 4 bytes. */
    uint64_t stride = (w * 3 + 3) & ~(uint64_t)3;
    if (data_off < BMP_FILE_HDR + (uint64_t)info_size)
        return LIGHT_E_UNSUPPORTED;
    /* The final row's padding may legitimately be omitted. */
    if ((uint64_t)data_off + stride * (h - 1) + w * 3 > len)
        return LIGHT_E_TRUNCATED;

    /* Row order doesn't change a sum, so top-down vs bottom-up is moot. */
    return light_from_bgr24(buf + data_off, (uint32_t)w, (uint32_t)h,
                            (size_t)stride, out);
}

int light_parse_level(const char *s, fixed4_t *out)
{
    if (!s || !out)
        return -1;
    while (isspace((unsigned char)*s))
        s++;
    if (!isdigit((unsigned char)*s))  /* rejects "", "-1", "+1", ".5" */
        return -1;

    int64_t whole = 0;
    for (; isdigit((unsigned char)*s); s++) {
        whole = whole * 10 + (*s - '0');
        if (whole > LIGHT_LEVEL_MAX / FIXED4_SCALE)
            return -1;
    }

    int64_t frac = 0;
    int digits = 0;
    if (*s == '.') {
        s++;
        for (; isdigit((unsigned char)*s); s++) {
            if (digits == 4)
                return -1;
            frac = frac * 10 + (*s - '0');
            digits++;
        }
        if (digits == 0)
            return -1;
    }
    for (; digits < 4; digits++)
        frac *= 10;

    while (isspace((unsigned char)*s))
        s++;
    if (*s != '\0')
        return -1;

    fixed4_t v = whole * FIXED4_SCALE + frac;
    if (v > LIGHT_LEVEL_MAX)
        return -1;
    *out = v;
    return 0;
}

char *fixed4_format(fixed4_t v, char *buf, size_t len)
{
    if (!buf || len == 0)
        return buf;
    const char *sign = v < 0 ? "-" : "";
    /* Negate in unsigned space so INT64_MIN is safe. */
    uint64_t mag = v < 0 ? -(uint64_t)v : (uint64_t)v;
    snprintf(buf, len, "%s%lld.%04lld", sign,
             (long long)(mag / FIXED4_SCALE), (long long)(mag % FIXED4_SCALE));
    return buf;
}

enum light_verdict light_verdict(fixed4_t avg, fixed4_t optimal,
                                 fixed4_t deadband)
{
    fixed4_t diff = avg - optimal;
    if (diff < -deadband)
        return VERDICT_MORE;
    if (diff > deadband)
        return VERDICT_LESS;
    return VERDICT_OK;
}

const char *light_verdict_str(enum light_verdict v)
{
    switch (v) {
    case VERDICT_MORE: return "NEEDS MORE LIGHT";
    case VERDICT_LESS: return "NEEDS LESS LIGHT";
    case VERDICT_OK:   return "LIGHT OK";
    }
    return "LIGHT OK";
}

void light_stats_init(struct light_stats *st)
{
    st->sum = 0;
    st->count = 0;
}

int light_stats_add(struct light_stats *st, fixed4_t level)
{
    if (!st || level < 0 || level > LIGHT_LEVEL_MAX)
        return -1;
    if (st->count >= STATS_MAX_COUNT)
        return -1;
    st->sum += level;
    st->count++;
    return 0;
}

int light_stats_avg(const struct light_stats *st, fixed4_t *out)
{
    if (!st || !out || st->count == 0)
        return -1;
    *out = (2 * st->sum + st->count) / (2 * st->count);
    return 0;
}
