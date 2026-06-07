# LoRa Text App — Flutter

## Setup (first time only)

Open `lora-text/app/` in VS Code and reopen in the Flutter devcontainer when prompted.

The `src/` folder contains the source files, but you need a full Flutter project scaffold first:

```bash
cd ~/projects/lora/lora-text/app

# 1. Create project scaffold
flutter create lora_text_app --project-name lora_text_app

# 2. Copy our source into it
cp -r src/lib/* lora_text_app/lib/
cp src/pubspec.yaml lora_text_app/pubspec.yaml

# 3. Add Android BLE permissions (see below)

# 4. Install dependencies
cd lora_text_app && flutter pub get
```

## Build & Deploy to Android phone

1. Enable **Developer Options** and **USB Debugging** on your Android phone
2. Plug it in via USB
3. In the devcontainer terminal:

```bash
cd ~/projects/lora/lora-text/app/lora_text_app
flutter devices          # verify phone is listed
flutter run --no-dds     # build, deploy and start
```

> **Devcontainer note:** If you see `Error connecting to the service protocol`, use
> `--no-dds` as above. The app runs fine on the phone — this error only affects the
> VS Code debugger tunnel. Hot reload still works by pressing `r` in the terminal.

For subsequent runs, `flutter run --no-dds` redeploys automatically.

To build a standalone APK:

```bash
flutter build apk --release
# Output: build/app/outputs/flutter-apk/app-release.apk
```

> The devcontainer has `--privileged` and USB passthrough configured,
> so `flutter run` to a physical phone works directly from the VS Code terminal.

## Android permissions

Add to `android/app/src/main/AndroidManifest.xml` inside `<manifest>`:

```xml
<uses-permission android:name="android.permission.BLUETOOTH_SCAN"
    android:usesPermissionFlags="neverForLocation" />
<uses-permission android:name="android.permission.BLUETOOTH_CONNECT" />
<uses-permission android:name="android.permission.ACCESS_FINE_LOCATION" />
```

## iOS permissions

Add to `ios/Runner/Info.plist`:

```xml
<key>NSBluetoothAlwaysUsageDescription</key>
<string>LoRa Text uses BLE to communicate with the LoRa device</string>
<key>NSBluetoothPeripheralUsageDescription</key>
<string>LoRa Text uses BLE to communicate with the LoRa device</string>
```

## Usage

1. Flash firmware to one or both Heltec V4.3 boards
2. Open app, tap **Scan**
3. Connect to `LoRaText-XXXXXX`
4. Type a message and tap **Send** — it goes over LoRa, encrypted
5. Incoming messages appear automatically with RSSI/SNR info

## PSK

The pre-shared key is set at build time via `CONFIG_LORA_PSK` in `sdkconfig.defaults`.
All boards and the app must use the **same** PSK. Change it before flashing production devices.

> The app does **not** need to know the PSK — encryption/decryption happens
> entirely on the ESP32 over the LoRa air link. BLE is cleartext (but short-range).
