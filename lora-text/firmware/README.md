# LoRa Text — Firmware

ESP-IDF firmware for the Heltec WiFi LoRa 32 V4.3 (ESP32-S3 + SX1262, 868 MHz).

## Prerequisites

Open this `firmware/` folder in VS Code. When prompted, **Reopen in Container** to use the ESP-IDF devcontainer.

## Build & Flash

### Option 1 — VS Code status bar (easiest)

Use the buttons in the bottom status bar:

| Button | Action |
|--------|--------|
| ⚙️ Build | Compile only |
| 🔌 Select port | Pick `/dev/ttyACM0` or `/dev/ttyUSB0` |
| ⚡ Flash | Flash to device |
| 🖥️ Monitor | Open serial monitor |
| 🔥 Build Flash Monitor | Do all three in one click |

The **flame icon** (Build Flash Monitor) is the most convenient for daily use.

### Option 2 — Terminal

```bash
# First time, or after changing sdkconfig.defaults:
rm -f sdkconfig

idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Press `Ctrl+]` to exit the monitor.

### Finding the port

```bash
ls /dev/ttyUSB* /dev/ttyACM*
```

Heltec V4 typically shows as `/dev/ttyACM0`. With two boards plugged in you'll see `ACM0` and `ACM1`.

## Configuration

Edit `sdkconfig.defaults` to change non-secret settings before building:

- `CONFIG_LORA_FREQUENCY_HZ` — LoRa frequency (default: 868 MHz EU)
- Hardware GPIO pins — preconfigured for Heltec V4.3, no changes needed

> **Important:** If you change `sdkconfig.defaults`, delete `sdkconfig` before rebuilding:
> ```bash
> rm -f sdkconfig && idf.py build
> ```

## Pre-shared Key (PSK) & Encryption

All devices that want to communicate **must use the same PSK**. Messages from a device
with a different PSK are silently dropped (AES-256-GCM auth tag mismatch).

### How it works

The PSK is a human-readable passphrase. At boot the firmware derives a 32-byte AES-256
key by running it through SHA-256:

```
AES key = SHA-256(CONFIG_LORA_PSK)
```

Each LoRa packet is encrypted with **AES-256-GCM**, which also authenticates the
message. A wrong key or corrupted packet is detected and discarded automatically.

### Setting your own PSK

Set the PSK only in your local `sdkconfig` via `menuconfig`. Do not put it in
`sdkconfig.defaults` or commit it to git.

```bash
idf.py menuconfig
# navigate to: LoRa Text Messenger Configuration → LoRa Pre-Shared Key (passphrase)
```

The project will fail to compile if `CONFIG_LORA_PSK` is left empty.

Then rebuild:

```bash
rm -f sdkconfig && idf.py build
```

### Generating a strong PSK

```bash
# Generate a random 32-character passphrase:
openssl rand -base64 24
```

Paste the output into `CONFIG_LORA_PSK` in `menuconfig`, which writes it to your
local `sdkconfig`, and flash **all** your devices with the same value.

> **Security note:** The PSK is compiled into the firmware binary. Encryption protects
> the LoRa air link. The BLE link (phone ↔ ESP32) is cleartext but short-range (~10 m).

## Flash troubleshooting

- **Permission denied on port:** The devcontainer runs `--privileged` with USB passthrough — unplug and replug the USB cable.
- **Wrong port:** Use `ls /dev/ttyACM* /dev/ttyUSB*` to find the correct device.
- **Boot loop / crash:** Open monitor (`idf.py monitor`) to see the panic output.
