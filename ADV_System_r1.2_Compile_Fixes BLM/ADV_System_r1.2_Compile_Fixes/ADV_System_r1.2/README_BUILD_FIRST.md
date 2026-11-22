# ADV System r1.2 — FLASH beta SOURCE (not a verified binary release)

This is a source integration, **NOT a compiled or hardware-verified image**. Do not flash the custom bootloader or partition table until you have built and checked both components.

## Contents
- `ADV_System_r1.2_FLASH_Beta_Source.ino` — ADV System with FLASH selection, confirmation, OTA installer, and one-shot boot flag.
- `partitions.csv`, `bootloader_components/`, `bl_sdkconfig.defaults`, `build-bootloader.sh` — adapted unchanged from the uploaded MIT-licensed M5Cardputer Loader project (copyright p0rtm0lester).
- Existing local audio headers, dependencies and ESP8266Audio compatibility notes.

## Architecture
- Factory at `0x10000` (0x160000 bytes) contains ADV System.
- `ota_0` at `0x170000` (0x480000 bytes) receives selected `/Firmware/*.bin`.
- Bootflag at `0xd000` is set only after the update completes.
- Custom bootloader consumes bootflag and permits one launch; on subsequent reset it clears otadata and returns to factory.
- No app or filesystem partition is overwritten intentionally other than ota_0.
- Only standalone ESP32-S3 **application binaries** are supported in this beta; merged/full-flash images and apps needing their own partition layout are unsupported. Firmware downloaded from third parties is untrusted; it can access raw flash and potentially overwrite the launcher. There is no hardware-enforced protection.

## Important build caveat
Arduino IDE's default Upload button will install a stock bootloader and/or a different partition table if not configured correctly, defeating RESET-return behavior. This package includes source, **not** a ready-to-flash combined image. Use a matched PlatformIO/ESP-IDF toolchain to build the application and hooked bootloader, and verify the app fits the factory partition. The PlatformIO configuration is provided as a starting point, **not build-validated**.

The standalone bootloader build script is inherited from the loader project; it expects a pioarduino/ESP-IDF toolchain. The firmware must be compiled and the resulting images inspected before full flashing.

### Licensing
The bootloader hook and partition layout are derived from the uploaded M5Cardputer Loader source under MIT license; see LICENSE and THIRD-PARTY-NOTICES.md.
