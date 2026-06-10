> Part of [**app-pixels.com**](https://www.app-pixels.com) — browse + flash this app at [`/apps/filehub`](https://www.app-pixels.com/apps/filehub).

# filehub

**Filehub** · v1.0.0

Browse, upload, and download SD-card files over WiFi.

**Hardware:** Waveshare ESP32-S3 1.8" AMOLED Touch

**Tags:** `#tool` `#wifi`

A three-mode SD-card file server.

- **WiFi mode** — joins your home WiFi and serves a WebDAV endpoint plus a small browser upload/download UI. Connect from any device on the same network.
- **Hotspot mode** — broadcasts its own access point. Same WebDAV + browser UI, but no router needed — useful on the go.
- **USB mode** — plug in over USB-C and the SD card mounts as a regular USB drive (same role as the **USB Stick** app).

WebDAV and the browser UI can be password-protected.

## Controls
- **BOOT** — cycle WiFi / Hotspot / USB
- Touch the on-screen URL — copy to clipboard

## `setup.txt` keys
**Mandatory** (depending on mode)
- WiFi mode: `SSID` / `PASSWORD`
- Hotspot mode: `AP_SSID` / `AP_PASS` (password min 8 chars)

**Optional**
- `WEBDAV_USER` / `WEBDAV_PASS` — basic-auth for WebDAV. Comment out for open access.
- `HTTP_UPLOAD_DIR`, `HTTP_DOWNLOAD_DIR`, `HTTP_PORT` — paths and port for the browser UI.

## Editing `setup.txt`
The device reads `/setup/setup.txt` from the SD card on boot. [Download a working sample](https://sosbxffigpteqilpgxwn.supabase.co/storage/v1/object/public/app-assets/setup/setup.txt) — covers every app — and edit the keys you need.

Don't want to eject the card? Use the [**USB Stick**](/apps/usb-stick) app (mounts the SD card as a USB drive over USB-C) or the [**Filehub**](/apps/filehub) app (edit over WiFi).

## Build

1. Install [arduino-cli](https://arduino.github.io/arduino-cli/) or Arduino IDE 2.x.
2. Add the ESP32 board package (≥ 3.1.0):

   ```
   arduino-cli core update-index --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
   arduino-cli core install esp32:esp32 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```

3. Install the required Arduino libraries:

   - GFX Library for Arduino (moononournation)
   - XPowersLib (lewishe)

4. Compile and upload:

   ```
   FQBN='esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,PSRAM=opi,FlashSize=16M,FlashMode=qio,PartitionScheme=app3M_fat9M_16MB,UploadSpeed=921600,LoopCore=1,EventsCore=1'
   arduino-cli compile -b "$FQBN" --build-path /tmp/filehub_build .
   arduino-cli upload  -b "$FQBN" --input-dir /tmp/filehub_build -p /dev/ttyACM0 .
   ```

   For browser flashing without a build environment, use the [pre-built binary](https://www.app-pixels.com/apps/filehub).

## License

MIT — see [LICENSE](LICENSE). Do whatever you want with it.

---

Part of the [app-pixels.com](https://www.app-pixels.com) catalogue · live listing: https://www.app-pixels.com/apps/filehub
