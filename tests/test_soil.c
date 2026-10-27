/*
 * Soil sensor tests. Vote/error logic uses injected fake ops; the gpioget
 * plumbing uses a fake gpioget (tests/fake_bin) put first on PATH.
 * The real GPIO line is only exercised on the Pi.
 */
#define _POSIX_C_SOURCE 200809L

#include "soil.h"
#include "test.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define WORK   "build/tests/soil_tmp"
#define RECORD WORK "/args"

/* Scripted fake: sample i returns g_rc[i], and g_raw[i] on success. */
static int g_raw[SOIL_SAMPLES];
static int g_rc[SOIL_SAMPLES];
static int g_calls;

static int fake_read_raw(int *raw_out)
{
    int i = g_calls++;
    if (i >= SOIL_SAMPLES)
        return SOIL_E_ARG;  /* soil_read asked for too many samples */
    if (g_rc[i] != 0)
        return g_rc[i];
    *raw_out = g_raw[i];
    return 0;
}

static const soil_ops_t fake_ops = { .read_raw = fake_read_raw };

static void script(int a, int b, int c)
{
    static_assert(SOIL_SAMPLES == 3, "tests below script exactly 3 samples");
    g_raw[0] = a;
    g_raw[1] = b;
    g_raw[2] = c;
    memset(g_rc, 0, sizeof g_rc);
    g_calls = 0;
}

static void test_majority(void)
{
    static const struct { int a, b, c; soil_state_t want; } cases[] = {
        { 0, 0, 0, SOIL_WET },
        { 1, 1, 1, SOIL_DRY },
        { 0, 0, 1, SOIL_WET },
        { 1, 1, 0, SOIL_DRY },
        { 0, 1, 1, SOIL_DRY },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        script(cases[i].a, cases[i].b, cases[i].c);
        soil_state_t s = (soil_state_t)99;
        CHECK_EQ(soil_read(&fake_ops, &s), SOIL_OK);
        CHECK_EQ(s, cases[i].want);
        CHECK_EQ(g_calls, SOIL_SAMPLES);
    }
}

static void test_read_error_fails_whole_read(void)
{
    for (int bad = 0; bad < SOIL_SAMPLES; bad++) {
        script(1, 1, 1);
        g_rc[bad] = SOIL_E_TIMEOUT;
        soil_state_t s = (soil_state_t)99;
        CHECK_EQ(soil_read(&fake_ops, &s), SOIL_E_TIMEOUT);
        CHECK_EQ(s, 99);  /* untouched: no stale or partial value */
    }

    /* A driver handing back something other than 0/1 is an error too. */
    script(0, 2, 0);
    soil_state_t s = (soil_state_t)99;
    CHECK_EQ(soil_read(&fake_ops, &s), SOIL_E_PARSE);
    CHECK_EQ(s, 99);

    CHECK_EQ(soil_read(NULL, &s), SOIL_E_ARG);
    CHECK_EQ(soil_read(&fake_ops, NULL), SOIL_E_ARG);
}

static void test_parse(void)
{
    int raw = -1;
    CHECK_EQ(soil_parse("0\n", 2, &raw), 0);
    CHECK_EQ(raw, 0);
    CHECK_EQ(soil_parse("1\n", 2, &raw), 0);
    CHECK_EQ(raw, 1);

    static const char *const bad[] = {
        "", "2", "01", "active", " 1\n", "1 \n", "1\n\n", "1\r\n", "\n", "0\t", "-1",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        raw = 42;
        CHECK_EQ(soil_parse(bad[i], strlen(bad[i]), &raw), -1);
        CHECK_EQ(raw, 42);
    }
    /* Embedded NUL: length says 2, but the second byte isn't '\n'. */
    CHECK_EQ(soil_parse("1\0", 2, &raw), -1);
}

