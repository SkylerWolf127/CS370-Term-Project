/*
 * I2C soil sensor tests. The driver runs against a scripted fake transport;
 * the real transport is only poked at with ordinary files and pipes.
 * Real hardware is only exercised on the Pi.
 */
#define _POSIX_C_SOURCE 200809L

#include "soil_i2c.h"
#include "test.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define WORK "build/tests/soil_i2c_tmp"

/* ---- Fake transport ---------------------------------------------------- */

/* One seesaw transaction: expected register pair, then what read() gives. */
struct xfer {
    uint8_t base, func;
    int rc;          /* returned by read() instead of data, if nonzero */
    size_t len;
    uint8_t b[4];
};

#define MAX_XFERS 32

struct fake {
    struct xfer x[MAX_XFERS];
    size_t n, pos;
    int bad_order;          /* write/read out of script, wrong register */
    long waited_us;         /* delay since the last register write */
    long min_read_wait_us;  /* smallest write->read delay seen */
    long delays[64];
    size_t ndelays;
};

static struct fake g;

static int fake_write_reg(void *ctx, uint8_t base, uint8_t func)
{
    (void)ctx;
    if (g.pos >= g.n || g.x[g.pos].base != base || g.x[g.pos].func != func)
        g.bad_order++;
    g.waited_us = 0;
    return SOIL_I2C_OK;
}

static int fake_delay_us(void *ctx, long us)
{
    (void)ctx;
    if (g.ndelays < sizeof g.delays / sizeof g.delays[0])
        g.delays[g.ndelays++] = us;
    g.waited_us += us;
    return SOIL_I2C_OK;
}

static int fake_read(void *ctx, uint8_t *buf, size_t n)
{
    (void)ctx;
    if (g.pos >= g.n) {
        g.bad_order++;
        return SOIL_I2C_E_IO;
    }
    if (g.waited_us < g.min_read_wait_us)
        g.min_read_wait_us = g.waited_us;
    const struct xfer *x = &g.x[g.pos++];
    if (x->rc != 0)
        return x->rc;
    if (x->len != n)
        return SOIL_I2C_E_SHORT_READ;
    memcpy(buf, x->b, n);
    return SOIL_I2C_OK;
}

static const soil_i2c_ops_t fake_ops = {
    .write_reg = fake_write_reg,
    .delay_us = fake_delay_us,
    .read = fake_read,
};

static void reset(void)
{
    memset(&g, 0, sizeof g);
    g.min_read_wait_us = 1L << 40;
}

static void push(uint8_t base, uint8_t func, int rc, size_t len,
                 uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3)
{
    if (g.n >= MAX_XFERS) {
        g.bad_order++;
        return;
    }
    g.x[g.n++] = (struct xfer){ base, func, rc, len, { b0, b1, b2, b3 } };
}

static void push_id(uint8_t id) { push(0x00, 0x01, 0, 1, id, 0, 0, 0); }
static void push_raw(unsigned v) { push(0x0F, 0x10, 0, 2, v >> 8, v & 0xFF, 0, 0); }
static void push_raw_rc(int rc) { push(0x0F, 0x10, rc, 2, 0, 0, 0, 0); }
static void push_temp(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    push(0x00, 0x04, 0, 4, a, b, c, d);
}
static void push_temp_rc(int rc) { push(0x00, 0x04, rc, 4, 0, 0, 0, 0); }

/* 21.5 C: 0x00158000 / 65536 = 21.5 */
static void push_temp_ok(void) { push_temp(0x00, 0x15, 0x80, 0x00); }

static const struct soil_i2c_reading UNTOUCHED = { -7, -7, -7, true };

static int untouched(const struct soil_i2c_reading *r)
{
    return r->raw == -7 && r->moisture == -7 && r->temp_x10 == -7 && r->temp_ok;
}

static int run(bool *id_checked, struct soil_i2c_reading *r, char *why, size_t len)
{
    *r = UNTOUCHED;
    return soil_i2c_read(&fake_ops, NULL, id_checked, 200, 2000, r, why, len);
}

/* ---- Tests ------------------------------------------------------------- */

