#define _POSIX_C_SOURCE 200809L

#include "config.h"
#include "log.h"
#include "test.h"

#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define WORK "build/tests/config_log_tmp"
#define CONF WORK "/plantsense.conf"
#define LOGS WORK "/logs"

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void test_interval_parse(void)
{
    int v = -1;
    CHECK_EQ(config_parse_interval("60", &v), 0);
    CHECK_EQ(v, 60);
    CHECK_EQ(config_parse_interval(" 5\n", &v), 0);
    CHECK_EQ(v, 5);
    CHECK_EQ(config_parse_interval("86400", &v), 0);
    CHECK_EQ(v, 86400);

    static const char *const bad[] = {
        "", "4", "0", "-60", "+60", "86401", "60s", "6 0", "1e3", "99999999999999999999",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        v = 42;
        CHECK_EQ(config_parse_interval(bad[i], &v), -1);
        CHECK_EQ(v, 42);
    }
}

static void test_config_files(void)
{
    struct config cfg = { 1, 1 };
    char why[128];

    unlink(CONF);
    CHECK_EQ(config_load(CONF, &cfg, why, sizeof why), CONFIG_OK);  /* missing */
    CHECK_EQ(cfg.interval_s, 60);
    CHECK_EQ(cfg.optimal_light, 500000);

    write_file(CONF, "# hi\n\n  interval_seconds = 120 \noptimal_light=42.5\n");
    CHECK_EQ(config_load(CONF, &cfg, why, sizeof why), CONFIG_OK);
    CHECK_EQ(cfg.interval_s, 120);
    CHECK_EQ(cfg.optimal_light, 425000);

    static const char *const bad[] = {
        "interval_seconds=abc\n",
        "interval_seconds=1\n",
        "optimal_light=101\n",
        "optimal_light=-3\n",
        "optimal_light=\n",
        "colour=green\n",
        "just some words\n",
        "optimal_light=42.5\ninterval_seconds=60;rm -rf /\n",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        struct config keep = { 77, 77 };
        write_file(CONF, bad[i]);
        CHECK_EQ(config_load(CONF, &keep, why, sizeof why), CONFIG_E_MALFORMED);
        CHECK_EQ(keep.interval_s, 77);  /* untouched on failure */
        CHECK_EQ(keep.optimal_light, 77);
    }

    /* Overlong line is rejected, not split into pieces. */
    char longline[600];
    memset(longline, 'x', sizeof longline - 2);
    longline[sizeof longline - 2] = '\n';
    longline[sizeof longline - 1] = '\0';
    write_file(CONF, longline);
    CHECK_EQ(config_load(CONF, &cfg, why, sizeof why), CONFIG_E_MALFORMED);

    /* Save -> load round trip, and no temp file left behind. */
    struct config out = { 300, 123456 }, in = { 0, 0 };
    CHECK_EQ(config_save(CONF, &out), CONFIG_OK);
    CHECK_EQ(config_load(CONF, &in, why, sizeof why), CONFIG_OK);
    CHECK_EQ(in.interval_s, 300);
    CHECK_EQ(in.optimal_light, 123456);
    CHECK(access(CONF ".tmp", F_OK) != 0);
}

static void test_log_format(void)
{
    char line[LOG_LINE_MAX];
    /* 2026-10-26T16:05:00 in UTC */
    const time_t t = 1793030700;
    log_format_reading(line, sizeof line, t, 421337, 419020, VERDICT_MORE);
    CHECK_STR(line, "2026-10-26T16:05:00 level=42.1337 avg=41.9020 NEEDS MORE LIGHT");
    log_format_reading(line, sizeof line, t, 0, 1000000, VERDICT_LESS);
    CHECK_STR(line, "2026-10-26T16:05:00 level=0.0000 avg=100.0000 NEEDS LESS LIGHT");
    log_format_failure(line, sizeof line, t, "rpicam-still timed out after 15000 ms");
    CHECK_STR(line, "2026-10-26T16:05:00 ERROR capture failed: rpicam-still timed out after 15000 ms");
}

