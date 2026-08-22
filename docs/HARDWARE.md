# Hardware — device capabilities (fully identified)

Probed 2026-08-22 via esptool 5.3.1 + boot-log forensics. Board identified by matching
firmware log signatures (`NoAudioCodec Simplex`, `PowerManager: Temperature updated`)
against xiaozhi-esp32 mainline board definitions.

## Identity

**Zhengchen ZC-1.54TFT-WiFi** (Taobao item CZ356), sold rebranded — this unit broadcasts
config AP "InterPower-2CD5". Runs stock **xiaozhi-esp32** (小智 AI chatbot) firmware.
Board definition: `xiaozhi-esp32/main/boards/zhengchen/1.54tft-wifi/` (has `config.h`,
`power_manager.h`, display class — reuse these pin defs verbatim in our firmware).

## SoC / memory

| | |
|---|---|
| SoC | ESP32-S3 (QFN56, rev v0.2), dual LX7 @ 240 MHz + LP core, vector ISA |
| PSRAM | 8 MB embedded (AP_3v3) |
| Flash | 16 MB (mfr 0x68, dev 0x4018) |
| Radio | Wi-Fi 2.4 GHz b/g/n, BLE 5 |
| MAC | 14:c1:9f:3b:2c:d4 |
| USB | CH340K on UART0 (0x1A86:0x7522) — S3 native USB not exposed |

## Peripherals (pin map from zhengchen board config)

| Subsystem | Pins | Notes |
|---|---|---|
| I2S mic (std, INMP441-class) | WS=4, SCK=5, DIN=6 | 16 kHz capture — exactly what cry analysis needs |
| I2S speaker amp | DOUT=7, BCLK=15, LRCK=16 | stock drove it at 24 kHz; crybaby plays 16 kHz (`SPK_SAMPLE_RATE`) to match the voicebot's WAV. TX needs `auto_clear_after_cb` or the DMA ring loops the tail forever |
| Buttons | three on the top edge, left→right: Vol−=39, BOOT=0, Vol+=10; separate power button on the left case edge | **BOOT=0 is the middle button**, and it is the one held for download-mode entry. Volume−/Vol+ are the outer two |
| LCD 240×240 SPI (ST7789-class) | SDA=41, SCL=42, RES=45, DC=40, CS=21 | **no touch** — SDA/SCL here are SPI data/clock, board has no I2C bus or touch controller; input is the 3 buttons |
| Backlight | GPIO20 PWM | brightness control |
| Battery | ADC1_CH7 sense, charging-detect on GPIO9 (high=charger), on-die temp sensor | stock calibration: +80 raw offset, 3-sample avg, breakpoints 2030→0 % … 2606→100 % |

## Sensor inventory (complete)

The device senses exactly four things — nothing else is populated:

| Sensor | Interface | What it measures |
|---|---|---|
| MEMS microphone (INMP441-class) | I2S | the only environmental sensor — 16 kHz mono audio |
| Battery voltage | ADC1_CH7 (divider) | pack voltage → % via stock breakpoint table |
| Charger presence | GPIO9 digital | high while USB charger attached |
| Die temperature | ESP32-S3 internal | chip temp (not room temp; stock logged it) |

No touchscreen, no IMU/accelerometer, no ambient-light, no humidity/room-temp, no camera.
Radios: Wi-Fi 2.4 GHz + BLE 5 (BLE unused by both stock and crybaby firmware).

## Flash layout (stock)

nvs 16K @0x9000 · otadata 8K · phy 4K · model 960K @0x10000 (WakeNet9 wake-word) ·
ota_0 6M @0x100000 · ota_1 6M @0x700000

## Flashing workflow

- No auto-download circuit: enter bootloader by **holding the middle (BOOT) button while
  tapping power** (or BOOT+RST). RTS hard-reset from esptool works once connected.
- 921600 baud verified for writes via CH340K (15 s for a 1.4 MB app vs 42 s at 230400).
  Bulk *reads* stay unreliable at any baud — see FLASHING.md.
- ⚠️ USB connection drops repeatedly — suspect charge-only/worn cable. Use a known
  data cable before flashing anything.
- **Before first reflash: dump full 16 MB flash to `backup/stock-full-16MB.bin`**
  (restore path to stock firmware). Not yet done — pending stable cable.

## Capability verdict for the project

Everything needed is present: 16 kHz I2S mic (cry capture), Wi-Fi (cloud classify),
240×240 LVGL screen (result UI), 3 buttons (analyze, push-to-talk, volume chord),
battery+gauge (portable), speaker (boot chime + spoken voicebot replies — no longer
unused), 8 MB PSRAM (record buffers, the ~600 KB reply buffer, future on-device model).
