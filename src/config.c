#define _POSIX_C_SOURCE 200809L

#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void config_defaults(struct config *cfg)
{
    cfg->interval_s = CONFIG_INTERVAL_DEFAULT;
    cfg->optimal_light = CONFIG_OPTIMAL_DEFAULT;
}

int config_parse_interval(const char *s, int *out)
{
    if (!s || !out)
        return -1;
    while (isspace((unsigned char)*s))
        s++;
    if (!isdigit((unsigned char)*s))
        return -1;
    long v = 0;
    for (; isdigit((unsigned char)*s); s++) {
        v = v * 10 + (*s - '0');
        if (v > CONFIG_INTERVAL_MAX)
            return -1;
    }
    while (isspace((unsigned char)*s))
        s++;
    if (*s != '\0' || v < CONFIG_INTERVAL_MIN)
        return -1;
    *out = (int)v;
    return 0;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

int config_load(const char *path, struct config *cfg, char *why, size_t why_len)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT) {
            config_defaults(cfg);
            return CONFIG_OK;
        }
        snprintf(why, why_len, "cannot open: %s", strerror(errno));
        return CONFIG_E_IO;
    }

    struct config tmp;
    config_defaults(&tmp);
    char line[256];
    int lineno = 0;
    int rc = CONFIG_OK;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        if (!strchr(line, '\n') && !feof(f)) {
            snprintf(why, why_len, "line %d: too long", lineno);
            rc = CONFIG_E_MALFORMED;
            break;
        }
        char *s = trim(line);
        if (*s == '\0' || *s == '#')
            continue;
        char *eq = strchr(s, '=');
        if (!eq) {
            snprintf(why, why_len, "line %d: expected key=value", lineno);
            rc = CONFIG_E_MALFORMED;
            break;
        }
        *eq = '\0';
        const char *key = trim(s);
        const char *val = eq + 1;
        int ok;
        if (strcmp(key, "interval_seconds") == 0)
            ok = config_parse_interval(val, &tmp.interval_s) == 0;
        else if (strcmp(key, "optimal_light") == 0)
            ok = light_parse_level(val, &tmp.optimal_light) == 0;
        else {
            snprintf(why, why_len, "line %d: unknown key '%s'", lineno, key);
            rc = CONFIG_E_MALFORMED;
            break;
        }
        if (!ok) {
            snprintf(why, why_len, "line %d: bad value for %s", lineno, key);
            rc = CONFIG_E_MALFORMED;
            break;
        }
    }
    if (rc == CONFIG_OK && ferror(f)) {
        snprintf(why, why_len, "read error");
        rc = CONFIG_E_IO;
    }
    fclose(f);
    if (rc == CONFIG_OK)
        *cfg = tmp;
    return rc;
}

int config_save(const char *path, const struct config *cfg)
{
    char tmp[512];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp)
        return CONFIG_E_IO;

    FILE *f = fopen(tmp, "w");
    if (!f)
        return CONFIG_E_IO;
    char level[FIXED4_STR_LEN];
    int bad = fprintf(f, "# plantsense settings\ninterval_seconds=%d\noptimal_light=%s\n",
                      cfg->interval_s,
                      fixed4_format(cfg->optimal_light, level, sizeof level)) < 0;
    bad |= fflush(f) != 0;
    bad |= fsync(fileno(f)) != 0;
    bad |= fclose(f) != 0;
    if (bad || rename(tmp, path) != 0) {
        unlink(tmp);
        return CONFIG_E_IO;
    }
    return CONFIG_OK;
}
