#define _POSIX_C_SOURCE 200809L

#include "soil_i2c.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "sig.h"

/* ---- Real transport ---------------------------------------------------- */

static int errno_status(int err)
{
    /* The Pi's controller reports a NACKed address as EREMOTEIO. */
    return err == EREMOTEIO || err == ENXIO ? SOIL_I2C_E_NODEV : SOIL_I2C_E_IO;
}

static int linux_write_reg(void *ctx, uint8_t base, uint8_t func)
{
    const struct soil_i2c_bus *bus = ctx;
    const uint8_t cmd[2] = { base, func };
    for (;;) {
        ssize_t n = write(bus->fd, cmd, sizeof cmd);
        if (n == (ssize_t)sizeof cmd)
            return SOIL_I2C_OK;
        if (n >= 0)
            return SOIL_I2C_E_SHORT_WRITE;
        if (errno != EINTR)
            return errno_status(errno);
        if (sig_pending())
            return SOIL_I2C_E_ABORTED;
    }
}

static int linux_read(void *ctx, uint8_t *buf, size_t n)
{
    const struct soil_i2c_bus *bus = ctx;
    for (;;) {
        /* One read() is one I2C transaction: a short one can't be resumed. */
        ssize_t got = read(bus->fd, buf, n);
        if (got == (ssize_t)n)
            return SOIL_I2C_OK;
        if (got >= 0)
            return SOIL_I2C_E_SHORT_READ;
        if (errno != EINTR)
            return errno_status(errno);
        if (sig_pending())
            return SOIL_I2C_E_ABORTED;
    }
}

static int linux_delay_us(void *ctx, long us)
{
    (void)ctx;
    struct timespec req = { us / 1000000, (us % 1000000) * 1000 }, rem;
    for (;;) {
        if (sig_pending())
            return SOIL_I2C_E_ABORTED;
        if (nanosleep(&req, &rem) == 0)
            return SOIL_I2C_OK;
        if (errno != EINTR)
            return SOIL_I2C_E_IO;
        req = rem;
    }
}

const soil_i2c_ops_t soil_i2c_linux_ops = {
    .write_reg = linux_write_reg,
    .delay_us = linux_delay_us,
    .read = linux_read,
};

int soil_i2c_bus_open(struct soil_i2c_bus *bus, const char *path, int addr,
                      char *why, size_t why_len)
{
    bus->fd = open(path, O_RDWR | O_CLOEXEC);
    if (bus->fd < 0) {
        int err = errno;
        if (err == EACCES || err == EPERM)
            snprintf(why, why_len, "open %s failed: permission denied"
                     " (add the user to the i2c group)", path);
        else if (err == ENOENT)
            snprintf(why, why_len, "open %s failed: no such device"
                     " (enable I2C in raspi-config)", path);
        else
            snprintf(why, why_len, "open %s failed: %s", path, strerror(err));
        return SOIL_I2C_E_OPEN;
    }
    if (ioctl(bus->fd, I2C_SLAVE, (unsigned long)addr) < 0) {
        snprintf(why, why_len, "select 0x%02x on %s failed: %s", addr, path,
                 strerror(errno));
        soil_i2c_bus_close(bus);
        return SOIL_I2C_E_OPEN;
    }
    return SOIL_I2C_OK;
}

void soil_i2c_bus_close(struct soil_i2c_bus *bus)
{
    if (bus->fd >= 0)
        close(bus->fd);  /* nothing useful to do if close fails on a char dev */
    bus->fd = -1;
}

/* ---- Pure decoding and math -------------------------------------------- */

int soil_i2c_decode_moisture(const uint8_t b[2], int32_t *raw_out)
{
    int32_t v = (int32_t)b[0] << 8 | b[1];
    if (v > SOIL_I2C_RAW_MAX)
        return SOIL_I2C_E_INVALID;
    *raw_out = v;
    return SOIL_I2C_OK;
}

int32_t soil_i2c_decode_temp_x10(const uint8_t b[4])
{
    uint64_t v = (uint64_t)(b[0] & SEESAW_TEMP_TOP_MASK) << 24
               | (uint64_t)b[1] << 16 | (uint64_t)b[2] << 8 | b[3];
    /* v < 2^30, so v * 10 can't overflow; result < 2^24 fits int32. */
    return (int32_t)((v * 10 + 32768) / 65536);
}

int soil_i2c_moisture(int32_t raw, int32_t dry_raw, int32_t wet_raw, fixed4_t *out)
{
    if (!out || wet_raw <= dry_raw)
        return -1;
    if (raw <= dry_raw) {
        *out = 0;
        return 0;
    }
    if (raw >= wet_raw) {
        *out = LIGHT_LEVEL_MAX;
        return 0;
    }
    int64_t num = (int64_t)raw - dry_raw;   /* 0 < num < span */
    int64_t span = (int64_t)wet_raw - dry_raw;
    /* Both < 2^32, so num * 2e6 + span < 2^53: no int64 overflow. */
    static_assert(INT64_MAX / (2 * LIGHT_LEVEL_MAX) > 2 * (int64_t)UINT32_MAX,
                  "moisture multiply could overflow int64");
    *out = (num * 2 * LIGHT_LEVEL_MAX + span) / (2 * span);
    return 0;
}

const char *soil_i2c_verdict(fixed4_t moisture, fixed4_t optimal, fixed4_t deadband)
{
    /* Same band test as light: below the band means "needs more". */
    switch (light_verdict(moisture, optimal, deadband)) {
    case VERDICT_MORE: return "NEEDS WATER";
    case VERDICT_LESS: return "TOO WET";
    case VERDICT_OK:   break;
    }
    return "SOIL OK";
}

