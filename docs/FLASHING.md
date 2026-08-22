# Flashing the device — SOP

Target: **Zhengchen ZC-1.54TFT-WiFi** (ESP32-S3, CH340K USB bridge). Every fact here was
learned the hard way; follow exactly.

## Pick your lane

Two independent choices — where you **build**, and where the **USB cable** is:

| | Build | Flash |
|---|---|---|
| **Local** (one machine does both) | `idf.py build` on the machine with the cable | `idf.py flash` or the esptool command below |
| **Remote build** (this project's setup) | a big Linux box compiles | machine with the cable flashes the copied binaries |

Building needs ESP-IDF (~2 GB, slow first compile). Flashing needs only `esptool` — a
5 MB pip/brew install. Splitting them keeps laptops light and builds fast.

## One-time host setup

### macOS (flashing host)
1. `brew install esptool`
2. Driver for the CH340K: `brew install --cask wch-ch34x-usb-serial-driver`, then launch
   **CH34xVCPDriver.app** → Install, and approve it under *System Settings → General →
   Login Items & Extensions → Driver Extensions*. Re-plug after approving.
3. Port appears as `/dev/cu.wchusbserial*`. No port? See "USB link quirks".

### Linux (flashing host)
1. `pipx install esptool` (or distro package)
2. No driver needed — the `ch341` kernel module is built in. Port appears as `/dev/ttyUSB0`.
3. Permission denied on the port: `sudo usermod -aG dialout $USER` (or `uucp` on Arch),
   then re-login.

### Linux (build host — local or remote)
```bash
mkdir -p ~/esp && cd ~/esp
git clone --depth 1 -b v5.5.1 --recursive --shallow-submodules https://github.com/espressif/esp-idf
cd esp-idf && ./install.sh esp32s3
python3 tools/idf_tools.py install cmake ninja     # install.sh sometimes skips these
```
Note: `export.sh` behaves best under **bash** — over ssh, wrap commands in `bash -lc "…"`.

## Build

**Local:**
```bash
source ~/esp/esp-idf/export.sh
cd firmware && cp main/secrets.h.example main/secrets.h   # edit Wi-Fi + server URL
idf.py set-target esp32s3 && idf.py build
```

**Remote (from the repo root on your laptop; `buildbox` = your build host):**
```bash
rsync -a --exclude build firmware/ buildbox:crybaby-firmware/
ssh buildbox 'bash -lc "source ~/esp/esp-idf/export.sh >/dev/null && cd ~/crybaby-firmware && idf.py set-target esp32s3 && idf.py build"'
scp buildbox:crybaby-firmware/build/{crybaby.bin,bootloader/bootloader.bin,partition_table/partition-table.bin} build-artifacts/
```

## Enter USB download mode (always manual on this board)

The straps hide on the case buttons — **middle = GPIO0 (boot)**, **left = power/reset**:

> Plug in USB → **hold middle** → **tap power** → keep holding middle ~2 s → release.
> Screen goes black **and stays black** = ready. UI comes back = retry.

Best practice: **keep holding the middle button until esptool starts writing.** The
port-open RTS pulse resets the chip; with GPIO0 still held low the reset lands back in
download mode instead of booting the app (the usual cause of "Invalid head of packet").
If the port disappears from `/dev` entirely, re-seat the USB-C plug flipped 180° first.

## Flash (single invocation — this is a hard rule)

RTS is hardwired to chip reset, and opening the serial port pulses RTS. Consequence:
**every esptool invocation reboots the chip out of download mode**, so all images must go
in ONE command. Multi-step scripts will never work on this board.

```bash
# macOS:  PORT=/dev/cu.wchusbserial110      Linux:  PORT=/dev/ttyUSB0
esptool --port $PORT --baud 921600 --connect-attempts 20 write-flash \
  0x0     build-artifacts/bootloader.bin \
  0x8000  build-artifacts/partition-table.bin \
  0x10000 build-artifacts/crybaby.bin
```

Success = `Hash of data verified` three times, then an automatic reset into the app.
(`idf.py flash` on a local build works too — it issues exactly one invocation.)

**Baud**: 921600 is verified good on this board — 15.3 s vs 41.9 s at 230400 (771 vs
282 kbit/s), hash verified. Writes are what's reliable here; the read flakiness in
"USB link quirks" below is unchanged by baud. If a flash ever fails mid-write, step
down 921600 → 460800 → 230400 (each retry needs a fresh download-mode entry, since the
failed invocation already reset the chip).

**Single-file alternative** — merge once, then flashing is one file at one offset
(ideal for releases and for handing to someone without the repo):
```bash
esptool --chip esp32s3 merge-bin -o crybaby-full.bin --flash-size 16MB \
  0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 crybaby.bin
esptool --port $PORT --baud 921600 --connect-attempts 20 write-flash 0x0 crybaby-full.bin
```

## USB link quirks (when nothing works)

1. **Green LED proves nothing** — it runs off the battery. The bridge chip must appear on
   the USB bus: macOS `ioreg -p IOUSB | grep -i serial` · Linux `lsusb | grep 1a86`.
2. **USB-C data pins are single-sided** on this board — flip the plug 180°.
3. **Charge-only cables are everywhere** — if the bus stays empty, swap cables first.
4. **Bulk reads (device→host) are unreliable** — writes are solid (verified to 921600),
   but full-flash dumps may die mid-stream. Keep reads small (the 24 KB NVS backup works).
5. **Serial monitoring**: deassert DTR **and** RTS before opening the port (pyserial:
   `s = serial.Serial(); s.rts = s.dtr = False; s.port = PORT; s.open()`), otherwise the
   chip is held in reset and you read nothing. `idf.py monitor` handles this correctly.

## Recovery

- The button sequence reaches silicon ROM — the device **cannot be bricked** by any flash.
- Stock chatbot restore: build board `zhengchen-1.54tft-wifi` at
  https://xiaozhi.me/console/firmware-builder, flash it (same single-invocation rule),
  then restore identity: `esptool write-flash 0x9000 backup/stock-nvs.bin`
  (NVS backup lives outside git, on the original machine).