static void test_log_format_soil(void)
{
    char line[LOG_LINE_MAX];
    /* 2026-10-27T17:30:00 in UTC */
    const time_t t = 1793122200;

    /* Both sensors OK. */
    log_format_reading(line, sizeof line, t, 421337, 419020, VERDICT_MORE);
    log_append_soil(line, sizeof line, SOIL_OK, SOIL_DRY);
    CHECK_STR(line, "2026-10-27T17:30:00 level=42.1337 avg=41.9020 NEEDS MORE LIGHT"
                    " soil=DRY raw=1 NEEDS WATER");
    log_format_reading(line, sizeof line, t, 500000, 500000, VERDICT_OK);
    log_append_soil(line, sizeof line, SOIL_OK, SOIL_WET);
    CHECK_STR(line, "2026-10-27T17:30:00 level=50.0000 avg=50.0000 LIGHT OK"
                    " soil=WET raw=0 SOIL OK");

    /* Soil failed, light fine. */
    log_format_reading(line, sizeof line, t, 421337, 419020, VERDICT_MORE);
    log_append_soil(line, sizeof line, SOIL_E_TIMEOUT, SOIL_WET);
    CHECK_STR(line, "2026-10-27T17:30:00 level=42.1337 avg=41.9020 NEEDS MORE LIGHT"
                    " soil=ERROR(gpioget timeout)");

    /* Light failed, soil fine. */
    log_format_failure(line, sizeof line, t, "rpicam-still timed out after 15000 ms");
    log_append_soil(line, sizeof line, SOIL_OK, SOIL_DRY);
    CHECK_STR(line, "2026-10-27T17:30:00 ERROR capture failed: rpicam-still timed out"
                    " after 15000 ms soil=DRY raw=1 NEEDS WATER");

    /* Both failed. */
    log_format_failure(line, sizeof line, t, "could not run rpicam-still (installed? on PATH?)");
    log_append_soil(line, sizeof line, SOIL_E_EXEC, SOIL_WET);
    CHECK_STR(line, "2026-10-27T17:30:00 ERROR capture failed: could not run rpicam-still"
                    " (installed? on PATH?) soil=ERROR(gpioget not found)");
}

static void test_log_files(void)
{
    char newest[LOG_PATH_MAX];
    CHECK_EQ(log_find_newest(LOGS "_does_not_exist", newest, sizeof newest), 1);

    struct session_log a, b;
    CHECK_EQ(log_open(&a, LOGS, 1793030700), 0);  /* creates the directory */
    CHECK_STR(a.path, LOGS "/session_20261026_160500.txt");
    CHECK_EQ(log_write_line(&a, "first"), 0);
    CHECK_EQ(log_write_line(&a, "second"), 0);
    CHECK_EQ(log_close(&a), 0);

    CHECK_EQ(log_open(&b, LOGS, 1793030700 + 86400), 0);
    CHECK_EQ(log_write_line(&b, "tomorrow"), 0);
    CHECK_EQ(log_close(&b), 0);

    write_file(LOGS "/session_notes.txt", "decoy\n");   /* wrong shape */
    write_file(LOGS "/session_29991231_235959.bak", "decoy\n");

    CHECK_EQ(log_find_newest(LOGS, newest, sizeof newest), 0);
    CHECK_STR(newest, LOGS "/session_20261027_160500.txt");

    /* Read the first file back through log_print_file. */
    FILE *mem = fopen(WORK "/printed", "w+");
    CHECK(mem != NULL);
    if (mem) {
        CHECK_EQ(log_print_file(LOGS "/session_20261026_160500.txt", mem), 0);
        rewind(mem);
        char got[64] = "";
        size_t n = fread(got, 1, sizeof got - 1, mem);
        got[n] = '\0';
        CHECK_STR(got, "first\nsecond\n");
        fclose(mem);
    }
    CHECK_EQ(log_print_file(LOGS "/nope.txt", stdout), -1);

    struct session_log closed = { 0 };
    CHECK_EQ(log_write_line(&closed, "x"), -1);
}

/* Remove every file in `dir` (one level deep), then the dir itself. */
static void remove_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    char p[512];
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        unlink(p);
    }
    closedir(d);
    rmdir(dir);
}

int main(void)
{
    setenv("TZ", "UTC", 1);
    tzset();
    mkdir("build", 0755);
    mkdir("build/tests", 0755);
    /* Start clean so the newest-log test isn't fooled by an earlier run. */
    remove_dir(LOGS);
    remove_dir(WORK);
    mkdir(WORK, 0755);

    test_interval_parse();
    test_config_files();
    test_log_format();
    test_log_format_soil();
    test_log_files();
    return test_report("test_config_log");
}
