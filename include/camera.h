#ifndef PLANTSENSE_CAMERA_H
#define PLANTSENSE_CAMERA_H

#include <signal.h>
#include <stddef.h>

#include "light.h"

/* Found via PATH (execvp), so tests can substitute a fake. */
#define CAM_PROGRAM "rpicam-still"

#define CAM_WIDTH  640
#define CAM_HEIGHT 480

/*
 * Exposure is LOCKED. With auto-exposure on, the camera would brighten a
 * dark room and darken a sunny one until every photo looked "average",
 * normalizing away the very thing we're measuring. Fixed shutter + gain +
 * white-balance gains make pixel brightness track actual light.
 *
 * 10000 us is a whole number of 100 Hz flicker periods (50 Hz mains), which
 * keeps fluorescent/LED flicker from adding noise. Tune on the Pi: pick the
 * shortest shutter where the plant's brightest light doesn't saturate.
 */
#define CAM_SHUTTER_US "10000"
#define CAM_GAIN       "1.0"
#define CAM_AWB_GAINS  "1.0,1.0"  /* fixed red,blue gains: AWB off */

/* rpicam-still on a Pi 4 typically needs 1-3 s; this is the kill deadline. */
#define CAM_TIMEOUT_MS 15000

enum cam_status {
    CAM_OK = 0,
    CAM_E_TEMPFILE,  /* couldn't create the temp file */
    CAM_E_FORK,      /* fork or waitpid failed */
    CAM_E_EXEC,      /* rpicam-still missing or not executable */
    CAM_E_EXIT,      /* rpicam-still exited nonzero */
    CAM_E_SIGNALED,  /* rpicam-still died from a signal */
    CAM_E_TIMEOUT,   /* killed after timeout_ms */
    CAM_E_ABORTED,   /* abort_flag became nonzero; child killed */
    CAM_E_READ,      /* couldn't read the image file */
    CAM_E_TOO_BIG,   /* image larger than our fixed buffer */
    CAM_E_IMAGE,     /* image read but rejected by light_from_bmp */
};

/*
 * Capture one frame, compute its light level, delete the image.
 * The temp file is removed on every path, and the child is always reaped
 * before we return. On failure, `why` gets a human-readable reason.
 */
int camera_measure(fixed4_t *level, int timeout_ms,
                   const volatile sig_atomic_t *abort_flag,
                   char *why, size_t why_len);

#endif
