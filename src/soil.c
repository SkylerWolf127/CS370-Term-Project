#define _POSIX_C_SOURCE 200809L

#include "soil.h"

#include <time.h>

#include "proc.h"
#include "sig.h"

/* Room for "0\n" plus a little, so overlong output is caught, not cut. */
#define SOIL_OUT_MAX 16

int soil_parse(const char *buf, size_t len, int *raw_out)
{
    if (!buf || !raw_out)
        return -1;
    if (len == 2 && buf[1] != '\n')
        return -1;
    if (len != 1 && len != 2)
        return -1;
    if (buf[0] != '0' && buf[0] != '1')
        return -1;
    *raw_out = buf[0] - '0';
    return 0;
}

static int gpioget_read_raw(int *raw_out)
{
    char *const argv[] = {
        SOIL_PROGRAM, "--numeric", "-c", SOIL_GPIO_CHIP, SOIL_GPIO_LINE, NULL,
    };
    char out[SOIL_OUT_MAX];
    size_t got = 0;

    switch (proc_run(argv, SOIL_TIMEOUT_MS, sig_flag(), out, sizeof out, &got,
                     NULL, 0)) {
    case PROC_OK:         break;
    case PROC_E_EXEC:     return SOIL_E_EXEC;
    case PROC_E_EXIT:     return SOIL_E_EXIT;
    case PROC_E_SIGNALED: return SOIL_E_SIGNALED;
    case PROC_E_TIMEOUT:  return SOIL_E_TIMEOUT;
    case PROC_E_ABORTED:  return SOIL_E_ABORTED;
    case PROC_E_OUTPUT:   return SOIL_E_PARSE;
    default:              return SOIL_E_FORK;
    }
    return soil_parse(out, got, raw_out) == 0 ? SOIL_OK : SOIL_E_PARSE;
}

const soil_ops_t soil_gpioget_ops = { .read_raw = gpioget_read_raw };

int soil_read(const soil_ops_t *ops, soil_state_t *out)
{
    if (!ops || !ops->read_raw || !out)
        return SOIL_E_ARG;

    int dry_votes = 0;
    for (int i = 0; i < SOIL_SAMPLES; i++) {
        if (i > 0) {
            struct timespec gap = { 0, SOIL_SAMPLE_GAP_MS * 1000L * 1000L };
            nanosleep(&gap, NULL);  /* EINTR just shortens the gap */
        }
        int raw = -1;
        int rc = ops->read_raw(&raw);
        if (rc != 0)
            return rc;
        if (raw != 0 && raw != 1)
            return SOIL_E_PARSE;
        if (raw == SOIL_RAW_DRY)
            dry_votes++;
    }
    *out = dry_votes * 2 > SOIL_SAMPLES ? SOIL_DRY : SOIL_WET;
    return SOIL_OK;
}

const char *soil_strerror(int status)
{
    switch (status) {
    case SOIL_OK:         return "ok";
    case SOIL_E_ARG:      return "bad argument";
    case SOIL_E_FORK:     return "fork failed";
    case SOIL_E_EXEC:     return SOIL_PROGRAM " not found";
    case SOIL_E_EXIT:     return SOIL_PROGRAM " failed";
    case SOIL_E_SIGNALED: return SOIL_PROGRAM " killed";
    case SOIL_E_TIMEOUT:  return SOIL_PROGRAM " timeout";
    case SOIL_E_ABORTED:  return "interrupted";
    case SOIL_E_PARSE:    return "bad " SOIL_PROGRAM " output";
    default:              return "read failed";
    }
}

const char *soil_state_str(soil_state_t s)
{
    return s == SOIL_DRY ? "DRY" : "WET";
}

int soil_state_raw(soil_state_t s)
{
    return s == SOIL_DRY ? SOIL_RAW_DRY : SOIL_RAW_WET;
}

const char *soil_verdict_str(soil_state_t s)
{
    return s == SOIL_DRY ? "NEEDS WATER" : "SOIL OK";
}
