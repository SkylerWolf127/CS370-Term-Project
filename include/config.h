#ifndef PLANTSENSE_CONFIG_H
#define PLANTSENSE_CONFIG_H

#include <stddef.h>

#include "light.h"

#define CONFIG_PATH "plantsense.conf"

#define CONFIG_INTERVAL_MIN     5      /* rpicam-still needs a few seconds */
#define CONFIG_INTERVAL_MAX     86400  /* once a day */
#define CONFIG_INTERVAL_DEFAULT 60
#define CONFIG_OPTIMAL_DEFAULT  ((fixed4_t)500000)  /* 50.0000 */

struct config {
    int interval_s;
    fixed4_t optimal_light;
};

enum config_status {
    CONFIG_OK = 0,
    CONFIG_E_IO,
    CONFIG_E_MALFORMED,
};

void config_defaults(struct config *cfg);

/* Whole decimal seconds in [MIN, MAX], surrounding whitespace allowed. */
int config_parse_interval(const char *s, int *out);

/*
 * Missing file -> defaults and CONFIG_OK. On any error `cfg` is left
 * untouched and `why` says which line was bad.
 *
 * File format, one setting per line, '#' comments:
 *   interval_seconds=60
 *   optimal_light=50.0000
 */
int config_load(const char *path, struct config *cfg, char *why, size_t why_len);

/* Write via temp file + rename, so a power cut never leaves half a file. */
int config_save(const char *path, const struct config *cfg);

#endif
