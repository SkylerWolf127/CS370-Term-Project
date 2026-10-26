#define _POSIX_C_SOURCE 200809L

#include "sig.h"

#include <stddef.h>

static volatile sig_atomic_t g_signal;

static void on_signal(int signo)
{
    /* Async-signal-safe: a single store, nothing else. */
    if (g_signal != SIGTERM)
        g_signal = signo;
}

int sig_install(void)
{
    struct sigaction sa = { 0 };
    sa.sa_handler = on_signal;
    sa.sa_flags = 0;  /* no SA_RESTART, see header */
    if (sigemptyset(&sa.sa_mask) != 0)
        return -1;
    if (sigaction(SIGINT, &sa, NULL) != 0 || sigaction(SIGTERM, &sa, NULL) != 0)
        return -1;
    return 0;
}

int sig_pending(void) { return g_signal; }
void sig_clear(void) { g_signal = 0; }
const volatile sig_atomic_t *sig_flag(void) { return &g_signal; }
