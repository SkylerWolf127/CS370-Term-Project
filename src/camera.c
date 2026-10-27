#define _POSIX_C_SOURCE 200809L

#include "camera.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "proc.h"

#define CAM_TEMPLATE "/tmp/plantsense_XXXXXX"

/* Fixed buffer instead of malloc: header slack + padded 24-bit rows. */
#define CAM_ROW_BYTES ((CAM_WIDTH * 3 + 3) & ~3)
#define CAM_BUF_BYTES (1024 + (size_t)CAM_ROW_BYTES * CAM_HEIGHT)
static uint8_t g_image[CAM_BUF_BYTES];

static void say(char *why, size_t len, const char *fmt, ...)
{
    if (!why || len == 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, len, fmt, ap);
    va_end(ap);
}

/* Returns CAM_OK once rpicam-still exits 0, else a cam_status. Always reaps. */
static int capture(const char *path, int timeout_ms,
                   const volatile sig_atomic_t *abort_flag,
                   char *why, size_t why_len)
{
    char *const argv[] = {
        CAM_PROGRAM,
        "--nopreview",
        "-t", "1",  /* ms before capture; exposure is fixed so no settling needed */
        "--width", "640",
        "--height", "480",
        "--shutter", CAM_SHUTTER_US,
        "--gain", CAM_GAIN,
        "--awbgains", CAM_AWB_GAINS,
        "--encoding", "bmp",
        "-o", (char *)path,
        NULL,
    };
    static_assert(CAM_WIDTH == 640 && CAM_HEIGHT == 480,
                  "update the --width/--height strings above");

    switch (proc_run(argv, timeout_ms, abort_flag, NULL, 0, NULL, why, why_len)) {
    case PROC_OK:         return CAM_OK;
    case PROC_E_EXEC:     return CAM_E_EXEC;
    case PROC_E_EXIT:     return CAM_E_EXIT;
    case PROC_E_SIGNALED: return CAM_E_SIGNALED;
    case PROC_E_TIMEOUT:  return CAM_E_TIMEOUT;
    case PROC_E_ABORTED:  return CAM_E_ABORTED;
    default:              return CAM_E_FORK;
    }
}

/* Read the whole file into g_image. */
static int read_image(const char *path, size_t *len, char *why, size_t why_len)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        say(why, why_len, "open image failed: %s", strerror(errno));
        return CAM_E_READ;
    }
    size_t got = 0;
    int rc = CAM_OK;
    for (;;) {
        if (got == sizeof g_image) {
            uint8_t extra;
            ssize_t n = read(fd, &extra, 1);
            if (n != 0) {
                say(why, why_len, "image larger than %zu bytes", sizeof g_image);
                rc = CAM_E_TOO_BIG;
            }
            break;
        }
        ssize_t n = read(fd, g_image + got, sizeof g_image - got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            say(why, why_len, "read image failed: %s", strerror(errno));
            rc = CAM_E_READ;
            break;
        }
        if (n == 0)
            break;
        got += (size_t)n;
    }
    if (close(fd) != 0 && rc == CAM_OK) {
        say(why, why_len, "close image failed: %s", strerror(errno));
        rc = CAM_E_READ;
    }
    *len = got;
    return rc;
}

int camera_measure(fixed4_t *level, int timeout_ms,
                   const volatile sig_atomic_t *abort_flag,
                   char *why, size_t why_len)
{
    say(why, why_len, "ok");

    char path[] = CAM_TEMPLATE;
    int fd = mkstemp(path);
    if (fd < 0) {
        say(why, why_len, "mkstemp failed: %s", strerror(errno));
        return CAM_E_TEMPFILE;
    }
    close(fd);  /* rpicam-still reopens it by name */

    /* capture() always reaps the child, so nobody can recreate the file
     * after we unlink it below. */
    int rc = capture(path, timeout_ms, abort_flag, why, why_len);
    size_t len = 0;
    if (rc == CAM_OK)
        rc = read_image(path, &len, why, why_len);

    /* Delete on every path, success or not. */
    if (unlink(path) != 0 && errno != ENOENT && rc == CAM_OK) {
        say(why, why_len, "could not delete %s: %s", path, strerror(errno));
        rc = CAM_E_READ;
    }
    if (rc != CAM_OK)
        return rc;

    int lrc = light_from_bmp(g_image, len, level);
    if (lrc != LIGHT_OK) {
        say(why, why_len, "image rejected: %s", light_strerror(lrc));
        return CAM_E_IMAGE;
    }
    return CAM_OK;
}
