/*
 * Soil settings in plantsense.conf, and the I2C soil tail of a log line.
 * The legacy (pre-soil) config/log behaviour is covered, unmodified, by
 * tests/test_config_log.c.
 */
#define _POSIX_C_SOURCE 200809L

#include "config.h"
#include "log.h"
#include "test.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define WORK "build/tests/soil_config_tmp"
#define CONF WORK "/plantsense.conf"

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void read_file(const char *path, char *buf, size_t len)
{
    buf[0] = '\0';
    FILE *f = fopen(path, "r");
    CHECK(f != NULL);
    if (f) {
        size_t n = fread(buf, 1, len - 1, f);
        buf[n] = '\0';
        fclose(f);
    }
}

static const struct soil_config SOIL_KEEP = { SOIL_BACKEND_GPIO, 77, 78, 79 };

static int soil_kept(const struct soil_config *s)
{
    return s->backend == SOIL_BACKEND_GPIO && s->dry_raw == 77 && s->wet_raw == 78
        && s->optimal == 79;
}

static int soil_defaults(const struct soil_config *s)
{
    return s->backend == SOIL_BACKEND_I2C && s->dry_raw == 200 && s->wet_raw == 2000
        && s->optimal == 500000;
}

static void test_parsers(void)
{
    int32_t raw = -1;
    CHECK_EQ(config_parse_soil_raw("0", &raw), 0);
    CHECK_EQ(raw, 0);
    CHECK_EQ(config_parse_soil_raw(" 4095\n", &raw), 0);
    CHECK_EQ(raw, 4095);
    static const char *const bad_raw[] = {
        "", "-1", "+1", "4096", "12a", "1 2", "1.5", "0x10", "99999999999999999999",
    };
    for (size_t i = 0; i < sizeof bad_raw / sizeof bad_raw[0]; i++) {
        raw = 42;
        CHECK_EQ(config_parse_soil_raw(bad_raw[i], &raw), -1);
        CHECK_EQ(raw, 42);
    }

    enum soil_backend b = SOIL_BACKEND_GPIO;
    CHECK_EQ(config_parse_backend(" i2c\n", &b), 0);
    CHECK_EQ(b, SOIL_BACKEND_I2C);
    CHECK_EQ(config_parse_backend("gpio", &b), 0);
    CHECK_EQ(b, SOIL_BACKEND_GPIO);
    static const char *const bad_b[] = { "", "I2C", "i2c2", "gp io", "spi", "i2", "gpioo" };
    for (size_t i = 0; i < sizeof bad_b / sizeof bad_b[0]; i++) {
        b = (enum soil_backend)42;
        CHECK_EQ(config_parse_backend(bad_b[i], &b), -1);
        CHECK_EQ(b, 42);
    }
}

