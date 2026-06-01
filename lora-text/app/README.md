# LoRa Text App — Flutter

## Setup

Flutter is not pre-installed in the dev container. Run these steps on your local machine:

```bash
# 1. Create the Flutter project scaffold
cd ~/projects/lora/lora-text/app
flutter create lora_text_app --project-name lora_text_app

# 2. Replace the generated lib/ with our source
cp -r src/lib/* lora_text_app/lib/
cp src/pubspec.yaml lora_text_app/pubspec.yaml

# 3. Install dependencies
cd lora_text_app
flutter pub get

# 4. Add Android BLE permissions (see below), then run
flutter run
```

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
