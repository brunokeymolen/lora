# LoRa Ping-Pong – Heltec WiFi LoRa 32 V4

Range / antenna test for two **Heltec WiFi LoRa 32 V4** boards (ESP32-S3 + SX1262, 868 MHz EU).

## Quick start

```bash
# 1. Clone or download this repo, then:
bash setup.sh                  # init git & pull ra01s submodule

# 2. Set target
idf.py set-target esp32s3

# 3. (Optional) tune TX power, timeouts, LoRa parameters
idf.py menuconfig              # Ping-Pong Configuration / SX126X Configuration

# 4. Build & flash BOTH devices with the same binary
idf.py build
idf.py -p /dev/ttyACM0 flash
idf.py -p /dev/ttyACM0 monitor --no-reset
```

Both devices run **identical firmware**. Press the USR button on either one to initiate a ping.

## How it works

**Identical firmware on both devices.** LoRa is half-duplex: when transmitting, the RX path is electrically disconnected — you cannot hear your own packets. No filtering needed for self-reception.

| Action | Behaviour |
|--------|-----------|
| Button press | Sends PING (broadcast), enters `WAIT_PONG` state, displays RTT/RSSI/SNR on reply |
| Receive PING | Random back-off delay, sends PONG carrying the peer's measured RSSI/SNR, returns to IDLE |
| Receive PONG | Only processed while in `WAIT_PONG` state, matched by `(dst_id == my_id, seq)` |

### Collision avoidance (layered)

1. **Device ID** – last 3 bytes of ESP32 MAC, embedded in every packet; PONG carries the requester's ID as `dst_id`
2. **Random PONG delay** – `0..PONG_DELAY_MAX_MS` ms before replying, prevents simultaneous TX if both hear the same packet
3. **State guard** – a device in `WAIT_PONG` state ignores incoming PINGs
4. **Seen-packet cache** – 8-entry ring buffer drops duplicate `(src_id, seq)` within 10 s

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

## Pin assignments (Heltec WiFi LoRa 32 V4)

| Function        | GPIO |
|-----------------|------|
| LoRa NSS (CS)   | 8    |
| LoRa SCK        | 9    |
| LoRa MOSI       | 10   |
| LoRa MISO       | 11   |
| LoRa RST        | 12   |
| LoRa BUSY       | 13   |
| LoRa DIO1       | 14   |
| LoRa TXEN (PA)  | 34   |
| OLED SDA        | 17   |
| OLED SCL        | 18   |
| OLED VEXT       | 36 (LOW=on) |
| OLED RST        | 21   |
| USR button      | 0  (active LOW) |

## LoRa parameters (sdkconfig.defaults)

| Parameter       | Default    |
|-----------------|------------|
| Frequency       | 868 MHz    |
| Spreading Factor| SF7        |
| Bandwidth       | 125 kHz    |
| Coding Rate     | 4/5        |
| TX power        | 14 dBm (configurable up to 22 dBm on SX1262 output) |

Change via `idf.py menuconfig` → *SX126X Configuration* and *Ping-Pong Configuration*.

> ⚠️ **Always attach the antenna before powering the board.** Transmitting without an antenna can damage the SX1262.

## Dependencies

| Library | Source |
|---------|--------|
| ra01s (SX126x driver) | [nopnop2002/esp-idf-sx126x](https://github.com/nopnop2002/esp-idf-sx126x) – pulled by `setup.sh` |
| ssd1306 | `components/ssd1306/` – bundled, no download needed |