static void test_load_all(void)
{
    struct config cfg = { .interval_s = 1, .optimal_light = 1 };
    struct soil_config soil = SOIL_KEEP;
    char why[128];

    /* Missing file: everything defaults. */
    unlink(CONF);
    CHECK_EQ(config_load_all(CONF, &cfg, &soil, why, sizeof why), CONFIG_OK);
    CHECK(soil_defaults(&soil));

    /* Soil keys missing: defaults for soil, file values for the rest. */
    soil = SOIL_KEEP;
    write_file(CONF, "interval_seconds=90\noptimal_light=12.5\n");
    CHECK_EQ(config_load_all(CONF, &cfg, &soil, why, sizeof why), CONFIG_OK);
    CHECK_EQ(cfg.interval_s, 90);
    CHECK_EQ(cfg.optimal_light, 125000);
    CHECK(soil_defaults(&soil));

    /* New keys parse. */
    write_file(CONF, "soil_backend = gpio\n soil_dry_raw=300\nsoil_wet_raw= 1500 \n"
                     "soil_optimal=42.5\n");
    CHECK_EQ(config_load_all(CONF, &cfg, &soil, why, sizeof why), CONFIG_OK);
    CHECK_EQ(soil.backend, SOIL_BACKEND_GPIO);
    CHECK_EQ(soil.dry_raw, 300);
    CHECK_EQ(soil.wet_raw, 1500);
    CHECK_EQ(soil.optimal, 425000);

    /* Only one of the pair given: the other stays at its default. */
    write_file(CONF, "soil_wet_raw=1015\n");
    CHECK_EQ(config_load_all(CONF, &cfg, &soil, why, sizeof why), CONFIG_OK);
    CHECK_EQ(soil.dry_raw, 200);
    CHECK_EQ(soil.wet_raw, 1015);

    static const char *const bad[] = {
        "soil_backend=spi\n",
        "soil_backend=\n",
        "soil_backend=I2C\n",
        "soil_dry_raw=-1\n",
        "soil_dry_raw=abc\n",
        "soil_wet_raw=4096\n",
        "soil_wet_raw=\n",
        "soil_optimal=101\n",
        "soil_optimal=-1\n",
        "soil_optimal=50.00001\n",
        "soil_dry_raw=500\nsoil_wet_raw=500\n",   /* wet == dry */
        "soil_dry_raw=900\nsoil_wet_raw=400\n",   /* wet < dry */
        "soil_dry_raw=2500\n",                     /* > default wet 2000 */
        "soil_wet_raw=100\n",                      /* < default dry 200 */
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        struct config keep = { .interval_s = 77, .optimal_light = 77 };
        soil = SOIL_KEEP;
        write_file(CONF, bad[i]);
        CHECK_EQ(config_load_all(CONF, &keep, &soil, why, sizeof why), CONFIG_E_MALFORMED);
        CHECK_EQ(keep.interval_s, 77);  /* untouched on failure */
        CHECK(soil_kept(&soil));
    }
    write_file(CONF, "soil_dry_raw=500\nsoil_wet_raw=500\n");
    config_load_all(CONF, &cfg, &soil, why, sizeof why);
    CHECK_STR(why, "soil_wet_raw (500) must be greater than soil_dry_raw (500)");
}

