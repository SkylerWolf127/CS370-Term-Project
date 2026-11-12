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

void config_soil_defaults(struct soil_config *soil)
{
    soil->backend = SOIL_BACKEND_I2C;
    soil->dry_raw = CONFIG_SOIL_DRY_DEFAULT;
    soil->wet_raw = CONFIG_SOIL_WET_DEFAULT;
    soil->optimal = CONFIG_SOIL_OPTIMAL_DEFAULT;
}

/* Whole decimal number in [min, max], surrounding whitespace allowed. */
static int parse_whole(const char *s, long min, long max, long *out)
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
        if (v > max)
            return -1;
    }
    while (isspace((unsigned char)*s))
        s++;
    if (*s != '\0' || v < min)
        return -1;
    *out = v;
    return 0;
}

int config_parse_interval(const char *s, int *out)
{
    long v;
    if (!out || parse_whole(s, CONFIG_INTERVAL_MIN, CONFIG_INTERVAL_MAX, &v) != 0)
        return -1;
    *out = (int)v;
    return 0;
}

int config_parse_soil_raw(const char *s, int32_t *out)
{
    long v;
    if (!out || parse_whole(s, 0, CONFIG_SOIL_RAW_MAX, &v) != 0)
        return -1;
    *out = (int32_t)v;
    return 0;
}

int config_parse_backend(const char *s, enum soil_backend *out)
{
    if (!s || !out)
        return -1;
    while (isspace((unsigned char)*s))
        s++;
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1]))
        n--;
    if (n == 3 && strncmp(s, "i2c", 3) == 0)
        *out = SOIL_BACKEND_I2C;
    else if (n == 4 && strncmp(s, "gpio", 4) == 0)
        *out = SOIL_BACKEND_GPIO;
    else
        return -1;
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

int config_load_all(const char *path, struct config *cfg, struct soil_config *soil,
                    char *why, size_t why_len)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT) {
            config_defaults(cfg);
            if (soil)
                config_soil_defaults(soil);
            return CONFIG_OK;
        }
        snprintf(why, why_len, "cannot open: %s", strerror(errno));
        return CONFIG_E_IO;
    }

    struct config tmp;
    struct soil_config stmp;
    config_defaults(&tmp);
    config_soil_defaults(&stmp);
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
        else if (strcmp(key, "soil_backend") == 0)
            ok = config_parse_backend(val, &stmp.backend) == 0;
        else if (strcmp(key, "soil_dry_raw") == 0)
            ok = config_parse_soil_raw(val, &stmp.dry_raw) == 0;
        else if (strcmp(key, "soil_wet_raw") == 0)
            ok = config_parse_soil_raw(val, &stmp.wet_raw) == 0;
        else if (strcmp(key, "soil_optimal") == 0)
            ok = light_parse_level(val, &stmp.optimal) == 0;
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
    if (rc == CONFIG_OK && stmp.wet_raw <= stmp.dry_raw) {
        snprintf(why, why_len, "soil_wet_raw (%d) must be greater than soil_dry_raw (%d)",
                 (int)stmp.wet_raw, (int)stmp.dry_raw);
        rc = CONFIG_E_MALFORMED;
    }
    if (rc == CONFIG_OK && ferror(f)) {
        snprintf(why, why_len, "read error");
        rc = CONFIG_E_IO;
    }
    fclose(f);
    if (rc == CONFIG_OK) {
        *cfg = tmp;
        if (soil)
            *soil = stmp;
    }
    return rc;
}

int config_load(const char *path, struct config *cfg, char *why, size_t why_len)
{
    return config_load_all(path, cfg, NULL, why, why_len);
}

int config_save_all(const char *path, const struct config *cfg,
                    const struct soil_config *soil)
{
    char tmp[512];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp)
        return CONFIG_E_IO;

    FILE *f = fopen(tmp, "w");
    if (!f)
        return CONFIG_E_IO;
    char level[FIXED4_STR_LEN], sopt[FIXED4_STR_LEN];
    int bad = fprintf(f, "# plantsense settings\ninterval_seconds=%d\noptimal_light=%s\n",
                      cfg->interval_s,
                      fixed4_format(cfg->optimal_light, level, sizeof level)) < 0;
    if (soil) {
        /* See header: anything load_all would reject is omitted, not written. */
        bad |= fprintf(f, "soil_backend=%s\n",
                       soil->backend == SOIL_BACKEND_GPIO ? "gpio" : "i2c") < 0;
        if (soil->optimal >= 0 && soil->optimal <= LIGHT_LEVEL_MAX)
            bad |= fprintf(f, "soil_optimal=%s\n",
                           fixed4_format(soil->optimal, sopt, sizeof sopt)) < 0;
        if (soil->dry_raw >= 0 && soil->wet_raw > soil->dry_raw
            && soil->wet_raw <= CONFIG_SOIL_RAW_MAX)
            bad |= fprintf(f, "soil_dry_raw=%d\nsoil_wet_raw=%d\n",
                           (int)soil->dry_raw, (int)soil->wet_raw) < 0;
    }
    bad |= fflush(f) != 0;
    bad |= fsync(fileno(f)) != 0;
    bad |= fclose(f) != 0;
    if (bad || rename(tmp, path) != 0) {
        unlink(tmp);
        return CONFIG_E_IO;
    }
    return CONFIG_OK;
}

int config_save(const char *path, const struct config *cfg)
{
    return config_save_all(path, cfg, NULL);
}
