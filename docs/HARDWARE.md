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
| I2S speaker amp | DOUT=7, BCLK=15, LRCK=16 | 24 kHz playback |
| Buttons | BOOT=0 (left), Vol+=10, Vol−=39 (top) | left button doubles as download-mode entry |
| LCD 240×240 SPI (ST7789-class) | SDA=41, SCL=42, RES=45, DC=40, CS=21 | LVGL-ready |
| Backlight | GPIO20 PWM | brightness control |
| Battery | ADC1_CH7 sense, charging-detect GPIO, on-die temp sensor | stock fw showed ~85%, charging detect works |

## Flash layout (stock)

nvs 16K @0x9000 · otadata 8K · phy 4K · model 960K @0x10000 (WakeNet9 wake-word) ·
ota_0 6M @0x100000 · ota_1 6M @0x700000

## Flashing workflow

- No auto-download circuit: enter bootloader by **holding left (BOOT) button while
  plugging USB in** (or BOOT+RST). RTS hard-reset from esptool works once connected.
- 921600 baud OK via CH340K.
- ⚠️ USB connection drops repeatedly — suspect charge-only/worn cable. Use a known
  data cable before flashing anything.
- **Before first reflash: dump full 16 MB flash to `backup/stock-full-16MB.bin`**
  (restore path to stock firmware). Not yet done — pending stable cable.

## Capability verdict for the project

Everything needed is present: 16 kHz I2S mic (cry capture), Wi-Fi (cloud classify),
240×240 LVGL screen (result UI), 3 buttons (analyze + nav), battery+gauge (portable),
speaker (optional audio feedback), 8 MB PSRAM (record buffers / future on-device model).
