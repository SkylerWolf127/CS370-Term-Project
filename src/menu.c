#define _POSIX_C_SOURCE 200809L

#include "menu.h"

#include <stdio.h>
#include <string.h>

#include "exit_codes.h"
#include "light.h"
#include "log.h"
#include "sig.h"
#include "soil_i2c.h"

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

static void show_menu(const struct config *cfg, const struct soil_config *soil)
{
    char opt[FIXED4_STR_LEN], sopt[FIXED4_STR_LEN], item3[64] = "";
    if (soil->backend == SOIL_BACKEND_I2C)
        snprintf(item3, sizeof item3, " (0-100)   [%s]",
                 fixed4_format(soil->optimal, sopt, sizeof sopt));
    printf("\n=== plantsense ===\n"
           "1. Set capture interval (seconds)      [%d]\n"
           "2. Set optimal light level (0-100)     [%s]\n"
           "3. Set optimal soil moisture%s\n"
           "4. Show most recent log\n"
           "5. Start monitoring\n"
           "6. Exit\n"
           "7. Calibrate soil sensor\n"
           "> ",
           cfg->interval_s, fixed4_format(cfg->optimal_light, opt, sizeof opt), item3);
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

static bool set_soil_optimal(const struct config *cfg, struct soil_config *soil)
{
    char buf[64];
    printf("Optimal soil moisture (0-100, up to 4 decimals): ");
    enum read_result r = read_line(buf, sizeof buf);
    if (r == READ_EOF)
        return false;
    fixed4_t v;
    if (r == READ_TOO_LONG || light_parse_level(buf, &v) != 0) {
        fprintf(stderr, "ERROR: enter a number like 42 or 42.5 between 0 and 100.\n");
        return true;
    }
    soil->optimal = v;
    save(cfg, soil);
    return true;
}

/* One median reading, then save its raw count as the dry or wet point. */
static bool calibrate(const struct config *cfg, struct soil_config *soil,
                      struct monitor_state *mon)
{
    if (soil->backend != SOIL_BACKEND_I2C) {
        fprintf(stderr, "ERROR: calibration needs soil_backend=i2c in %s.\n", CONFIG_PATH);
        return true;
    }
    printf("Reading soil sensor...\n");
    struct soil_i2c_reading rd;
    char why[160];
    if (soil_i2c_measure(&mon->soil_id_checked, soil->dry_raw, soil->wet_raw,
                         &rd, why, sizeof why) != SOIL_I2C_OK) {
        fprintf(stderr, "ERROR: soil sensor: %s\n", why);
        return !sig_pending();
    }
    char line[LOG_LINE_MAX] = "";
    log_append_soil_i2c(line, sizeof line, NULL, &rd,
                        soil_i2c_verdict(rd.moisture, soil->optimal, SOIL_I2C_DEADBAND));
    printf("Now:%s\n(calibration: dry=%d wet=%d)\n",
           line, (int)soil->dry_raw, (int)soil->wet_raw);

    char buf[16];
    printf("Save raw=%d as the [d]ry point, [w]et point, or [c]ancel? ", (int)rd.raw);
    enum read_result r = read_line(buf, sizeof buf);
    if (r == READ_EOF)
        return false;
    char c = r == READ_OK && buf[0] != '\0' && buf[1] == '\n' ? buf[0] : '?';
    struct soil_config next = *soil;
    if (c == 'd')
        next.dry_raw = rd.raw;
    else if (c == 'w')
        next.wet_raw = rd.raw;
    else if (c == 'c') {
        printf("Cancelled.\n");
        return true;
    } else {
        fprintf(stderr, "ERROR: enter d, w, or c.\n");
        return true;
    }
    if (next.wet_raw <= next.dry_raw) {
        fprintf(stderr, "ERROR: not saved: the wet point (%d) must be above the dry point (%d).\n"
                        "The sensor reads higher counts in wetter soil. Re-measure, or set the\n"
                        "other point first if it's still at a default.\n",
                (int)next.wet_raw, (int)next.dry_raw);
        return true;
    }
    *soil = next;
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
        show_menu(cfg, soil);
        char buf[64];
        enum read_result r = read_line(buf, sizeof buf);
        if (r == READ_EOF) {
            putchar('\n');
            return PS_EXIT_OK;  /* EOF, Ctrl-C, or SIGTERM at the menu */
        }
        int choice = 0;
        if (r == READ_OK && buf[0] >= '1' && buf[0] <= '7' && buf[1] == '\n')
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
            if (soil->backend == SOIL_BACKEND_GPIO)
                fprintf(stderr, "ERROR: soil moisture is not implemented yet\n");
            else if (!set_soil_optimal(cfg, soil))
                return PS_EXIT_OK;
            break;
        case 4:
            show_latest_log();
            break;
        case 5:
            switch (monitor_run(mon, cfg, soil)) {
            case MONITOR_STOPPED:   printf("\nMonitoring stopped.\n"); break;
            case MONITOR_TERMINATE: return PS_EXIT_OK;
            case MONITOR_E_CAMERA:  return PS_EXIT_CAMERA;
            case MONITOR_E_IO:      return PS_EXIT_IO;
            }
            break;
        case 6:
            return PS_EXIT_OK;
        case 7:
            if (!calibrate(cfg, soil, mon))
                return PS_EXIT_OK;
            break;
        default:
            fprintf(stderr, "ERROR: choose 1-7.\n");
            break;
        }
    }
}
