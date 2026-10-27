#ifndef PLANTSENSE_PROC_H
#define PLANTSENSE_PROC_H

#include <signal.h>
#include <stddef.h>

/* Child's exit code when execvp fails, like a shell. */
#define PROC_EXEC_FAILED 127

enum proc_status {
    PROC_OK = 0,
    PROC_E_FORK,      /* fork, pipe, or waitpid failed */
    PROC_E_EXEC,      /* program missing or not executable */
    PROC_E_EXIT,      /* program exited nonzero */
    PROC_E_SIGNALED,  /* program died from a signal */
    PROC_E_TIMEOUT,   /* killed after timeout_ms */
    PROC_E_ABORTED,   /* abort_flag became nonzero; child killed */
    PROC_E_OUTPUT,    /* reading stdout failed, or it overflowed `out` */
};

/*
 * fork + execvp(argv[0], argv), never a shell. The child dies with us
 * (PDEATHSIG) and is always reaped before we return, on every path.
 *
 * If `out` is non-NULL, the child's stdout is captured into it and
 * NUL-terminated; `*out_got` gets the byte count. Otherwise stdout goes
 * to /dev/null. stdin and stderr always go to /dev/null.
 * On failure, `why` gets a human-readable reason.
 */
int proc_run(char *const argv[], int timeout_ms,
             const volatile sig_atomic_t *abort_flag,
             char *out, size_t out_len, size_t *out_got,
             char *why, size_t why_len);

#endif
