# Copilot Instructions

## Project Overview

LoRa ping-pong range/antenna tester for two **Heltec WiFi LoRa 32 V4** boards (ESP32-S3 + SX1262, EU 868 MHz). One device is the **PING** initiator (button-triggered), the other is the **PONG** responder. Both roles are in the same codebase; role is selected via `idf.py menuconfig`. Results (RSSI, SNR, RTT) are displayed on the onboard 128×64 SSD1306 OLED.

## Development Environment

Developed inside the devcontainer defined in `.devcontainer/esp32/`. Uses `espressif/idf:release-v5.5` with USB passthrough for flashing. The devcontainer also installs `qemu-system-xtensa` for limited emulation.

## First-time Setup

```sh
bash setup.sh          # init git repo + pull ra01s submodule
idf.py set-target esp32s3
idf.py menuconfig      # set PING or PONG role
```

## Build, Flash, and Monitor

```sh
idf.py build
idf.py flash monitor                       # combined flash + serial monitor
idf.py flash monitor -p /dev/ttyUSB0       # explicit port
idf.py fullclean                           # wipe build dir
```

## Project Layout

```
CMakeLists.txt          # top-level; references ra01s via EXTRA_COMPONENT_DIRS
sdkconfig.defaults      # committed pin/frequency/LoRa defaults (edit this)
sdkconfig               # generated; do NOT commit
main/
  main.c                # app_main(), ping_task(), pong_task(), display_task()
  Kconfig.projbuild     # ROLE_PING/ROLE_PONG + tunable parameters
  CMakeLists.txt
components/
  ssd1306/              # bundled SSD1306 I2C driver + 5×7 ASCII font
    ssd1306.h / .c
ext/
  esp-idf-sx126x/       # git submodule: nopnop2002/esp-idf-sx126x (ra01s component)
```

## Hardware Pin Map – Heltec WiFi LoRa 32 V4

| Signal          | GPIO | Notes |
|-----------------|------|-------|
| LoRa NSS (CS)   | 8    | |
| LoRa SCK        | 9    | |
| LoRa MOSI       | 10   | |
| LoRa MISO       | 11   | |
| LoRa RST        | 12   | |
| LoRa BUSY       | 13   | |
| LoRa DIO1       | 14   | interrupt line |
| LoRa TXEN       | 34   | PA enable (active HIGH) |
| OLED SDA        | 17   | |
| OLED SCL        | 18   | |
| OLED VEXT       | 36   | LOW = power on |
| USR button      | 0    | BOOT button, active LOW |

All pin values are set in `sdkconfig.defaults` and overridable via `menuconfig`.

## Key Conventions

### ra01s driver (SX1262)

The driver lives in `ext/esp-idf-sx126x/components/ra01s/`. Key API:

```c
LoRaInit();
LoRaBegin(freq_hz, tx_power_dbm, tcxo_voltage, use_ldo);  // returns 0 on success
SetDio2AsRfSwitchCtrl(1);   // must call after LoRaBegin on Heltec V4
LoRaConfig(sf, bw, cr, preamble, payload_len, crc_on, invert_irq);
LoRaSend(buf, len, SX126x_TXMODE_SYNC);   // blocking
LoRaReceive(buf, max_len);                // non-blocking; returns bytes received or 0
GetPacketStatus(&rssi, &snr);             // call immediately after LoRaReceive > 0
```

- **Bandwidth** in `LoRaConfig` / `CONFIG_BANDWIDTH`: the value is passed **raw to the SX1262 register** (no lookup table). Use SX1262 datasheet values: `3`=62.5 kHz, **`4`=125 kHz**, `5`=250 kHz, `6`=500 kHz. BW=7 is undefined — do not use it.
- **Heltec V4 TCXO**: voltage = `1.8f`, `use_ldo = false` (uses DC-DC regulator).
- `SetDio2AsRfSwitchCtrl(1)` is required — it tells the SX1262 to automatically drive the RF switch (antenna path) during TX/RX.
- `CONFIG_TXEN_GPIO=34` activates the on-board PA; `CONFIG_RXEN_GPIO=-1` (DIO2 handles RX switch). **The ra01s driver was patched** (`ra01s.c SetTxEnable/SetRxEnable`) to support TXEN-only configs — without the patch, TXEN is never driven when RXEN=-1.

### SSD1306 component

```c
ssd1306_init(&dev, sda_gpio, scl_gpio, vext_gpio);  // vext_gpio=-1 to skip
ssd1306_printf(&dev, row, "fmt %d", val);           // full-width row (pads with spaces)
ssd1306_flush(&dev);                                // write framebuffer to display
ssd1306_clear(&dev);                                // clear + flush
```

Uses ESP-IDF v5.x new I2C master API (`driver/i2c_master.h`). 21 characters × 8 rows at 6×8 px per character.

### Half-duplex & self-reception

LoRa is **half-duplex**: the SX1262 is either in TX or RX mode, never both. When transmitting, the RF switch disconnects the RX path — you cannot hear your own packets at all. No software filtering needed.

### Symmetric design (both devices same binary)

Both devices run identical firmware. Every device:
- Listens in `STATE_IDLE`
- Button press → sends PING (broadcast dst), enters `STATE_WAIT_PONG`
- Receives PING → random delay → sends PONG with peer's `src_id` as `dst_id`
- Receives PONG while in `STATE_WAIT_PONG` → validates `dst_id == my_id && seq == tx_seq` → records stats

### Packet structure

```c
typedef struct __attribute__((packed)) {
    uint8_t  type;              // 0x50=PING, 0x60=PONG
    uint8_t  src_id[3];         // sender's last 3 MAC bytes
    uint8_t  dst_id[3];         // recipient ID (PONG) or 0xFF:FF:FF (PING broadcast)
    uint16_t seq;               // big-endian sequence number
    int8_t   rssi_rep;          // RSSI the responder measured on the PING (PONG only)
    int8_t   snr_rep;           // SNR  the responder measured on the PING (PONG only)
    uint8_t  tx_power;
    uint16_t uptime_s;          // big-endian sender uptime in seconds
} lora_pkt_t;
```

Multi-byte integers in packets are **big-endian** (`__builtin_bswap16` for conversion).

### Configuration

- Edit `sdkconfig.defaults` for permanent pin/frequency changes.
- `idf.py menuconfig` → *Ping-Pong Configuration* for TX power, timeouts, PONG delay.
- `idf.py menuconfig` → *SX126X Configuration* for LoRa modem parameters (SF, BW, CR).
- `sdkconfig` is generated and must **not** be committed.
- No role selection needed — both devices flash the same binary.

## QEMU Emulation

```sh
idf.py build
# Launch with generated firmware (SPI/I2C peripherals not emulated)
```