static void test_decode(void)
{
    int32_t raw = -1;
    CHECK_EQ(soil_i2c_decode_moisture((const uint8_t[]){ 0x01, 0x9C }, &raw), SOIL_I2C_OK);
    CHECK_EQ(raw, 412);
    CHECK_EQ(soil_i2c_decode_moisture((const uint8_t[]){ 0x0F, 0xFF }, &raw), SOIL_I2C_OK);
    CHECK_EQ(raw, 4095);
    raw = 42;
    CHECK_EQ(soil_i2c_decode_moisture((const uint8_t[]){ 0x10, 0x00 }, &raw), SOIL_I2C_E_INVALID);
    CHECK_EQ(soil_i2c_decode_moisture((const uint8_t[]){ 0xFF, 0xFF }, &raw), SOIL_I2C_E_INVALID);
    CHECK_EQ(raw, 42);

    /* Masking: bytes that differ only in the top 2 bits decode the same. */
    const uint8_t t[] = { 0x00, 0x15, 0x80, 0x00 };
    CHECK_EQ(soil_i2c_decode_temp_x10(t), 215);
    CHECK_EQ(soil_i2c_decode_temp_x10((const uint8_t[]){ 0x40, 0x15, 0x80, 0x00 }), 215);
    CHECK_EQ(soil_i2c_decode_temp_x10((const uint8_t[]){ 0x80, 0x15, 0x80, 0x00 }), 215);
    CHECK_EQ(soil_i2c_decode_temp_x10((const uint8_t[]){ 0xC0, 0x15, 0x80, 0x00 }), 215);
    CHECK_EQ(soil_i2c_decode_temp_x10((const uint8_t[]){ 0x3F, 0xFF, 0xFF, 0xFF }), 163840);

    /* Rounding: 16384 / 65536 * 10 = 2.5 exactly -> 3; one less -> 2. */
    CHECK_EQ(soil_i2c_decode_temp_x10((const uint8_t[]){ 0, 0, 0x40, 0x00 }), 3);
    CHECK_EQ(soil_i2c_decode_temp_x10((const uint8_t[]){ 0, 0, 0x3F, 0xFF }), 2);
    CHECK_EQ(soil_i2c_decode_temp_x10((const uint8_t[]){ 0, 0, 0, 0 }), 0);
}

static void test_moisture_math(void)
{
    fixed4_t m = -1;
    CHECK_EQ(soil_i2c_moisture(200, 200, 2000, &m), 0);
    CHECK_EQ(m, 0);
    CHECK_EQ(soil_i2c_moisture(0, 200, 2000, &m), 0);
    CHECK_EQ(m, 0);
    CHECK_EQ(soil_i2c_moisture(2000, 200, 2000, &m), 0);
    CHECK_EQ(m, 1000000);
    CHECK_EQ(soil_i2c_moisture(4095, 200, 2000, &m), 0);
    CHECK_EQ(m, 1000000);
    CHECK_EQ(soil_i2c_moisture(1100, 200, 2000, &m), 0);  /* midpoint */
    CHECK_EQ(m, 500000);
    CHECK_EQ(soil_i2c_moisture(412, 200, 2000, &m), 0);   /* 11.77777... */
    CHECK_EQ(m, 117778);

    /* 4th-decimal rounding: 33.33333 down, 66.66667 up, exact .5 up. */
    CHECK_EQ(soil_i2c_moisture(1, 0, 3, &m), 0);
    CHECK_EQ(m, 333333);
    CHECK_EQ(soil_i2c_moisture(2, 0, 3, &m), 0);
    CHECK_EQ(m, 666667);
    CHECK_EQ(soil_i2c_moisture(1, 0, 2000000, &m), 0);    /* 0.00005 */
    CHECK_EQ(m, 1);
    CHECK_EQ(soil_i2c_moisture(3, 0, 2000000, &m), 0);    /* 0.00015 */
    CHECK_EQ(m, 2);
    /* Widest int32 span: no overflow. */
    CHECK_EQ(soil_i2c_moisture(0, INT32_MIN, INT32_MAX, &m), 0);
    CHECK_EQ(m, 500000);

    m = 42;
    CHECK_EQ(soil_i2c_moisture(500, 2000, 2000, &m), -1);  /* wet == dry */
    CHECK_EQ(soil_i2c_moisture(500, 2000, 200, &m), -1);   /* wet < dry */
    CHECK_EQ(m, 42);
}

static void test_verdict(void)
{
    const fixed4_t opt = 500000, db = SOIL_I2C_DEADBAND;
    CHECK_EQ(db, 50000);
    CHECK_STR(soil_i2c_verdict(449999, opt, db), "NEEDS WATER");
    CHECK_STR(soil_i2c_verdict(450000, opt, db), "SOIL OK");
    CHECK_STR(soil_i2c_verdict(450001, opt, db), "SOIL OK");
    CHECK_STR(soil_i2c_verdict(500000, opt, db), "SOIL OK");
    CHECK_STR(soil_i2c_verdict(549999, opt, db), "SOIL OK");
    CHECK_STR(soil_i2c_verdict(550000, opt, db), "SOIL OK");
    CHECK_STR(soil_i2c_verdict(550001, opt, db), "TOO WET");
    CHECK_STR(soil_i2c_verdict(0, 0, db), "SOIL OK");
    CHECK_STR(soil_i2c_verdict(1000000, 1000000, db), "SOIL OK");
}

