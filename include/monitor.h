#ifndef PLANTSENSE_MONITOR_H
#define PLANTSENSE_MONITOR_H

#include <stdbool.h>

#include "config.h"
#include "light.h"
#include "log.h"

/* Give up (and exit PS_EXIT_CAMERA) after this many failures in a row. */
#define MONITOR_MAX_CAMERA_FAILURES 5

/* Lives for the whole run: one session log, one running average. */
struct monitor_state {
    struct session_log log;
    bool log_open;
    struct light_stats stats;
};

enum monitor_result {
    MONITOR_STOPPED,    /* Ctrl-C: back to the menu */
    MONITOR_TERMINATE,  /* SIGTERM: exit cleanly */
    MONITOR_E_CAMERA,
    MONITOR_E_IO,
};

void monitor_init(struct monitor_state *m);
enum monitor_result monitor_run(struct monitor_state *m, const struct config *cfg);
int monitor_close(struct monitor_state *m);

#endif
