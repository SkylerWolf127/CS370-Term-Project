#define _POSIX_C_SOURCE 200809L

#include "camera.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CAM_TEMPLATE "/tmp/plantsense_XXXXXX"
#define EXEC_FAILED  127  /* child's exit code when execvp fails, like a shell */
#define POLL_NS      (20 * 1000 * 1000)

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

static long elapsed_ms(const struct timespec *start)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start->tv_sec) * 1000L
         + (now.tv_nsec - start->tv_nsec) / 1000000L;
}

static void kill_and_reap(pid_t pid)
{
    kill(pid, SIGKILL);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
        ;
}

static void run_child(const char *path, pid_t parent)
{
    /* Die with the parent, even if it's SIGKILLed: no orphans. */
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != parent)
        _exit(EXEC_FAILED);

    /* rpicam-still is chatty; keep the menu readable. */
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO)
            close(devnull);
    }

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
    execvp(argv[0], argv);
    _exit(EXEC_FAILED);
}

/* Returns CAM_OK once the child exits 0, else a cam_status. Always reaps. */
static int capture(const char *path, int timeout_ms,
                   const volatile sig_atomic_t *abort_flag,
                   char *why, size_t why_len)
{
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid < 0) {
        say(why, why_len, "fork failed: %s", strerror(errno));
        return CAM_E_FORK;
    }
    if (pid == 0)
        run_child(path, parent);  /* never returns */

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int status = 0;
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid)
            break;
        if (r < 0 && errno != EINTR) {
            int e = errno;
            kill_and_reap(pid);
            say(why, why_len, "waitpid failed: %s", strerror(e));
            return CAM_E_FORK;
        }
        if (abort_flag && *abort_flag) {
            kill_and_reap(pid);
            say(why, why_len, "capture interrupted by signal");
            return CAM_E_ABORTED;
        }
        if (elapsed_ms(&start) >= timeout_ms) {
            kill_and_reap(pid);
            say(why, why_len, "%s timed out after %d ms", CAM_PROGRAM, timeout_ms);
            return CAM_E_TIMEOUT;
        }
        struct timespec nap = { 0, POLL_NS };
        nanosleep(&nap, NULL);  /* EINTR is fine: we re-check everything */
    }

    /* Ctrl-C reaches the child too (same process group), so check first. */
    if (abort_flag && *abort_flag) {
        say(why, why_len, "capture interrupted by signal");
        return CAM_E_ABORTED;
    }
    if (WIFSIGNALED(status)) {
        say(why, why_len, "%s killed by signal %d", CAM_PROGRAM, WTERMSIG(status));
        return CAM_E_SIGNALED;
    }
    if (WEXITSTATUS(status) == EXEC_FAILED) {
        say(why, why_len, "could not run %s (installed? on PATH?)", CAM_PROGRAM);
        return CAM_E_EXEC;
    }
    if (WEXITSTATUS(status) != 0) {
        say(why, why_len, "%s exited with status %d", CAM_PROGRAM, WEXITSTATUS(status));
        return CAM_E_EXIT;
    }
    return CAM_OK;
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