static void test_save_all(void)
{
    char why[128], text[512];

    /* A valid soil struct survives save_all -> load_all exactly, and so do
     * the interval and light level. */
    const struct config out = { .interval_s = 300, .optimal_light = 123456 };
    const struct soil_config sout = { SOIL_BACKEND_GPIO, 345, 1015, 654321 };
    struct config in = { .interval_s = 0, .optimal_light = 0 };
    struct soil_config sin = SOIL_KEEP;
    CHECK_EQ(config_save_all(CONF, &out, &sout), CONFIG_OK);
    CHECK_EQ(config_load_all(CONF, &in, &sin, why, sizeof why), CONFIG_OK);
    CHECK_EQ(in.interval_s, 300);
    CHECK_EQ(in.optimal_light, 123456);
    CHECK_EQ(sin.backend, SOIL_BACKEND_GPIO);
    CHECK_EQ(sin.dry_raw, 345);
    CHECK_EQ(sin.wet_raw, 1015);
    CHECK_EQ(sin.optimal, 654321);
    CHECK(access(CONF ".tmp", F_OK) != 0);

    /* Soil keys are intact in the file itself. */
    read_file(CONF, text, sizeof text);
    CHECK_STR(text, "# plantsense settings\ninterval_seconds=300\noptimal_light=12.3456\n"
                    "soil_backend=gpio\nsoil_optimal=65.4321\n"
                    "soil_dry_raw=345\nsoil_wet_raw=1015\n");

    /* Edges of the valid range round-trip too. */
    const struct soil_config edge = { SOIL_BACKEND_I2C, 0, 4095, 0 };
    CHECK_EQ(config_save_all(CONF, &out, &edge), CONFIG_OK);
    CHECK_EQ(config_load_all(CONF, &in, &sin, why, sizeof why), CONFIG_OK);
    CHECK_EQ(sin.backend, SOIL_BACKEND_I2C);
    CHECK_EQ(sin.dry_raw, 0);
    CHECK_EQ(sin.wet_raw, 4095);
    CHECK_EQ(sin.optimal, 0);

    /* Invalid pairs are omitted on save; load_all then gives 200/2000. */
    static const struct { int32_t dry, wet; } bad_pairs[] = {
        { 0, 0 }, { 500, 500 }, { 900, 400 }, { -5, 10 }, { 10, 4096 },
    };
    for (size_t i = 0; i < sizeof bad_pairs / sizeof bad_pairs[0]; i++) {
        const struct soil_config bad = {
            SOIL_BACKEND_GPIO, bad_pairs[i].dry, bad_pairs[i].wet, 250000,
        };
        CHECK_EQ(config_save_all(CONF, &out, &bad), CONFIG_OK);
        read_file(CONF, text, sizeof text);
        CHECK(strstr(text, "soil_dry_raw") == NULL);
        CHECK(strstr(text, "soil_wet_raw") == NULL);
        sin = SOIL_KEEP;
        CHECK_EQ(config_load_all(CONF, &in, &sin, why, sizeof why), CONFIG_OK);
        CHECK_EQ(sin.dry_raw, 200);
        CHECK_EQ(sin.wet_raw, 2000);
        CHECK_EQ(sin.backend, SOIL_BACKEND_GPIO);  /* the rest still saved */
        CHECK_EQ(sin.optimal, 250000);
        CHECK_EQ(in.interval_s, 300);
    }

    /* Out-of-range optimal is omitted the same way. */
    const struct soil_config bad_opt = { SOIL_BACKEND_I2C, 300, 1500, 1000001 };
    CHECK_EQ(config_save_all(CONF, &out, &bad_opt), CONFIG_OK);
    CHECK_EQ(config_load_all(CONF, &in, &sin, why, sizeof why), CONFIG_OK);
    CHECK_EQ(sin.optimal, 500000);
    CHECK_EQ(sin.dry_raw, 300);
}

static void test_legacy_load(void)
{
    struct config cfg = { .interval_s = 1, .optimal_light = 1 };
    char why[128];

    /* The legacy loader accepts a file with valid soil keys... */
    write_file(CONF, "interval_seconds=120\noptimal_light=42.5\nsoil_backend=i2c\n"
                     "soil_dry_raw=300\nsoil_wet_raw=1500\nsoil_optimal=40\n");
    CHECK_EQ(config_load(CONF, &cfg, why, sizeof why), CONFIG_OK);
    CHECK_EQ(cfg.interval_s, 120);
    CHECK_EQ(cfg.optimal_light, 425000);

    /* ...and rejects one with a malformed soil key or bad pair. */
    static const char *const bad[] = {
        "interval_seconds=120\nsoil_backend=usb\n",
        "interval_seconds=120\nsoil_dry_raw=x\n",
        "interval_seconds=120\nsoil_dry_raw=600\nsoil_wet_raw=500\n",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        struct config keep = { .interval_s = 77, .optimal_light = 77 };
        write_file(CONF, bad[i]);
        CHECK_EQ(config_load(CONF, &keep, why, sizeof why), CONFIG_E_MALFORMED);
        CHECK_EQ(keep.interval_s, 77);
    }
}

