# CS370-Term-Project
## An embedded Raspberry Pi Application to do the thing!

Brought to you by: The C<sup>(-1)</sup>-Team
*Bringing you "functional" software since 2024*

## AI Disclosure 
As part of the assignment description and requirements, Claude Code was required in the workflow. 


# Supported Hardware
* TBD

# Supported Operating System(s)
* TBD


# Build instructions and requirements 
* `make` builds `build/plantsense`; `make test` runs the hardware-free unit tests.
* On the Pi, `rpicam-still` (Raspberry Pi OS Bookworm) must be on `PATH`.
* Exit codes are documented in `include/exit_codes.h`.

# Soil sensor (Adafruit STEMMA Soil Sensor, I2C, the default backend)
* Wire VIN to 3.3 V, GND, SDA to pin 3, SCL to pin 5. It appears at `0x36` on `/dev/i2c-1`.
* Enable I2C (`sudo raspi-config` -> Interface Options -> I2C) and add your user to the
  `i2c` group (`sudo usermod -aG i2c $USER`, then log out and back in).
* Calibrate with menu item 7: hold the probe in dry air (or dry soil) -> `d`, then in
  water (or freshly watered soil) -> `w`. Values are saved to `plantsense.conf`.
* `soil_backend=gpio` in `plantsense.conf` switches back to the old 1-bit gpioget sensor.
