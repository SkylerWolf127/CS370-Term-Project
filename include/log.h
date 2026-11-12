#ifndef PLANTSENSE_LOG_H
#define PLANTSENSE_LOG_H

#include <stdio.h>
#include <time.h>

#include "light.h"
#include "soil.h"
#include "soil_i2c.h"

#define LOG_DIR      "logs"
#define LOG_LINE_MAX 320  /* light part + soil part */
#define LOG_PATH_MAX 256

/* One file per run: <dir>/session_YYYYMMDD_HHMMSS.txt */
struct session_log {
    FILE *fp;
    char path[LOG_PATH_MAX];
};

/* Creates `dir` if missing. Returns 0, or -1 with errno set. */
int log_open(struct session_log *log, const char *dir, time_t started);
int log_close(struct session_log *log);

/* "2026-10-26T16:05:00 level=42.1337 avg=41.9020 NEEDS MORE LIGHT" */
void log_format_reading(char *buf, size_t len, time_t when, fixed4_t level,
                        fixed4_t avg, enum light_verdict v);
/* "2026-10-26T16:05:00 ERROR capture failed: <reason>" */
void log_format_failure(char *buf, size_t len, time_t when, const char *reason);
/* Append " soil=DRY raw=1 NEEDS WATER", or " soil=ERROR(<reason>)" when
 * soil_rc is nonzero, to a line made by one of the two above. */
void log_append_soil(char *buf, size_t len, int soil_rc, soil_state_t state);
/* I2C backend: append " soil=57.1234 raw=412 temp=21.5 SOIL OK" (temp=NA
 * when !r->temp_ok), or " soil=ERROR(<err>)" when `err` is non-NULL. */
void log_append_soil_i2c(char *buf, size_t len, const char *err,
                         const struct soil_i2c_reading *r, const char *verdict);

/* Append one line (no trailing newline needed), then flush and fsync. */
int log_write_line(struct session_log *log, const char *line);

/* Newest <dir>/session_*.txt. 0 found, 1 none, -1 error. */
int log_find_newest(const char *dir, char *out, size_t len);

/* Copy a file to `out`. 0 ok, -1 error. */
int log_print_file(const char *path, FILE *out);

#endif
