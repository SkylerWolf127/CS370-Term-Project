# CS370-Term-Project
## An embedded Raspberry Pi Application to do the thing!
-----
# Plantsense Plant Sensor
*An easy way to keep an eye on your plant and see if it needs some more attention* 

Brought to you by: The C<sup>(-1)</sup>-Team
*Bringing you "functional" software since 2024*

## AI Disclosure 
As part of the assignment description and requirements, Claude Code was required in the workflow.
Claude Code was used in creating testing and assisting with development. Claude Code is NOT present in the final executable and was only present in the development pipeline. 


# Supported Hardware
* Raspberry Pi Model 4B (4GB)
* Adafruit STEMMA Soil Sensor
* Raspberry Pi Camera or compatible. (Must interface through the Pi's onboard Camera Connector)

# Supported Operating System(s)
* Raspberry Pi OS 64-bit (Debian 13)
* Theoretically any Debian based OS should work if the requisite packages are installed. (RPI Cam, interfacing with GPIO) 


# Build instructions and requirements 
* `make` builds `build/plantsense`; `make test` runs the hardware-free unit tests.
* On the Pi, `rpicam-still` (Raspberry Pi OS Bookworm) must be on `PATH`.
* Exit codes are documented in `include/exit_codes.h`.
