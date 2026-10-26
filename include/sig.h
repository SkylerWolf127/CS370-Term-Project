#ifndef PLANTSENSE_SIG_H
#define PLANTSENSE_SIG_H

#include <signal.h>

/*
 * SIGINT/SIGTERM only set a flag; everything else happens in normal code.
 * Handlers are installed without SA_RESTART so blocking calls (fgets,
 * nanosleep) return early with EINTR and the flag gets noticed promptly.
 */
int sig_install(void);

/* 0, SIGINT, or SIGTERM. A pending SIGTERM is never downgraded to SIGINT. */
int sig_pending(void);
void sig_clear(void);

/* For code (like camera capture) that just needs "should I abort?". */
const volatile sig_atomic_t *sig_flag(void);

#endif