static void test_full_reading(void)
{
    bool id = false;
    struct soil_i2c_reading r;
    char why[96];

    reset();
    push_id(0x55);
    push_raw(0x019C);
    push_raw(0x019C);
    push_raw(0x019C);
    push_temp_ok();
    CHECK_EQ(run(&id, &r, why, sizeof why), SOIL_I2C_OK);
    CHECK_EQ(r.raw, 412);
    CHECK_EQ(r.moisture, 117778);
    CHECK(r.temp_ok);
    CHECK_EQ(r.temp_x10, 215);
    CHECK(id);
    CHECK_EQ(g.pos, g.n);
    CHECK_EQ(g.bad_order, 0);
    CHECK(g.min_read_wait_us >= 5000);  /* every read waited >= 5 ms */

    /* Hardware ID only on first use. */
    reset();
    push_raw(1100);
    push_raw(1100);
    push_raw(1100);
    push_temp_ok();
    CHECK_EQ(run(&id, &r, why, sizeof why), SOIL_I2C_OK);
    CHECK_EQ(r.moisture, 500000);
    CHECK_EQ(g.bad_order, 0);

    /* Sample gaps were the named constant. */
    int gaps = 0;
    for (size_t i = 0; i < g.ndelays; i++)
        gaps += g.delays[i] == SOIL_I2C_SAMPLE_GAP_MS * 1000L;
    CHECK_EQ(gaps, SOIL_I2C_SAMPLES - 1);
}

static void test_hwid_mismatch(void)
{
    bool id = false;
    struct soil_i2c_reading r;
    char why[96];

    reset();
    push_id(0x56);
    CHECK_EQ(run(&id, &r, why, sizeof why), SOIL_I2C_E_HWID);
    CHECK_STR(why, "unexpected device 0x56 at 0x36");
    CHECK(!id);
    CHECK(untouched(&r));
    CHECK_EQ(g.pos, 1u);  /* didn't go on to read moisture */

    /* Nobody home: the transport's error comes through. */
    reset();
    push(0x00, 0x01, SOIL_I2C_E_NODEV, 1, 0, 0, 0, 0);
    CHECK_EQ(run(&id, &r, why, sizeof why), SOIL_I2C_E_NODEV);
    CHECK_STR(why, "no device at 0x36");
    CHECK(!id);
    CHECK(untouched(&r));
}

static void test_retry(void)
{
    bool id = true;
    struct soil_i2c_reading r;
    char why[96];

    /* 3 bad values then a good one: recovers (1 try + 3 retries). */
    reset();
    push_raw(0xFFFF);
    push_raw(4096);
    push_raw(0xFFFF);
    push_raw(412);
    push_raw(412);
    push_raw(412);
    push_temp_ok();
    CHECK_EQ(run(&id, &r, why, sizeof why), SOIL_I2C_OK);
    CHECK_EQ(r.raw, 412);
    CHECK_EQ(g.bad_order, 0);
    int retry_gaps = 0;
    for (size_t i = 0; i < g.ndelays; i++)
        retry_gaps += g.delays[i] == SOIL_I2C_RETRY_GAP_US;
    CHECK_EQ(retry_gaps, 3);
    CHECK(SOIL_I2C_RETRY_GAP_US >= 1000);

    /* Bad value on a later sample recovers too. */
    reset();
    push_raw(412);
    push_raw(0xFFFF);
    push_raw(413);
    push_raw(414);
    push_temp_ok();
    CHECK_EQ(run(&id, &r, why, sizeof why), SOIL_I2C_OK);
    CHECK_EQ(r.raw, 413);

    /* 4 consecutive bad values: the reading fails, nothing stale. */
    reset();
    push_raw(0xFFFF);
    push_raw(0xFFFF);
    push_raw(0xFFFF);
    push_raw(0xFFFF);
    CHECK_EQ(run(&id, &r, why, sizeof why), SOIL_I2C_E_INVALID);
    CHECK_STR(why, "invalid reading");
    CHECK(untouched(&r));
    CHECK_EQ(g.pos, 4u);  /* stopped after the 4th try */
}