static void test_log_format_i2c(void)
{
    char line[LOG_LINE_MAX];
    /* 2026-11-12T16:30:00 in UTC */
    const time_t t = 1794501000;
    const struct soil_i2c_reading ok = { 412, 571234, 215, true };

    /* Both sensors OK. */
    log_format_reading(line, sizeof line, t, 421337, 419020, VERDICT_MORE);
    log_append_soil_i2c(line, sizeof line, NULL, &ok, "SOIL OK");
    CHECK_STR(line, "2026-11-12T16:30:00 level=42.1337 avg=41.9020 NEEDS MORE LIGHT"
                    " soil=57.1234 raw=412 temp=21.5 SOIL OK");

    /* Edges: 0.0000 and 100.0000, whole-degree temperature. */
    const struct soil_i2c_reading dry = { 0, 0, 200, true };
    const struct soil_i2c_reading wet = { 4095, 1000000, 9, true };
    log_format_reading(line, sizeof line, t, 500000, 500000, VERDICT_OK);
    log_append_soil_i2c(line, sizeof line, NULL, &dry, "NEEDS WATER");
    CHECK_STR(line, "2026-11-12T16:30:00 level=50.0000 avg=50.0000 LIGHT OK"
                    " soil=0.0000 raw=0 temp=20.0 NEEDS WATER");
    log_format_reading(line, sizeof line, t, 500000, 500000, VERDICT_OK);
    log_append_soil_i2c(line, sizeof line, NULL, &wet, "TOO WET");
    CHECK_STR(line, "2026-11-12T16:30:00 level=50.0000 avg=50.0000 LIGHT OK"
                    " soil=100.0000 raw=4095 temp=0.9 TOO WET");

    /* Temperature failed alone. */
    const struct soil_i2c_reading no_temp = { 412, 571234, 0, false };
    log_format_reading(line, sizeof line, t, 421337, 419020, VERDICT_MORE);
    log_append_soil_i2c(line, sizeof line, NULL, &no_temp, "SOIL OK");
    CHECK_STR(line, "2026-11-12T16:30:00 level=42.1337 avg=41.9020 NEEDS MORE LIGHT"
                    " soil=57.1234 raw=412 temp=NA SOIL OK");

    /* Soil failed, light fine. */
    log_format_reading(line, sizeof line, t, 421337, 419020, VERDICT_MORE);
    log_append_soil_i2c(line, sizeof line, "no device at 0x36", NULL, NULL);
    CHECK_STR(line, "2026-11-12T16:30:00 level=42.1337 avg=41.9020 NEEDS MORE LIGHT"
                    " soil=ERROR(no device at 0x36)");

    /* Light failed, soil fine. */
    log_format_failure(line, sizeof line, t, "rpicam-still timed out after 15000 ms");
    log_append_soil_i2c(line, sizeof line, NULL, &ok, "SOIL OK");
    CHECK_STR(line, "2026-11-12T16:30:00 ERROR capture failed: rpicam-still timed out"
                    " after 15000 ms soil=57.1234 raw=412 temp=21.5 SOIL OK");

    /* Both failed. */
    log_format_failure(line, sizeof line, t, "could not run rpicam-still (installed? on PATH?)");
    log_append_soil_i2c(line, sizeof line, "unexpected device 0x56 at 0x36", &ok, NULL);
    CHECK_STR(line, "2026-11-12T16:30:00 ERROR capture failed: could not run rpicam-still"
                    " (installed? on PATH?) soil=ERROR(unexpected device 0x56 at 0x36)");

    /* Longest open() reason still fits after the longest capture reason. */
    char reason[160];
    memset(reason, 'x', sizeof reason - 1);
    reason[sizeof reason - 1] = '\0';
    log_format_failure(line, sizeof line, t, reason);
    log_append_soil_i2c(line, sizeof line, "open /dev/i2c-1 failed: permission denied"
                        " (add the user to the i2c group)", NULL, NULL);
    CHECK(strstr(line, "(add the user to the i2c group))") != NULL);
}

int main(void)
{
    setenv("TZ", "UTC", 1);
    tzset();
    mkdir("build", 0755);
    mkdir("build/tests", 0755);
    mkdir(WORK, 0755);

    test_parsers();
    test_load_all();
    test_save_all();
    test_legacy_load();
    test_log_format_i2c();
    return test_report("test_soil_config_log");
}
