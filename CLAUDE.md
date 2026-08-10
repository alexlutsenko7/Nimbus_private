# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository scope

This `debug` directory holds two independent, loosely-coupled pieces of a larger weather-station project (the rest of the project — STM32 firmware, web dashboard, schematics — lives in sibling directories under `ProjectBookXWeather/`, outside this folder):

- `android/` — **Nimbus**, the Android companion app (Kotlin/Jetpack Compose, package `ca.alxlabs.nimbus`)
- `arduino/wifi_bt/wifi_bt.ino` — ESP32 (Seeed XIAO) firmware that bridges an STM32 sensor board to WiFi/BLE
- `www/` — currently empty

There is no top-level build system tying these together; each is built independently.

## Commands

### Android app (`android/`)
```
./gradlew assembleDebug      # build debug APK
./gradlew installDebug       # build and install on connected device/emulator
./gradlew test               # run local unit tests (app/src/test)
./gradlew connectedAndroidTest # run instrumented tests (app/src/androidTest, needs device/emulator)
./gradlew test --tests "ca.alxlabs.nimbus.ExampleUnitTest"  # run a single unit test class
```
Run these from `android/` (or pass `-p android`). The Gradle wrapper (`gradlew`/`gradlew.bat`) is checked in.

### Arduino firmware (`arduino/wifi_bt/wifi_bt.ino`)
Built/flashed via the Arduino IDE or `arduino-cli` targeting a Seeed XIAO ESP32 board (uses `BLEDevice`, `WiFi`, `Preferences`, `HTTPClient` from the ESP32 Arduino core). No CLI build is set up in-repo; there's no `arduino-cli.yaml`/sketch config here.

## Architecture

### End-to-end data/control flow
1. An STM32 sensor board sends line-oriented text over hardware UART (`Serial1`, pins `D7`/`D6`, 9600 baud) to the ESP32.
2. The ESP32 (`wifi_bt.ino`) inspects each line:
   - A line containing `"Temperature"` is a measurement (`parse_measurement`): parsed with `sscanf` into temperature/pressure/humidity/battery, then uploaded via HTTPS GET to `DATA_URL` (`alxlabs.ca/books/env_sensor/web/update_alx.php`), using WiFi credentials previously stored in flash (`Preferences`, namespace `"nimbus"`).
   - A line containing `"Setup"` puts the ESP32 into **BLE provisioning mode** (`ble_setup`), advertising as `"Weather station"`.
3. In provisioning mode, the Nimbus Android app (`MainActivity.kt`) scans for BLE devices named `"Weather station"`, connects, and drives a GATT-based pairing flow:
   - Write `"SCAN"` to the **command** characteristic → ESP32 scans 2.4 GHz WiFi networks and streams each result (`"SSID,RSSI,encrypted"`) as a separate **wifilist** notification (to stay under BLE MTU limits).
   - Write `"SSID|password"` to the **credential** characteristic → ESP32 attempts to connect, and on success persists credentials to flash for future measurement uploads.
   - The **status** characteristic streams state transitions (`SCANNING`, `SCAN_DONE:<n>`, `CONNECTING`, `CONNECT_OK`, `CONNECT_FAIL`, `ERROR:...`) back to the phone.
4. Outside provisioning mode, the app's main screen is just a `WebView` pointed at the hosted dashboard (`https://alxlabs.ca/books/env_sensor/web/`), which reads the data the ESP32 uploaded in step 2.

The **GATT UUIDs and characteristic roles must stay in sync** between `MainActivity.kt` (`serviceUuid`, `commandUuid`, `wifiListUuid`, `credentialUuid`, `statusUuid`) and `wifi_bt.ino` (`SERVICE_UUID`, `CHAR_COMMAND_UUID`, `CHAR_WIFILIST_UUID`, `CHAR_CREDENTIAL_UUID`, `CHAR_STATUS_UUID`) — changing one side without the other breaks provisioning.

### Android app structure
Single-activity app (`MainActivity.kt`) with no navigation library — screen switching is done via a `mutableStateOf<String>` (`"dashboard"` / `"scan"` / `"wifi_select"`) driving a `when` in Compose. All BLE logic (scanning, GATT connect, characteristic read/write, notification handling) lives directly in `MainActivity`; there is no separate BLE/repository layer. `ui/theme/` holds only generated Compose Material3 theme boilerplate (`Color.kt`, `Theme.kt`, `Type.kt`) — the app overrides it at runtime with a custom `darkColorScheme` built inline in `onCreate`, so editing `ui/theme/` has limited effect.

Notable BLE quirks handled in code (relevant if BLE pairing misbehaves):
- `refreshGattCache` uses reflection to call the hidden `BluetoothGatt.refresh()` method, working around stale GATT caches on reconnect.
- Connection has a 10s timeout that forces a disconnect/reconnect with `TRANSPORT_LE` if service discovery stalls.
- Descriptor writes (enabling notifications) are staggered with `postDelayed` because BLE only allows one pending GATT operation at a time.

### ESP32 firmware structure
Single `.ino` file, state-machine style via global flags (`g_b_scan_requested`, `g_b_credential_received`, etc.) set in BLE characteristic callbacks and consumed in `loop()`. `READY_PIN` (D5) is toggled to signal the STM32 when the ESP32 is busy handling a measurement or entering setup mode.