static void test_short_read(void)
{
    bool id = true;
    struct soil_i2c_reading r;
    char why[96];

    reset();
    push(0x0F, 0x10, 0, 1, 0x01, 0, 0, 0);  /* only 1 of 2 bytes */
    CHECK_EQ(run(&id, &r, why, sizeof why), SOIL_I2C_E_SHORT_READ);
    CHECK_STR(why, "short read");
    CHECK(untouched(&r));

    /* Short read of the hardware ID is not a match. */
    id = false;
    reset();
    push(0x00, 0x01, 0, 0, 0, 0, 0, 0);
    CHECK_EQ(run(&id, &r, why, sizeof why), SOIL_I2C_E_SHORT_READ);
    CHECK(!id);
}

static void test_median(void)
{
    static const unsigned perms[][3] = {
        { 100, 200, 300 }, { 100, 300, 200 }, { 200, 100, 300 },
        { 200, 300, 100 }, { 300, 100, 200 }, { 300, 200, 100 },
    };
    static_assert(SOIL_I2C_SAMPLES == 3, "tests below script exactly 3 samples");
    bool id = true;
    struct soil_i2c_reading r;
    char why[96];

    for (size_t i = 0; i < sizeof perms / sizeof perms[0]; i++) {
        reset();
        for (int k = 0; k < 3; k++)
            push_raw(perms[i][k]);
        push_temp_ok();
        CHECK_EQ(run(&id, &r, why, sizeof why), SOIL_I2C_OK);
        CHECK_EQ(r.raw, 200);
    }

    /* Duplicates. */
    static const unsigned dups[][4] = {
        { 5, 5, 9, 5 }, { 9, 5, 9, 9 }, { 7, 7, 7, 7 }, { 0, 4095, 4095, 4095 },
    };
    for (size_t i = 0; i < sizeof dups / sizeof dups[0]; i++) {
        reset();
        for (int k = 0; k < 3; k++)
            push_raw(dups[i][k]);
        push_temp_ok();
        CHECK_EQ(run(&id, &r, why, sizeof why), SOIL_I2C_OK);
        CHECK_EQ(r.raw, (int32_t)dups[i][3]);
    }

    /* An error on the first, middle, or last sample fails the reading. */
    for (int bad = 0; bad < 3; bad++) {
        reset();
        for (int k = 0; k < 3; k++) {
            if (k == bad)
                push_raw_rc(SOIL_I2C_E_IO);
            else
                push_raw(1000);
        }
        push_temp_ok();
        CHECK_EQ(run(&id, &r, why, sizeof why), SOIL_I2C_E_IO);
        CHECK(untouched(&r));
        CHECK_EQ(g.pos, (size_t)bad + 1);  /* stopped at the failure */
    }
}

static void test_temp_failure_alone(void)
{
    bool id = true;
    struct soil_i2c_reading r;
    char why[96];

    reset();
    push_raw(1100);
    push_raw(1100);
    push_raw(1100);
    push_temp_rc(SOIL_I2C_E_IO);
    CHECK_EQ(run(&id, &r, why, sizeof why), SOIL_I2C_OK);
    CHECK_EQ(r.raw, 1100);
    CHECK_EQ(r.moisture, 500000);
    CHECK(!r.temp_ok);

    reset();
    push_raw(1100);
    push_raw(1100);
    push_raw(1100);
    push(0x00, 0x04, 0, 2, 0, 0, 0, 0);  /* short temperature read */
    CHECK_EQ(run(&id, &r, why, sizeof why), SOIL_I2C_OK);
    CHECK(!r.temp_ok);
}

static void test_bad_args(void)
{
    bool id = true;
    struct soil_i2c_reading r = UNTOUCHED;
    char why[96];

    reset();
    CHECK_EQ(soil_i2c_read(&fake_ops, NULL, &id, 2000, 2000, &r, why, sizeof why),
             SOIL_I2C_E_CAL);
    CHECK_EQ(soil_i2c_read(&fake_ops, NULL, &id, 2000, 200, &r, why, sizeof why),
             SOIL_I2C_E_CAL);
    CHECK_EQ(soil_i2c_read(NULL, NULL, &id, 200, 2000, &r, why, sizeof why),
             SOIL_I2C_E_ARG);
    CHECK_EQ(soil_i2c_read(&fake_ops, NULL, NULL, 200, 2000, &r, why, sizeof why),
             SOIL_I2C_E_ARG);
    CHECK_EQ(soil_i2c_read(&fake_ops, NULL, &id, 200, 2000, NULL, why, sizeof why),
             SOIL_I2C_E_ARG);
    CHECK(untouched(&r));
    CHECK_EQ(g.pos, 0u);  /* never touched the bus */
}

