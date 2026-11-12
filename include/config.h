#ifndef PLANTSENSE_CONFIG_H
#define PLANTSENSE_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#include "light.h"

#define CONFIG_PATH "plantsense.conf"

#define CONFIG_INTERVAL_MIN     5      /* rpicam-still needs a few seconds */
#define CONFIG_INTERVAL_MAX     86400  /* once a day */
#define CONFIG_INTERVAL_DEFAULT 60
#define CONFIG_OPTIMAL_DEFAULT  ((fixed4_t)500000)  /* 50.0000 */

/* Soil calibration, raw seesaw counts (0-4095). Retune with menu item 7. */
#define CONFIG_SOIL_DRY_DEFAULT     200
#define CONFIG_SOIL_WET_DEFAULT     2000
#define CONFIG_SOIL_RAW_MAX         4095
#define CONFIG_SOIL_OPTIMAL_DEFAULT ((fixed4_t)500000)  /* 50.0000 */

enum soil_backend {
    SOIL_BACKEND_I2C = 0,  /* Adafruit STEMMA seesaw, the default */
    SOIL_BACKEND_GPIO,     /* 1-bit comparator via gpioget */
};

struct config {
    int interval_s;
    fixed4_t optimal_light;
};

/* Kept apart from struct config so existing positional initializers stay valid. */
struct soil_config {
    enum soil_backend backend;
    int32_t dry_raw;
    int32_t wet_raw;    /* always > dry_raw once loaded */
    fixed4_t optimal;   /* 0-100, same fixed point as light */
};

enum config_status {
    CONFIG_OK = 0,
    CONFIG_E_IO,
    CONFIG_E_MALFORMED,
};

void config_defaults(struct config *cfg);
void config_soil_defaults(struct soil_config *soil);

/* Whole decimal seconds in [MIN, MAX], surrounding whitespace allowed. */
int config_parse_interval(const char *s, int *out);
/* Whole number 0..CONFIG_SOIL_RAW_MAX, same whitespace rules. */
int config_parse_soil_raw(const char *s, int32_t *out);
/* Exactly "i2c" or "gpio", surrounding whitespace allowed. */
int config_parse_backend(const char *s, enum soil_backend *out);

/*
 * Missing file or key -> defaults and CONFIG_OK. On any error `cfg` and
 * `soil` are left untouched and `why` says which line was bad.
 *
 * File format, one setting per line, '#' comments:
 *   interval_seconds=60
 *   optimal_light=50.0000
 *   soil_backend=i2c          (i2c | gpio)
 *   soil_dry_raw=200
 *   soil_wet_raw=2000         (must be > soil_dry_raw)
 *   soil_optimal=50.0000
 */
int config_load_all(const char *path, struct config *cfg, struct soil_config *soil,
                    char *why, size_t why_len);

/*
 * Write via temp file + rename, so a power cut never leaves half a file.
 * save_all never writes a file that load_all would reject: an invalid
 * calibration pair (wet <= dry) is left out entirely, so the next load
 * uses the defaults for both.
 */
int config_save_all(const char *path, const struct config *cfg,
                    const struct soil_config *soil);

/*
 * Legacy wrappers, kept ONLY for the existing tests. Production code must
 * use the _all versions: config_save writes no soil keys, so calling it
 * would erase the user's soil settings. config_load still validates the
 * soil keys (and the dry/wet pair), then discards them.
 */
int config_load(const char *path, struct config *cfg, char *why, size_t why_len);
int config_save(const char *path, const struct config *cfg);

#endif
