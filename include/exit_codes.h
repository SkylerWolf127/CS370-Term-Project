#ifndef PLANTSENSE_EXIT_CODES_H
#define PLANTSENSE_EXIT_CODES_H

/*
 * Process exit codes, for supervisors (systemd, scripts) to act on.
 * 1 is deliberately unused so it never collides with a generic failure.
 */
enum plantsense_exit {
    PS_EXIT_OK     = 0,  /* menu Exit, EOF on stdin, Ctrl-C at the menu, SIGTERM */
    PS_EXIT_CONFIG = 2,  /* plantsense.conf unreadable or malformed */
    PS_EXIT_CAMERA = 3,  /* MONITOR_MAX_CAMERA_FAILURES captures failed in a row */
    PS_EXIT_IO     = 4,  /* session log write failed, or signal setup failed */
    PS_EXIT_SOIL   = 5,  /* soil sensor / GPIO (gpioget) error. Reserved: soil
                          * failures are logged per tick and never stop the
                          * loop, so nothing returns this yet. */
    PS_EXIT_I2C    = 6,  /* soil sensor I2C bus error (/dev/i2c-1, seesaw at
                          * 0x36). Reserved for the same reason as
                          * PS_EXIT_SOIL: I2C failures are logged per tick
                          * as soil=ERROR(<reason>) and the loop continues. */
};

#endif
