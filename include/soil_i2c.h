#ifndef PLANTSENSE_SOIL_I2C_H
#define PLANTSENSE_SOIL_I2C_H

/*
 * Adafruit STEMMA Soil Sensor (product 4026): capacitive moisture plus a
 * chip temperature, over I2C, speaking Adafruit's "seesaw" protocol.
 *
 * Every seesaw read is two separate bus transactions: write the register
 * pair (base, function), wait for the firmware to oversample, then read N
 * bytes. The transport for that sits behind soil_i2c_ops_t so tests can
 * swap in a fake; no test touches real hardware.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "light.h"

/* Wiring: VIN on 3.3 V, SDA/SCL on the Pi's I2C1 (pins 3/5). */
#define SOIL_I2C_BUS  "/dev/i2c-1"
#define SOIL_I2C_ADDR 0x36

/* seesaw registers: (base, function) pairs. */
#define SEESAW_STATUS_BASE  0x00
#define SEESAW_STATUS_HW_ID 0x01  /* 1 byte */
#define SEESAW_STATUS_TEMP  0x04  /* 4 bytes, big-endian, 16.16 fixed point */
#define SEESAW_TOUCH_BASE   0x0F
#define SEESAW_TOUCH_CH0    0x10  /* 2 bytes, big-endian */

#define SEESAW_HW_ID_EXPECTED 0x55
#define SEESAW_TEMP_TOP_MASK  0x3F  /* top 2 bits of the first temp byte are junk */

/* Anything above this is the firmware saying "not ready / bad sample". */
#define SOIL_I2C_RAW_MAX 4095

/*
 * Timing. The firmware oversamples, so if reads come back invalid on the
 * Pi, lengthen these first.
 */
#define SOIL_I2C_READ_DELAY_US 5000  /* register write -> data read, >= 5 ms */
#define SOIL_I2C_RETRY_GAP_US  1000  /* between retries of a bad sample, >= 1 ms */
#define SOIL_I2C_RETRIES       3     /* so up to 1 + 3 tries per sample */

/* SOIL_I2C_SAMPLES samples, SOIL_I2C_SAMPLE_GAP_MS apart; the median wins. */
#define SOIL_I2C_SAMPLES       3
#define SOIL_I2C_SAMPLE_GAP_MS 50
static_assert(SOIL_I2C_SAMPLES % 2 == 1, "median needs an odd sample count");

/* |moisture - optimal| <= this is SOIL OK. 5.0000 percentage points. */
#define SOIL_I2C_DEADBAND ((fixed4_t)50000)

enum soil_i2c_status {
    SOIL_I2C_OK = 0,
    SOIL_I2C_E_ARG,          /* null pointer */
    SOIL_I2C_E_CAL,          /* calibration with wet <= dry */
    SOIL_I2C_E_OPEN,         /* open/ioctl on the bus failed; `why` has details */
    SOIL_I2C_E_NODEV,        /* nothing ACKed at SOIL_I2C_ADDR */
    SOIL_I2C_E_HWID,         /* something answered, but it isn't a seesaw */
    SOIL_I2C_E_SHORT_READ,
    SOIL_I2C_E_SHORT_WRITE,
    SOIL_I2C_E_INVALID,      /* raw > SOIL_I2C_RAW_MAX after every retry */
    SOIL_I2C_E_IO,           /* any other bus error */
    SOIL_I2C_E_ABORTED,      /* our SIGINT/SIGTERM arrived mid-read */
};

/*
 * Transport. Each returns SOIL_I2C_OK or a soil_i2c_status.
 * `ctx` is whatever the transport needs (the real one: a soil_i2c_bus).
 */
typedef struct {
    int (*write_reg)(void *ctx, uint8_t base, uint8_t func);
    int (*delay_us)(void *ctx, long us);
    int (*read)(void *ctx, uint8_t *buf, size_t n);
} soil_i2c_ops_t;

/* The real transport: /dev/i2c-dev, write()/read(), nanosleep. */
struct soil_i2c_bus {
    int fd;
};
extern const soil_i2c_ops_t soil_i2c_linux_ops;

struct soil_i2c_reading {
    int32_t raw;        /* median raw count, 0..SOIL_I2C_RAW_MAX */
    fixed4_t moisture;  /* 0 (dry point) .. 1000000 (wet point), units 0.0001 % */
    int32_t temp_x10;   /* tenths of a degree C; only meaningful if temp_ok */
    bool temp_ok;       /* false -> log temp=NA. Informational: +/-2 C */
};

/*
 * Open `path` and select `addr`. On failure the fd is already closed and
 * `why` says what to do about it (e.g. join the i2c group).
 */
int soil_i2c_bus_open(struct soil_i2c_bus *bus, const char *path, int addr,
                      char *why, size_t why_len);
void soil_i2c_bus_close(struct soil_i2c_bus *bus);

/*
 * One full reading: hardware ID (only while *id_checked is false; set on a
 * match), SOIL_I2C_SAMPLES moisture samples -> median, then temperature.
 * Any moisture failure fails the whole reading and leaves `*out` untouched
 * (never stale). A temperature failure alone just clears temp_ok.
 * On failure `why` gets a human-readable reason.
 */
int soil_i2c_read(const soil_i2c_ops_t *ops, void *ctx, bool *id_checked,
                  int32_t dry_raw, int32_t wet_raw,
                  struct soil_i2c_reading *out, char *why, size_t why_len);

/* soil_i2c_read on the real bus, opened and closed within the call. */
int soil_i2c_measure(bool *id_checked, int32_t dry_raw, int32_t wet_raw,
                     struct soil_i2c_reading *out, char *why, size_t why_len);

/* Big-endian count; SOIL_I2C_E_INVALID (out untouched) if > RAW_MAX. */
int soil_i2c_decode_moisture(const uint8_t b[2], int32_t *raw_out);

/* 4 seesaw temperature bytes -> tenths of a degree C, rounded half up. */
int32_t soil_i2c_decode_temp_x10(const uint8_t b[4]);

/*
 * Linear map: raw <= dry -> 0.0000, raw >= wet -> 100.0000, rounded half
 * up at the 4th decimal. -1 (out untouched) if wet <= dry.
 */
int soil_i2c_moisture(int32_t raw, int32_t dry_raw, int32_t wet_raw, fixed4_t *out);

/* "NEEDS WATER" / "TOO WET" / "SOIL OK" for the current reading. */
const char *soil_i2c_verdict(fixed4_t moisture, fixed4_t optimal, fixed4_t deadband);

const char *soil_i2c_strerror(int status);

#endif