static void test_verdict(void)
{
    CHECK_STR(soil_verdict_str(SOIL_DRY), "NEEDS WATER");
    CHECK_STR(soil_verdict_str(SOIL_WET), "SOIL OK");
    CHECK_STR(soil_state_str(SOIL_DRY), "DRY");
    CHECK_STR(soil_state_str(SOIL_WET), "WET");
    CHECK_EQ(soil_state_raw(SOIL_DRY), 1);
    CHECK_EQ(soil_state_raw(SOIL_WET), 0);
}

static int no_children_left(void)
{
    return waitpid(-1, NULL, WNOHANG) < 0 && errno == ECHILD;
}

static void gpio_mode(const char *mode)
{
    setenv("PS_FAKE_GPIO_MODE", mode, 1);
    unlink(RECORD);
}

static void test_gpioget(void)
{
    soil_state_t s = (soil_state_t)99;

    gpio_mode("one");
    CHECK_EQ(soil_read(&soil_gpioget_ops, &s), SOIL_OK);
    CHECK_EQ(s, SOIL_DRY);
    gpio_mode("zero");
    CHECK_EQ(soil_read(&soil_gpioget_ops, &s), SOIL_OK);
    CHECK_EQ(s, SOIL_WET);
    CHECK(no_children_left());

    /* Exactly the command verified on the Pi. */
    char args[256] = "";
    FILE *f = fopen(RECORD, "r");
    CHECK(f != NULL);
    if (f) {
        if (!fgets(args, sizeof args, f))
            args[0] = '\0';
        fclose(f);
    }
    CHECK_STR(args, "--numeric -c gpiochip0 17\n");

    static const struct { const char *mode; int want; } fails[] = {
        { "fail",    SOIL_E_EXIT },
        { "garbage", SOIL_E_PARSE },
        { "flood",   SOIL_E_PARSE },
    };
    for (size_t i = 0; i < sizeof fails / sizeof fails[0]; i++) {
        gpio_mode(fails[i].mode);
        s = (soil_state_t)99;
        CHECK_EQ(soil_read(&soil_gpioget_ops, &s), fails[i].want);
        CHECK_EQ(s, 99);
        CHECK(no_children_left());
    }
}

static void test_gpioget_timeout(void)
{
    gpio_mode("hang");
    soil_state_t s = (soil_state_t)99;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    CHECK_EQ(soil_read(&soil_gpioget_ops, &s), SOIL_E_TIMEOUT);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    /* First sample times out and the read stops; it doesn't try all three. */
    CHECK(ms >= SOIL_TIMEOUT_MS && ms < 2 * SOIL_TIMEOUT_MS);
    CHECK_EQ(s, 99);
    CHECK_STR(soil_strerror(SOIL_E_TIMEOUT), "gpioget timeout");
    CHECK(no_children_left());
}

static void test_gpioget_missing(void)
{
    const char *old = getenv("PATH");
    char saved[4096];
    snprintf(saved, sizeof saved, "%s", old ? old : "");
    setenv("PATH", WORK, 1);  /* no gpioget here, like Fedora */
    soil_state_t s = (soil_state_t)99;
    CHECK_EQ(soil_read(&soil_gpioget_ops, &s), SOIL_E_EXEC);
    CHECK_EQ(s, 99);
    CHECK_STR(soil_strerror(SOIL_E_EXEC), "gpioget not found");
    CHECK(no_children_left());
    setenv("PATH", saved, 1);
}

int main(void)
{
    mkdir("build", 0755);
    mkdir("build/tests", 0755);
    mkdir(WORK, 0755);

    const char *old = getenv("PATH");
    char path[4096];
    snprintf(path, sizeof path, "tests/fake_bin:%s", old ? old : "/usr/bin:/bin");
    setenv("PATH", path, 1);
    setenv("PS_FAKE_GPIO_RECORD", RECORD, 1);

    test_majority();
    test_read_error_fails_whole_read();
    test_parse();
    test_verdict();
    test_gpioget();
    test_gpioget_timeout();
    test_gpioget_missing();
    return test_report("test_soil");
}