/* Lowest free fd: if it's unchanged, nothing leaked. */
static int next_fd(void)
{
    int fd = open("/dev/null", O_RDONLY);
    if (fd >= 0)
        close(fd);
    return fd;
}

static void test_real_transport(void)
{
    struct soil_i2c_bus bus;
    char why[160];
    int before = next_fd();

    CHECK_EQ(soil_i2c_bus_open(&bus, WORK "/no-such-bus", 0x36, why, sizeof why),
             SOIL_I2C_E_OPEN);
    CHECK_STR(why, "open " WORK "/no-such-bus failed: no such device"
                   " (enable I2C in raspi-config)");
    CHECK_EQ(bus.fd, -1);

    /* A regular file opens, but isn't an I2C adapter: ioctl fails, fd closed. */
    FILE *f = fopen(WORK "/plain", "w");
    CHECK(f != NULL);
    if (f)
        fclose(f);
    CHECK_EQ(soil_i2c_bus_open(&bus, WORK "/plain", 0x36, why, sizeof why),
             SOIL_I2C_E_OPEN);
    CHECK(strncmp(why, "select 0x36 on " WORK "/plain failed: ", 30) == 0);
    CHECK_EQ(bus.fd, -1);

    /* Permission denied names the fix (skipped as root: root ignores modes). */
    if (geteuid() != 0) {
        chmod(WORK "/plain", 0);
        CHECK_EQ(soil_i2c_bus_open(&bus, WORK "/plain", 0x36, why, sizeof why),
                 SOIL_I2C_E_OPEN);
        CHECK_STR(why, "open " WORK "/plain failed: permission denied"
                       " (add the user to the i2c group)");
        chmod(WORK "/plain", 0644);
    }
    CHECK_EQ(next_fd(), before);

    /* write/read/delay plumbing over a pipe instead of a bus. */
    int p[2];
    CHECK_EQ(pipe(p), 0);
    struct soil_i2c_bus wr = { p[1] }, rd = { p[0] };
    CHECK_EQ(soil_i2c_linux_ops.write_reg(&wr, 0x0F, 0x10), SOIL_I2C_OK);
    uint8_t b[2] = { 0, 0 };
    CHECK_EQ(soil_i2c_linux_ops.read(&rd, b, 2), SOIL_I2C_OK);
    CHECK_EQ(b[0], 0x0F);
    CHECK_EQ(b[1], 0x10);
    CHECK_EQ(soil_i2c_linux_ops.write_reg(&wr, 0x01, 0x9C), SOIL_I2C_OK);
    uint8_t big[4];
    CHECK_EQ(soil_i2c_linux_ops.read(&rd, big, 4), SOIL_I2C_E_SHORT_READ);
    CHECK_EQ(soil_i2c_linux_ops.delay_us(NULL, 1000), SOIL_I2C_OK);
    soil_i2c_bus_close(&wr);
    soil_i2c_bus_close(&rd);
    CHECK_EQ(wr.fd, -1);
    soil_i2c_bus_close(&wr);  /* idempotent */

    /* Bus error on a closed fd is an I/O error, not a crash. */
    CHECK_EQ(soil_i2c_linux_ops.read(&rd, b, 2), SOIL_I2C_E_IO);
    CHECK_EQ(next_fd(), before);
}

static void test_strerror(void)
{
    CHECK_STR(soil_i2c_strerror(SOIL_I2C_E_NODEV), "no device at 0x36");
    CHECK_STR(soil_i2c_strerror(SOIL_I2C_E_HWID), "unexpected hardware id");
    CHECK_STR(soil_i2c_strerror(SOIL_I2C_E_SHORT_READ), "short read");
    CHECK_STR(soil_i2c_strerror(SOIL_I2C_E_INVALID), "invalid reading");
    CHECK_STR(soil_i2c_strerror(SOIL_I2C_E_ABORTED), "interrupted");
    CHECK_STR(soil_i2c_strerror(999), "read failed");
}

int main(void)
{
    mkdir("build", 0755);
    mkdir("build/tests", 0755);
    mkdir(WORK, 0755);

    test_decode();
    test_moisture_math();
    test_verdict();
    test_full_reading();
    test_hwid_mismatch();
    test_retry();
    test_short_read();
    test_median();
    test_temp_failure_alone();
    test_bad_args();
    test_real_transport();
    test_strerror();
    return test_report("test_soil_i2c");
}
