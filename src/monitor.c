#define _POSIX_C_SOURCE 200809L

#include "monitor.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "camera.h"
#include "sig.h"

#define NAP_MS 200  /* bounds how long a signal can go unnoticed while idle */

void monitor_init(struct monitor_state *m)
{
    m->log.fp = NULL;
    m->log_open = false;
    light_stats_init(&m->stats);
}

int monitor_close(struct monitor_state *m)
{
    if (!m->log_open)
        return 0;
    m->log_open = false;
    return log_close(&m->log);
}

/* Sleep up to `seconds`, waking early for a signal. */
static void wait_interval(int seconds)
{
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        if (sig_pending())
            return;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long long ms = (now.tv_sec - start.tv_sec) * 1000LL
                     + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (ms >= seconds * 1000LL)
            return;
        struct timespec nap = { 0, NAP_MS * 1000L * 1000L };
        nanosleep(&nap, NULL);
    }
}

static int emit(struct monitor_state *m, const char *line)
{
    puts(line);
    if (log_write_line(&m->log, line) != 0) {
        fprintf(stderr, "ERROR: writing %s: %s\n", m->log.path, strerror(errno));
        return -1;
    }
    return 0;
}

enum monitor_result monitor_run(struct monitor_state *m, const struct config *cfg)
{
    if (!m->log_open) {
        if (log_open(&m->log, LOG_DIR, time(NULL)) != 0) {
            fprintf(stderr, "ERROR: cannot open session log in %s/: %s\n",
                    LOG_DIR, strerror(errno));
            return MONITOR_E_IO;
        }
        m->log_open = true;
    }

    char opt[FIXED4_STR_LEN];
    printf("Monitoring every %d s (optimal %s), logging to %s\n"
           "Press Ctrl-C to return to the menu.\n",
           cfg->interval_s, fixed4_format(cfg->optimal_light, opt, sizeof opt),
           m->log.path);

    int failures = 0;
    for (;;) {
        if (sig_pending())
            break;

        fixed4_t level;
        char why[160];
        char line[LOG_LINE_MAX];
        int rc = camera_measure(&level, CAM_TIMEOUT_MS, sig_flag(), why, sizeof why);
        time_t now = time(NULL);

        if (rc == CAM_OK && light_stats_add(&m->stats, level) != 0) {
            snprintf(why, sizeof why, "reading out of range or history full");
            rc = CAM_E_IMAGE;
        }
        if (rc == CAM_OK) {
            failures = 0;
            fixed4_t avg = level;
            light_stats_avg(&m->stats, &avg);  /* count >= 1 here */
            log_format_reading(line, sizeof line, now, level, avg,
                               light_verdict(avg, cfg->optimal_light, LIGHT_DEADBAND));
        } else {
            log_format_failure(line, sizeof line, now, why);
        }
        if (emit(m, line) != 0)
            return MONITOR_E_IO;

        if (rc == CAM_E_ABORTED)
            break;
        if (rc != CAM_OK && ++failures >= MONITOR_MAX_CAMERA_FAILURES) {
            fprintf(stderr, "ERROR: %d captures failed in a row; giving up.\n", failures);
            return MONITOR_E_CAMERA;
        }
        wait_interval(cfg->interval_s);
    }

    int signo = sig_pending();
    sig_clear();
    return signo == SIGTERM ? MONITOR_TERMINATE : MONITOR_STOPPED;
}
