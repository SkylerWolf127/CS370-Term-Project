#define _POSIX_C_SOURCE 200809L

#include "proc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define POLL_NS (20 * 1000 * 1000)

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

static void run_child(char *const argv[], pid_t parent, int out_fd)
{
    /* Die with the parent, even if it's SIGKILLed: no orphans. */
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != parent)
        _exit(PROC_EXEC_FAILED);

    /* Keep the menu readable; only a captured stdout goes anywhere. */
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO)
            close(devnull);
    }
    if (out_fd >= 0) {
        if (dup2(out_fd, STDOUT_FILENO) < 0)
            _exit(PROC_EXEC_FAILED);
        close(out_fd);
    }
    execvp(argv[0], argv);
    _exit(PROC_EXEC_FAILED);
}

/* Pull whatever is in the pipe now. Past `out_len - 1` bytes we keep
 * draining (so the child never blocks on a full pipe) but flag overflow. */
static int drain(int fd, char *out, size_t out_len, size_t *got, bool *overflow)
{
    for (;;) {
        char scratch[256];
        char *dst = scratch;
        size_t room = sizeof scratch;
        if (*got < out_len - 1) {
            dst = out + *got;
            room = out_len - 1 - *got;
        }
        ssize_t n = read(fd, dst, room);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;
        if (n < 0)
            return -1;
        if (n == 0)
            return 0;
        if (dst == scratch)
            *overflow = true;
        else
            *got += (size_t)n;
    }
}

int proc_run(char *const argv[], int timeout_ms,
             const volatile sig_atomic_t *abort_flag,
             char *out, size_t out_len, size_t *out_got,
             char *why, size_t why_len)
{
    const char *prog = argv[0];
    int fds[2] = { -1, -1 };
    size_t got = 0;
    bool overflow = false;

    if (out) {
        if (out_len == 0 || !out_got) {
            say(why, why_len, "proc_run: bad output buffer");
            return PROC_E_OUTPUT;
        }
        out[0] = '\0';
        *out_got = 0;
        if (pipe(fds) != 0) {
            say(why, why_len, "pipe failed: %s", strerror(errno));
            return PROC_E_FORK;
        }
        if (fcntl(fds[0], F_SETFD, FD_CLOEXEC) != 0
            || fcntl(fds[0], F_SETFL, O_NONBLOCK) != 0) {
            say(why, why_len, "fcntl failed: %s", strerror(errno));
            close(fds[0]);
            close(fds[1]);
            return PROC_E_FORK;
        }
    }

    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid < 0) {
        say(why, why_len, "fork failed: %s", strerror(errno));
        if (out) {
            close(fds[0]);
            close(fds[1]);
        }
        return PROC_E_FORK;
    }
    if (pid == 0) {
        if (out)
            close(fds[0]);
        run_child(argv, parent, fds[1]);  /* never returns */
    }
    if (out)
        close(fds[1]);

    int rc = PROC_OK;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int status = 0;
    for (;;) {
        if (out && drain(fds[0], out, out_len, &got, &overflow) != 0) {
            int e = errno;
            kill_and_reap(pid);
            say(why, why_len, "reading %s output failed: %s", prog, strerror(e));
            rc = PROC_E_OUTPUT;
            goto done;
        }
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid)
            break;
        if (r < 0 && errno != EINTR) {
            int e = errno;
            kill_and_reap(pid);
            say(why, why_len, "waitpid failed: %s", strerror(e));
            rc = PROC_E_FORK;
            goto done;
        }
        if (abort_flag && *abort_flag) {
            kill_and_reap(pid);
            say(why, why_len, "%s interrupted by signal", prog);
            rc = PROC_E_ABORTED;
            goto done;
        }
        if (elapsed_ms(&start) >= timeout_ms) {
            kill_and_reap(pid);
            say(why, why_len, "%s timed out after %d ms", prog, timeout_ms);
            rc = PROC_E_TIMEOUT;
            goto done;
        }
        struct timespec nap = { 0, POLL_NS };
        nanosleep(&nap, NULL);  /* EINTR is fine: we re-check everything */
    }

    /* Child is gone; collect anything it wrote after our last drain. */
    if (out && drain(fds[0], out, out_len, &got, &overflow) != 0) {
        say(why, why_len, "reading %s output failed: %s", prog, strerror(errno));
        rc = PROC_E_OUTPUT;
        goto done;
    }

    /* Ctrl-C reaches the child too (same process group), so check first. */
    if (abort_flag && *abort_flag) {
        say(why, why_len, "%s interrupted by signal", prog);
        rc = PROC_E_ABORTED;
    } else if (WIFSIGNALED(status)) {
        say(why, why_len, "%s killed by signal %d", prog, WTERMSIG(status));
        rc = PROC_E_SIGNALED;
    } else if (WEXITSTATUS(status) == PROC_EXEC_FAILED) {
        say(why, why_len, "could not run %s (installed? on PATH?)", prog);
        rc = PROC_E_EXEC;
    } else if (WEXITSTATUS(status) != 0) {
        say(why, why_len, "%s exited with status %d", prog, WEXITSTATUS(status));
        rc = PROC_E_EXIT;
    } else if (overflow) {
        say(why, why_len, "%s wrote more than %zu bytes", prog, out_len - 1);
        rc = PROC_E_OUTPUT;
    }

done:
    if (out) {
        close(fds[0]);
        out[got] = '\0';
        *out_got = got;
    }
    return rc;
}
