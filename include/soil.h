#ifndef PLANTSENSE_SOIL_H
#define PLANTSENSE_SOIL_H

/*
 * Soil moisture. Today: an analog sensor's digital comparator output on a
 * GPIO line, so we only get 1 bit. The driver sits behind soil_ops_t so a
 * proper digital sensor can replace it without touching the pipeline.
 */

#include <stddef.h>

/* Wiring: GPIO17 on gpiochip0, physical pin 11. */
#define SOIL_GPIO_CHIP "gpiochip0"
#define SOIL_GPIO_LINE "17"

/* Found via PATH (execvp), so tests can substitute a fake. libgpiod v2. */
#define SOIL_PROGRAM "gpioget"

/* Polarity: the raw level that means DRY. Flip here if the sensor changes. */
#define SOIL_RAW_DRY 1
#define SOIL_RAW_WET (!SOIL_RAW_DRY)

/* Debounce: majority of SOIL_SAMPLES reads, SOIL_SAMPLE_GAP_MS apart. Odd. */
#define SOIL_SAMPLES       3
#define SOIL_SAMPLE_GAP_MS 50
static_assert(SOIL_SAMPLES % 2 == 1, "majority vote needs an odd sample count");

/* gpioget returns in milliseconds; this is the kill deadline per sample. */
#define SOIL_TIMEOUT_MS 2000

typedef enum { SOIL_WET, SOIL_DRY } soil_state_t;

typedef struct {
    int (*read_raw)(int *raw_out);  /* 0 on success, nonzero error */
} soil_ops_t;

/* Nonzero results of read_raw and soil_read. */
enum soil_status {
    SOIL_OK = 0,
    SOIL_E_ARG,       /* null pointer */
    SOIL_E_FORK,      /* fork, pipe, or waitpid failed */
    SOIL_E_EXEC,      /* gpioget missing or not executable (e.g. not a Pi) */
    SOIL_E_EXIT,      /* gpioget exited nonzero */
    SOIL_E_SIGNALED,  /* gpioget died from a signal */
    SOIL_E_TIMEOUT,   /* gpioget killed after SOIL_TIMEOUT_MS */
    SOIL_E_ABORTED,   /* our SIGINT/SIGTERM arrived mid-read */
    SOIL_E_PARSE,     /* output was not exactly "0" or "1" */
};

/* The real driver: runs `gpioget --numeric -c gpiochip0 17`. */
extern const soil_ops_t soil_gpioget_ops;

/*
 * SOIL_SAMPLES reads, majority wins. Any failed read fails the whole thing
 * with that read's status, and `*out` is left untouched (never stale).
 */
int soil_read(const soil_ops_t *ops, soil_state_t *out);

/* Strict: "0" or "1", optionally followed by one '\n'. 0 ok, -1 rejected. */
int soil_parse(const char *buf, size_t len, int *raw_out);

const char *soil_strerror(int status);    /* "gpioget timeout", ... */
const char *soil_state_str(soil_state_t s);   /* "DRY" / "WET" */
int soil_state_raw(soil_state_t s);           /* polarity applied */
const char *soil_verdict_str(soil_state_t s); /* "NEEDS WATER" / "SOIL OK" */

#endif
