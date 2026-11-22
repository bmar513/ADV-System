# ADV System r1.2 integration kit

This is a **source package, not an installable firmware**. See `BUILD_STATUS.txt` before flashing anything.

Contains the six-pane ADV System with Music, SD, Sequencer, Notes, DIAG and experimental FLASH installer. The FLASH installer is intended to write compatible ESP32-S3 app-only `.bin` files from `/Firmware/` to the `ota_0` partition, set a one-shot boot flag, and reboot. The custom bootloader is intended to return to factory ADV System after reset. **Neither path has been hardware-validated in this integration.**

The SD card does not need repartitioning. The included `partitions.csv` is for internal ESP32-S3 flash.

The included `.ino` is an Arduino sketch. The bootloader needs an ESP-IDF build toolchain; do not use the ordinary Arduino Upload button as the complete installation method.

See `DEPENDENCIES.txt`, `README_BUILD_FIRST.md`, and `BUILD_STATUS.txt`.
