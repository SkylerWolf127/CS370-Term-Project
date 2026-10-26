/*
 * Camera plumbing tests with a fake rpicam-still (tests/fake_bin) put first
 * on PATH. Checks fork/exec/timeout/reap and temp-file cleanup; the real
 * camera is only exercised on the Pi.
 */
#define _POSIX_C_SOURCE 200809L

#include "camera.h"
#include "test.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define WORK   "build/tests/camera_tmp"
#define RECORD WORK "/record"

static void set_mode(const char *mode)
{
    setenv("PS_FAKE_MODE", mode, 1);
    unlink(RECORD);
}

/* Path the fake was told to write, or "" if it never ran. */
static const char *recorded_path(void)
{
    static char buf[256];
    buf[0] = '\0';
    FILE *f = fopen(RECORD, "r");
    if (f) {
        if (!fgets(buf, sizeof buf, f))
            buf[0] = '\0';
        fclose(f);
    }
    buf[strcspn(buf, "\n")] = '\0';
    return buf;
}

static int file_exists(const char *p) { return p[0] && access(p, F_OK) == 0; }

static int no_children_left(void)
{
    return waitpid(-1, NULL, WNOHANG) < 0 && errno == ECHILD;
}

/* 640x480 uniform grey-128 BMP, top-down, as rpicam-still would write. */
static void write_fixture(const char *path)
{
    static uint8_t img[54 + 640 * 3 * 480];
    memset(img, 0, 54);
    img[0] = 'B';
    img[1] = 'M';
    uint32_t vals[][2] = {
        { 2, sizeof img }, { 10, 54 }, { 14, 40 }, { 18, 640 },
        { 22, (uint32_t)-480 }, { 26, 1 | (24u << 16) },
    };
    for (size_t i = 0; i < sizeof vals / sizeof vals[0]; i++)
        for (int b = 0; b < 4; b++)
            img[vals[i][0] + b] = (uint8_t)(vals[i][1] >> (8 * b));
    memset(img + 54, 128, sizeof img - 54);
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f) {
        CHECK(fwrite(img, 1, sizeof img, f) == sizeof img);
        CHECK(fclose(f) == 0);
    }
}

static void test_success(void)
{
    set_mode("ok");
    fixed4_t level = -1;
    char why[128];
    CHECK_EQ(camera_measure(&level, 5000, NULL, why, sizeof why), CAM_OK);
    CHECK_EQ(level, 501961);
    CHECK(strncmp(recorded_path(), "/tmp/plantsense_", 16) == 0);
    CHECK(!file_exists(recorded_path()));
    CHECK(no_children_left());

    /* Exposure must be locked: these flags have to reach the tool. */
    char args[512] = "";
    FILE *f = fopen(RECORD ".args", "r");
    CHECK(f != NULL);
    if (f) {
        if (!fgets(args, sizeof args, f))
            args[0] = '\0';
        fclose(f);
    }
    CHECK(strstr(args, "--shutter " CAM_SHUTTER_US) != NULL);
    CHECK(strstr(args, "--gain " CAM_GAIN) != NULL);
    CHECK(strstr(args, "--awbgains " CAM_AWB_GAINS) != NULL);
    CHECK(strstr(args, "--encoding bmp") != NULL);
    CHECK(strstr(args, "--nopreview") != NULL);
}

static void test_failure(const char *mode, int want)
{
    set_mode(mode);
    fixed4_t level = 777;
    char why[128] = "";
    CHECK_EQ(camera_measure(&level, 5000, NULL, why, sizeof why), want);
    CHECK_EQ(level, 777);
    CHECK(strcmp(why, "ok") != 0 && why[0] != '\0');
    CHECK(!file_exists(recorded_path()));
    CHECK(no_children_left());
}

static void test_timeout_kills_child(void)
{
    set_mode("hang");
    fixed4_t level;
    char why[128];
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    CHECK_EQ(camera_measure(&level, 300, NULL, why, sizeof why), CAM_E_TIMEOUT);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    CHECK(ms >= 300 && ms < 3000);
    CHECK(!file_exists(recorded_path()));
    CHECK(no_children_left());
}

static void test_abort_flag(void)
{
    set_mode("hang");
    volatile sig_atomic_t flag = SIGINT;
    fixed4_t level;
    char why[128];
    CHECK_EQ(camera_measure(&level, 5000, &flag, why, sizeof why), CAM_E_ABORTED);
    CHECK(no_children_left());
}

static void test_missing_program(void)
{
    const char *old = getenv("PATH");
    char saved[4096];
    snprintf(saved, sizeof saved, "%s", old ? old : "");
    setenv("PATH", WORK, 1);  /* no rpicam-still here */
    fixed4_t level;
    char why[128];
    CHECK_EQ(camera_measure(&level, 5000, NULL, why, sizeof why), CAM_E_EXEC);
    CHECK(no_children_left());
    setenv("PATH", saved, 1);
}

int main(void)
{
    mkdir("build", 0755);
    mkdir("build/tests", 0755);
    mkdir(WORK, 0755);
    write_fixture(WORK "/grey.bmp");

    const char *old = getenv("PATH");
    char path[4096];
    snprintf(path, sizeof path, "tests/fake_bin:%s", old ? old : "/usr/bin:/bin");
    setenv("PATH", path, 1);
    setenv("PS_FAKE_FIXTURE", WORK "/grey.bmp", 1);
    setenv("PS_FAKE_RECORD", RECORD, 1);

    test_success();
    test_failure("fail", CAM_E_EXIT);
    test_failure("garbage", CAM_E_IMAGE);
    test_failure("nofile", CAM_E_READ);
    test_timeout_kills_child();
    test_abort_flag();
    test_missing_program();
    return test_report("test_camera");
}