const char *soil_i2c_strerror(int status)
{
    switch (status) {
    case SOIL_I2C_OK:            return "ok";
    case SOIL_I2C_E_ARG:         return "bad argument";
    case SOIL_I2C_E_CAL:         return "bad calibration (wet <= dry)";
    case SOIL_I2C_E_OPEN:        return "open failed";
    case SOIL_I2C_E_NODEV:       return "no device at 0x36";
    case SOIL_I2C_E_HWID:        return "unexpected hardware id";
    case SOIL_I2C_E_SHORT_READ:  return "short read";
    case SOIL_I2C_E_SHORT_WRITE: return "short write";
    case SOIL_I2C_E_INVALID:     return "invalid reading";
    case SOIL_I2C_E_IO:          return "i2c I/O error";
    case SOIL_I2C_E_ABORTED:     return "interrupted";
    default:                     return "read failed";
    }
}
static_assert(SOIL_I2C_ADDR == 0x36, "update the \"no device at\" message");

/* ---- Driver ------------------------------------------------------------ */

static int seesaw_read(const soil_i2c_ops_t *ops, void *ctx, uint8_t base,
                       uint8_t func, uint8_t *buf, size_t n)
{
    int rc = ops->write_reg(ctx, base, func);
    if (rc == SOIL_I2C_OK)
        rc = ops->delay_us(ctx, SOIL_I2C_READ_DELAY_US);
    if (rc == SOIL_I2C_OK)
        rc = ops->read(ctx, buf, n);
    return rc;
}

/* One sample; an out-of-range value is retried, a bus error is not. */
static int read_sample(const soil_i2c_ops_t *ops, void *ctx, int32_t *raw_out)
{
    for (int attempt = 0; attempt <= SOIL_I2C_RETRIES; attempt++) {
        if (attempt > 0) {
            int rc = ops->delay_us(ctx, SOIL_I2C_RETRY_GAP_US);
            if (rc != SOIL_I2C_OK)
                return rc;
        }
        uint8_t b[2];
        int rc = seesaw_read(ops, ctx, SEESAW_TOUCH_BASE, SEESAW_TOUCH_CH0, b, sizeof b);
        if (rc != SOIL_I2C_OK)
            return rc;
        if (soil_i2c_decode_moisture(b, raw_out) == SOIL_I2C_OK)
            return SOIL_I2C_OK;
    }
    return SOIL_I2C_E_INVALID;
}

static int fail(int rc, char *why, size_t why_len)
{
    snprintf(why, why_len, "%s", soil_i2c_strerror(rc));
    return rc;
}

int soil_i2c_read(const soil_i2c_ops_t *ops, void *ctx, bool *id_checked,
                  int32_t dry_raw, int32_t wet_raw,
                  struct soil_i2c_reading *out, char *why, size_t why_len)
{
    if (!ops || !ops->write_reg || !ops->delay_us || !ops->read || !id_checked || !out)
        return fail(SOIL_I2C_E_ARG, why, why_len);
    if (wet_raw <= dry_raw)
        return fail(SOIL_I2C_E_CAL, why, why_len);

    if (!*id_checked) {
        uint8_t id;
        int rc = seesaw_read(ops, ctx, SEESAW_STATUS_BASE, SEESAW_STATUS_HW_ID, &id, 1);
        if (rc != SOIL_I2C_OK)
            return fail(rc, why, why_len);
        if (id != SEESAW_HW_ID_EXPECTED) {
            snprintf(why, why_len, "unexpected device 0x%02x at 0x%02x", id, SOIL_I2C_ADDR);
            return SOIL_I2C_E_HWID;
        }
        *id_checked = true;
    }

    int32_t s[SOIL_I2C_SAMPLES];
    for (int i = 0; i < SOIL_I2C_SAMPLES; i++) {
        int rc = SOIL_I2C_OK;
        if (i > 0)
            rc = ops->delay_us(ctx, SOIL_I2C_SAMPLE_GAP_MS * 1000L);
        if (rc == SOIL_I2C_OK)
            rc = read_sample(ops, ctx, &s[i]);
        if (rc != SOIL_I2C_OK)
            return fail(rc, why, why_len);
    }
    /* Insertion sort; the middle element is the median. */
    for (int i = 1; i < SOIL_I2C_SAMPLES; i++)
        for (int j = i; j > 0 && s[j - 1] > s[j]; j--) {
            int32_t t = s[j];
            s[j] = s[j - 1];
            s[j - 1] = t;
        }

    struct soil_i2c_reading r = { .raw = s[SOIL_I2C_SAMPLES / 2] };
    soil_i2c_moisture(r.raw, dry_raw, wet_raw, &r.moisture);  /* cal checked above */

    uint8_t t[4];
    if (seesaw_read(ops, ctx, SEESAW_STATUS_BASE, SEESAW_STATUS_TEMP, t, sizeof t) == SOIL_I2C_OK) {
        r.temp_x10 = soil_i2c_decode_temp_x10(t);
        r.temp_ok = true;
    }
    *out = r;
    return SOIL_I2C_OK;
}

int soil_i2c_measure(bool *id_checked, int32_t dry_raw, int32_t wet_raw,
                     struct soil_i2c_reading *out, char *why, size_t why_len)
{
    struct soil_i2c_bus bus;
    int rc = soil_i2c_bus_open(&bus, SOIL_I2C_BUS, SOIL_I2C_ADDR, why, why_len);
    if (rc != SOIL_I2C_OK)
        return rc;
    rc = soil_i2c_read(&soil_i2c_linux_ops, &bus, id_checked, dry_raw, wet_raw,
                       out, why, why_len);
    soil_i2c_bus_close(&bus);
    return rc;
}
