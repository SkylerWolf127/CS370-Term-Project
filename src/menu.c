#define _POSIX_C_SOURCE 200809L

#include "menu.h"

#include <stdio.h>
#include <string.h>

#include "exit_codes.h"
#include "light.h"
#include "log.h"
#include "sig.h"

enum read_result { READ_OK, READ_TOO_LONG, READ_EOF };

/* fgets one line; an overlong line is drained and rejected, never split. */
static enum read_result read_line(char *buf, size_t len)
{
    fflush(stdout);
    if (!fgets(buf, (int)len, stdin))
        return READ_EOF;  /* EOF, error, or EINTR from a signal */
    if (!strchr(buf, '\n') && !feof(stdin)) {
        int c;
        while ((c = getchar()) != '\n' && c != EOF)
            ;
        return READ_TOO_LONG;
    }
    return READ_OK;
}

static void show_menu(const struct config *cfg)
{
    char opt[FIXED4_STR_LEN];
    printf("\n=== plantsense ===\n"
           "1. Set capture interval (seconds)      [%d]\n"
           "2. Set optimal light level (0-100)     [%s]\n"
           "3. Set optimal soil moisture\n"
           "4. Show most recent log\n"
           "5. Start monitoring\n"
           "6. Exit\n"
           "> ",
           cfg->interval_s, fixed4_format(cfg->optimal_light, opt, sizeof opt));
}

static void save(const struct config *cfg, const struct soil_config *soil)
{
    if (config_save_all(CONFIG_PATH, cfg, soil) != CONFIG_OK)
        fprintf(stderr, "ERROR: could not save %s; setting kept for this run only.\n",
                CONFIG_PATH);
    else
        printf("Saved.\n");
}

/* Returns false on EOF/signal so the caller can shut down. */
static bool set_interval(struct config *cfg, const struct soil_config *soil)
{
    char buf[64];
    printf("Capture interval in seconds (%d-%d): ", CONFIG_INTERVAL_MIN, CONFIG_INTERVAL_MAX);
    enum read_result r = read_line(buf, sizeof buf);
    if (r == READ_EOF)
        return false;
    int v;
    if (r == READ_TOO_LONG || config_parse_interval(buf, &v) != 0) {
        fprintf(stderr, "ERROR: enter a whole number from %d to %d.\n",
                CONFIG_INTERVAL_MIN, CONFIG_INTERVAL_MAX);
        return true;
    }
    cfg->interval_s = v;
    save(cfg, soil);
    return true;
}

static bool set_optimal(struct config *cfg, const struct soil_config *soil)
{
    char buf[64];
    printf("Optimal light level (0-100, up to 4 decimals): ");
    enum read_result r = read_line(buf, sizeof buf);
    if (r == READ_EOF)
        return false;
    fixed4_t v;
    if (r == READ_TOO_LONG || light_parse_level(buf, &v) != 0) {
        fprintf(stderr, "ERROR: enter a number like 42 or 42.5 between 0 and 100.\n");
        return true;
    }
    cfg->optimal_light = v;
    save(cfg, soil);
    return true;
}

static void show_latest_log(void)
{
    char path[LOG_PATH_MAX];
    int rc = log_find_newest(LOG_DIR, path, sizeof path);
    if (rc == 1) {
        printf("No session logs yet. Start monitoring (option 5) to make one.\n");
        return;
    }
    printf("--- %s ---\n", rc == 0 ? path : LOG_DIR);
    if (rc != 0 || log_print_file(path, stdout) != 0)
        fprintf(stderr, "ERROR: could not read the most recent log.\n");
    printf("--- end ---\n");
}

int menu_run(struct config *cfg, struct soil_config *soil, struct monitor_state *mon)
{
    for (;;) {
        show_menu(cfg);
        char buf[64];
        enum read_result r = read_line(buf, sizeof buf);
        if (r == READ_EOF) {
            putchar('\n');
            return PS_EXIT_OK;  /* EOF, Ctrl-C, or SIGTERM at the menu */
        }
        int choice = 0;
        if (r == READ_OK && buf[0] >= '1' && buf[0] <= '6' && buf[1] == '\n')
            choice = buf[0] - '0';

        switch (choice) {
        case 1:
            if (!set_interval(cfg, soil))
                return PS_EXIT_OK;
            break;
        case 2:
            if (!set_optimal(cfg, soil))
                return PS_EXIT_OK;
            break;
        case 3:
            fprintf(stderr, "ERROR: soil moisture is not implemented yet\n");
            break;
        case 4:
            show_latest_log();
            break;
        case 5:
            switch (monitor_run(mon, cfg)) {
            case MONITOR_STOPPED:   printf("\nMonitoring stopped.\n"); break;
            case MONITOR_TERMINATE: return PS_EXIT_OK;
            case MONITOR_E_CAMERA:  return PS_EXIT_CAMERA;
            case MONITOR_E_IO:      return PS_EXIT_IO;
            }
            break;
        case 6:
            return PS_EXIT_OK;
        default:
            fprintf(stderr, "ERROR: choose 1-6.\n");
            break;
        }
    }
}
