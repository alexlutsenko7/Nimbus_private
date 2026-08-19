/* simple_dev.ino - example sketch matching simple_dev/index.php and its
 * Android companion app (www/simple_dev/Android/). Talks to an STM32
 * sensor board over UART exactly like arduino/wifi_bt/wifi_bt.ino does -
 * same RX_PIN/TX_PIN/READY_PIN, same UART_BAUD, same "Temperature"/"Setup"
 * line dispatch and READY_PIN handshake. This board does nothing on its
 * own at boot; it waits for the STM32 to send it a line.
 *
 * Two responsibilities, both triggered by a line from the STM32:
 *  1. A line containing "Temperature" is a measurement - parsed the same
 *     way as wifi_bt.ino's parse_measurement(), then reformatted as a
 *     single "d" query parameter (decimal floats, battery in volts, e.g.
 *     "T23.5P1013.2H45.6B3.7") and HTTPS GETted to simple_dev/index.php.
 *  2. A line containing "Setup" starts BLE WiFi provisioning - mirrors
 *     wifi_bt.ino's mechanism (scan/credential/status characteristics,
 *     same message formats) but with its own UUIDs/device name (so this
 *     never gets confused with a real Nimbus device) and no accounts -
 *     there is no user-credential characteristic and no
 *     claim/CLAIM_OK/CLAIM_FAILED step, since simple_dev/index.php has no
 *     accounts to claim into. */

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <WiFi.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

/* STM32 UART link - same pins/baud/protocol as wifi_bt.ino, so this */
/* sketch can talk to the same STM32 board unmodified */
#define RX_PIN               D7
#define TX_PIN               D6
#define READY_PIN            D5
#define UART_BAUD            9600
#define DELAY_100MS          100u

#define PARSE_FIELD_COUNT    4
#define TEMP_SCALE           100
#define HUM_SCALE            1000

#define DATA_URL "https://alxlabs.ca/books/env_sensor/web/simple_dev/index.php"

#define DATA_UPLOAD_RETRY_COUNT 3
#define DATA_UPLOAD_RETRY_DELAY_MS 1000u
#define DATA_UPLOAD_TIMEOUT_MS 10000u
#define TLS_HANDSHAKE_TIMEOUT_S 20u

/* BLE UUIDs for the GATT service and characteristics - distinct from */
/* wifi_bt.ino's (leading digit 1 -> 2) so a phone/device from one setup */
/* never matches the other's service */
#define SERVICE_UUID          "22345678-1234-1234-1234-123456789ABC"
#define CHAR_COMMAND_UUID     "22345678-1234-1234-1234-123456789ABD"
#define CHAR_WIFILIST_UUID    "22345678-1234-1234-1234-123456789ABE"
#define CHAR_CREDENTIAL_UUID  "22345678-1234-1234-1234-123456789ABF"
#define CHAR_STATUS_UUID      "22345678-1234-1234-1234-123456789AC0"

/* BLE device name visible to scanning phones - distinct from wifi_bt.ino's */
/* "Weather station" so the two setups can't find each other's device */
#define BLE_DEVICE_NAME      "SimpleDev Station"

/* Hostname used for the DNS health check after connecting - see */
/* wifi_check_dns_healthy() */
#define SERVER_HOST          "alxlabs.ca"

/* WiFi scan/connect parameters - same AP-selection logic as wifi_bt.ino */
/* (BSSID pinning to the strongest AP for a given SSID, DNS health check, */
/* full radio reset between attempts) */
#define WIFI_SCAN_MAX_NETWORKS 20
#define WIFI_CONNECT_TIMEOUT_MS 15000u
#define WIFI_CONNECT_RETRY_COUNT 3
#define WIFI_CONNECT_FAIL_GRACE_MS 3000u
#define WIFI_RECONNECT_BACKOFF_BASE_MS 500u

/* Flash storage namespace and key names - distinct from wifi_bt.ino's */
/* "nimbus" namespace */
#define PREF_NAMESPACE       "simpledev"
#define PREF_KEY_SSID        "wifi_ssid"
#define PREF_KEY_PASS        "wifi_pass"
#define PREF_KEY_VALID       "wifi_valid"

/* Pointer to the BLE server instance */
BLEServer *gp_server;
/* Pointer to the command characteristic - phone writes commands here */
BLECharacteristic *gp_char_command;
/* Pointer to the wifi list characteristic - esp sends scan results here */
BLECharacteristic *gp_char_wifilist;
/* Pointer to the credential characteristic - phone writes ssid|password here */
BLECharacteristic *gp_char_credential;
/* Pointer to the status characteristic - esp sends status notifications here */
BLECharacteristic *gp_char_status;

/* True once a phone is connected via BLE */
bool g_b_client_connected = false;
/* Set by the command callback when the phone requests a WiFi scan */
bool g_b_scan_requested = false;
/* Set by the credential callback when the phone sends WiFi credentials */
bool g_b_credential_received = false;
/* SSID/password most recently received from the phone */
String g_str_received_ssid;
String g_str_received_pass;

/* Preferences object for reading/writing flash storage */
Preferences g_preferences;

void send_status(const char *p_message);
void wifi_scan_and_send(void);
bool wifi_connect_with_credentials(const String &str_ssid, const String &str_pass, bool b_persist);
bool wifi_connect_stored(void);
void wifi_connect_and_store(void);
void ble_setup(void);
void parse_measurement(String str_data);
void send_weather_status(float f_temperature_c, float f_pressure_hpa, float f_humidity_pct, float f_battery_v);
bool wifi_check_dns_healthy(void);
void wifi_reset_radio(void);
bool wifi_find_strongest_bssid(const String &str_ssid, uint8_t *p_bssid_out, int32_t *pi32_channel_out, int32_t *pi32_rssi_out);
void wifi_event_handler(WiFiEvent_t event, WiFiEventInfo_t info);

/* BLE server connection callbacks - called when a phone connects/disconnects */
class SimpleDevServerCallbacks : public BLEServerCallbacks
{
    void onConnect(BLEServer *pServer)
    {
        g_b_client_connected = true;
        Serial.printf("BLE client connected\r\n");
    }

    void onDisconnect(BLEServer *pServer)
    {
        g_b_client_connected = false;
        Serial.printf("BLE client disconnected\r\n");

        /* Only now tell the STM32 the setup session is actually over - */
        /* same reasoning as NimbusServerCallbacks::onDisconnect() in */
        /* wifi_bt.ino. Pulling READY_PIN low after a single (possibly */
        /* failed) credential attempt instead of here would end the */
        /* session before the phone gets a chance to retry with a */
        /* different password. */
        digitalWrite(READY_PIN, LOW);
    }
};

/* Command characteristic callback - called when the phone writes a command */
class CommandCallbacks : public BLECharacteristicCallbacks
{
    void onWrite(BLECharacteristic *pCharacteristic)
    {
        String str_value;

        str_value = pCharacteristic->getValue().c_str();
        str_value.trim();

        Serial.printf("Command received: %s\r\n", str_value.c_str());

        if (str_value == "SCAN")
        {
            /* Set the flag so loop() handles the scan - callbacks run on */
            /* the BLE stack's own thread, not loop()'s */
            g_b_scan_requested = true;
        }
    }
};

/* Credential characteristic callback - called when the phone writes ssid|password */
class CredentialCallbacks : public BLECharacteristicCallbacks
{
    void onWrite(BLECharacteristic *pCharacteristic)
    {
        String str_value;
        int32_t i32_separator;

        str_value = pCharacteristic->getValue().c_str();
        str_value.trim();

        Serial.printf("Credential received: %s\r\n", str_value.c_str());

        i32_separator = str_value.indexOf('|');

        if (i32_separator > 0)
        {
            g_str_received_ssid = str_value.substring(0, i32_separator);
            g_str_received_pass = str_value.substring(i32_separator + 1);
            g_b_credential_received = true;

            Serial.printf("SSID: %s\r\n", g_str_received_ssid.c_str());
            Serial.printf("Pass length: %d\r\n", g_str_received_pass.length());
        }
        else
        {
            send_status("ERROR:Invalid credential format");
        }
    }
};

void setup()
{
    /* Builtin LED, just turn on to indicate that system is working - same */
    /* as wifi_bt.ino. Active-low on this board (Seeed XIAO), so LOW is on. */
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);

    Serial.begin(115200);

    /* Init for GPIO to signal readiness to the STM32 - same as wifi_bt.ino */
    pinMode(READY_PIN, OUTPUT);
    digitalWrite(READY_PIN, LOW);

    WiFi.mode(WIFI_STA);
    /* Log the specific reason for every WiFi disconnect - same as wifi_bt.ino */
    WiFi.onEvent(wifi_event_handler);

    /* STM32 link - same pins/baud as wifi_bt.ino, so the same STM32 board */
    /* can drive this sketch unmodified. This board does nothing on its */
    /* own; everything below is triggered by a line arriving here. */
    Serial1.begin(UART_BAUD, SERIAL_8N1, RX_PIN, TX_PIN);
    /* Flush any bootloader garbage sitting in RX buffer */
    delay(DELAY_100MS);
    while (Serial1.available())
    {
        Serial1.read();
    }

    Serial.printf("STM32 UART link initialised, waiting for a line...\r\n");
}

void loop()
{
    String str_data;

    /* Check if data is available from the STM32 */
    if (Serial1.available())
    {
        /* Read the incoming line until newline */
        str_data = Serial1.readStringUntil('\n');
        str_data.trim();

        /* Check if this is a measurement data line */
        if (str_data.indexOf("Temperature") >= 0)
        {
            /* Signal readiness to STM32 */
            digitalWrite(READY_PIN, HIGH);
            parse_measurement(str_data);
            digitalWrite(READY_PIN, LOW);
        }
        /* Check if this is a setup request */
        else if (str_data.indexOf("Setup") >= 0)
        {
            /* Signal readiness to STM32 */
            digitalWrite(READY_PIN, HIGH);
            /* Enter setup mode - start BLE GATT server for provisioning. */
            /* READY_PIN only goes low again once BLE actually disconnects */
            /* (see SimpleDevServerCallbacks::onDisconnect) */
            ble_setup();
        }
    }

    /* Handle a WiFi scan request from the phone (set by command callback) */
    if (g_b_scan_requested)
    {
        g_b_scan_requested = false;
        wifi_scan_and_send();
    }

    /* Handle received WiFi credentials from the phone (set by credential callback) */
    if (g_b_credential_received)
    {
        g_b_credential_received = false;
        wifi_connect_and_store();
        Serial.printf("DONE, network connected and stored\r\n");
    }
}

/* Parses "Temperature: %ld Pressure: %ld Humidity: %ld Battery: %ld" from */
/* the STM32 - same format/scaling as wifi_bt.ino's parse_measurement() */
/* (temperature x100 C, humidity x1000 %RH, pressure Pa, battery mV) - then */
/* converts to the plain decimal floats simple_dev/index.php expects and */
/* uploads if WiFi is available. */
void parse_measurement(String str_data)
{
    int32_t i32_temperature;
    int32_t i32_pressure;
    int32_t i32_humidity;
    int32_t i32_battery;
    int32_t i32_parsed;

    i32_temperature = 0;
    i32_pressure = 0;
    i32_humidity = 0;
    i32_battery = 0;

    i32_parsed = sscanf(str_data.c_str(),
                        "Temperature: %ld Pressure: %ld Humidity: %ld Battery: %ld",
                        &i32_temperature,
                        &i32_pressure,
                        &i32_humidity,
                        &i32_battery);

    if (i32_parsed == PARSE_FIELD_COUNT)
    {
        float f_temperature_c;
        float f_pressure_hpa;
        float f_humidity_pct;
        float f_battery_v;

        f_temperature_c = i32_temperature / (float)TEMP_SCALE;
        f_pressure_hpa = i32_pressure / 100.0f;
        f_humidity_pct = i32_humidity / (float)HUM_SCALE;
        f_battery_v = i32_battery / 1000.0f;

        Serial.printf("Temperature: %.2f C, Pressure: %.1f hPa, Humidity: %.3f %%, Battery: %.2f V\r\n",
                      f_temperature_c, f_pressure_hpa, f_humidity_pct, f_battery_v);

        if (wifi_connect_stored())
        {
            send_weather_status(f_temperature_c, f_pressure_hpa, f_humidity_pct, f_battery_v);
            WiFi.disconnect();
            Serial.printf("WiFi disconnected after upload\r\n");
        }
        else
        {
            Serial.printf("Skipping data upload - no WiFi connection\r\n");
        }
    }
    else
    {
        Serial.printf("Parse error: only %ld of %d fields recognised\r\n", i32_parsed, PARSE_FIELD_COUNT);
    }
}

/* Initialise the BLE GATT server with the provisioning service/characteristics */
void ble_setup(void)
{
    BLEService *p_service;
    BLEAdvertising *p_advertising;
    BLEAdvertisementData adv_data;

    g_b_client_connected = false;
    g_b_scan_requested = false;
    g_b_credential_received = false;

    BLEDevice::init(BLE_DEVICE_NAME);

    gp_server = BLEDevice::createServer();
    gp_server->setCallbacks(new SimpleDevServerCallbacks());

    p_service = gp_server->createService(SERVICE_UUID);

    gp_char_command = p_service->createCharacteristic(
        CHAR_COMMAND_UUID,
        BLECharacteristic::PROPERTY_WRITE
    );
    gp_char_command->setCallbacks(new CommandCallbacks());

    gp_char_wifilist = p_service->createCharacteristic(
        CHAR_WIFILIST_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );
    gp_char_wifilist->addDescriptor(new BLE2902());

    gp_char_credential = p_service->createCharacteristic(
        CHAR_CREDENTIAL_UUID,
        BLECharacteristic::PROPERTY_WRITE
    );
    gp_char_credential->setCallbacks(new CredentialCallbacks());

    gp_char_status = p_service->createCharacteristic(
        CHAR_STATUS_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );
    gp_char_status->addDescriptor(new BLE2902());

    p_service->start();

    p_advertising = BLEDevice::getAdvertising();
    adv_data.setName(BLE_DEVICE_NAME);
    adv_data.setCompleteServices(BLEUUID(SERVICE_UUID));
    p_advertising->setAdvertisementData(adv_data);
    p_advertising->setScanResponse(true);
    BLEDevice::startAdvertising();

    Serial.printf("BLE GATT server started as '%s'\r\n", BLE_DEVICE_NAME);
}

/* Scan for 2.4GHz WiFi networks and send the results to the phone via BLE. */
/* Each network is sent as a separate notification to avoid MTU limits - */
/* same "SSID,RSSI,encrypted" format as wifi_bt.ino's wifi_scan_and_send() */
void wifi_scan_and_send(void)
{
    int32_t i32_network_count;
    int32_t i32_i;
    int32_t i32_sent;
    String str_entry;
    String str_done;

    send_status("SCANNING");
    Serial.printf("Starting WiFi scan...\r\n");

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(DELAY_100MS);

    i32_network_count = WiFi.scanNetworks();
    Serial.printf("Scan complete, found %ld networks\r\n", i32_network_count);

    i32_sent = 0;

    for (i32_i = 0; i32_i < i32_network_count && i32_i < WIFI_SCAN_MAX_NETWORKS; i32_i++)
    {
        /* Only include 2.4GHz networks (channels 1 through 14) */
        if (WiFi.channel(i32_i) <= 14)
        {
            str_entry = WiFi.SSID(i32_i);
            str_entry += ",";
            str_entry += String(WiFi.RSSI(i32_i));
            str_entry += ",";
            str_entry += (WiFi.encryptionType(i32_i) != WIFI_AUTH_OPEN) ? "1" : "0";

            gp_char_wifilist->setValue(str_entry.c_str());
            gp_char_wifilist->notify();

            Serial.printf("Sent network: %s\r\n", str_entry.c_str());
            i32_sent++;
            delay(DELAY_100MS);
        }
    }

    WiFi.scanDelete();

    str_done = "SCAN_DONE:" + String(i32_sent);
    send_status(str_done.c_str());
}

/* Send a status notification string to the phone via the status characteristic */
void send_status(const char *p_message)
{
    if (g_b_client_connected && gp_char_status != NULL)
    {
        gp_char_status->setValue(p_message);
        gp_char_status->notify();
        Serial.printf("Status sent: %s\r\n", p_message);
    }
}

/* Translate an esp_wifi disconnect reason code into a human-readable */
/* string - same subset as wifi_bt.ino's wifi_disconnect_reason_str() */
const char *wifi_disconnect_reason_str(uint8_t ui8_reason)
{
    switch (ui8_reason)
    {
        case 2:   return "AUTH_EXPIRE";
        case 3:   return "AUTH_LEAVE";
        case 4:   return "ASSOC_EXPIRE";
        case 5:   return "ASSOC_TOOMANY - AP is full";
        case 6:   return "NOT_AUTHED";
        case 7:   return "NOT_ASSOCED";
        case 8:   return "ASSOC_LEAVE";
        case 15:  return "4WAY_HANDSHAKE_TIMEOUT - wrong password or weak signal";
        case 200: return "BEACON_TIMEOUT - lost the AP's signal";
        case 201: return "NO_AP_FOUND - SSID not visible/out of range";
        case 202: return "AUTH_FAIL - wrong password";
        case 203: return "ASSOC_FAIL";
        case 204: return "HANDSHAKE_TIMEOUT";
        default:  return "unknown";
    }
}

/* Logs the specific reason for every WiFi disconnect - same as */
/* wifi_bt.ino's wifi_event_handler() */
void wifi_event_handler(WiFiEvent_t event, WiFiEventInfo_t info)
{
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
    {
        Serial.printf("WiFi disconnected, reason: %u (%s)\r\n",
                      info.wifi_sta_disconnected.reason,
                      wifi_disconnect_reason_str(info.wifi_sta_disconnected.reason));
    }
}

/* Resolve SERVER_HOST and report whether the result looks usable - same as */
/* wifi_bt.ino's wifi_check_dns_healthy(). WL_CONNECTED only proves 802.11 */
/* association succeeded, not that the path past the AP is healthy. */
bool wifi_check_dns_healthy(void)
{
    IPAddress ip_resolved;
    bool b_lookup_ok;

    b_lookup_ok = WiFi.hostByName(SERVER_HOST, ip_resolved);

    if (b_lookup_ok && ip_resolved != IPAddress(0, 0, 0, 0))
    {
        Serial.printf("%s resolves to %s\r\n", SERVER_HOST, ip_resolved.toString().c_str());
        return true;
    }

    Serial.printf("%s DNS check failed (resolved to %s)\r\n", SERVER_HOST, ip_resolved.toString().c_str());
    return false;
}

/* Fully reset the WiFi radio's internal state - same as wifi_bt.ino's */
/* wifi_reset_radio(). A plain WiFi.disconnect() is not enough to recover */
/* from a failed connection attempt. */
void wifi_reset_radio(void)
{
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(DELAY_100MS);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
}

/* Scan for every AP currently advertising str_ssid and report the */
/* strongest one's BSSID/channel via the output parameters - same as */
/* wifi_bt.ino's wifi_find_strongest_bssid(). A plain WiFi.begin(ssid, */
/* pass) joins whichever AP answers first, not necessarily the best one on */
/* a multi-AP/mesh network. Returns false (output parameters untouched) if */
/* the scan doesn't see str_ssid at all - callers should fall back to a */
/* plain, unpinned WiFi.begin() in that case. */
bool wifi_find_strongest_bssid(const String &str_ssid, uint8_t *p_bssid_out, int32_t *pi32_channel_out, int32_t *pi32_rssi_out)
{
    int32_t i32_network_count;
    int32_t i32_i;
    bool b_found;
    int32_t i32_best_rssi;

    b_found = false;
    i32_best_rssi = -1000;

    Serial.printf("Scanning for strongest '%s' AP...\r\n", str_ssid.c_str());

    i32_network_count = WiFi.scanNetworks();

    for (i32_i = 0; i32_i < i32_network_count; i32_i++)
    {
        if (WiFi.SSID(i32_i) == str_ssid)
        {
            Serial.printf("  Candidate BSSID: %s, channel %ld, RSSI %ld\r\n",
                          WiFi.BSSIDstr(i32_i).c_str(), (long)WiFi.channel(i32_i), (long)WiFi.RSSI(i32_i));

            if (WiFi.RSSI(i32_i) > i32_best_rssi)
            {
                i32_best_rssi = WiFi.RSSI(i32_i);
                memcpy(p_bssid_out, WiFi.BSSID(i32_i), 6);
                *pi32_channel_out = WiFi.channel(i32_i);
                *pi32_rssi_out = i32_best_rssi;
                b_found = true;
            }
        }
    }

    WiFi.scanDelete();

    if (b_found)
    {
        Serial.printf("Strongest '%s' AP: RSSI %ld\r\n", str_ssid.c_str(), (long)i32_best_rssi);
    }
    else
    {
        Serial.printf("'%s' not seen in scan - falling back to unpinned connect\r\n", str_ssid.c_str());
    }

    return b_found;
}

/* Attempt to connect to WiFi with the given credentials, retrying up to */
/* WIFI_CONNECT_RETRY_COUNT times. Same AP-selection logic as wifi_bt.ino: */
/* pins to the strongest BSSID/channel for the SSID (wifi_find_strongest_ */
/* bssid()), only accepts a connect once the DNS health check passes */
/* (wifi_check_dns_healthy()), and fully resets the radio between attempts */
/* (wifi_reset_radio()) rather than a plain disconnect. On success, */
/* optionally persists the credentials to flash for the next boot. */
bool wifi_connect_with_credentials(const String &str_ssid, const String &str_pass, bool b_persist)
{
    int32_t i32_attempt;
    uint32_t ui32_start;
    uint8_t ui8_best_bssid[6];
    int32_t i32_best_channel;
    int32_t i32_best_rssi;
    bool b_have_bssid;

    i32_best_channel = 0;
    i32_best_rssi = 0;

    /* Start from a known-clean radio state rather than just WiFi.mode(WIFI_STA) */
    wifi_reset_radio();

    /* Find the strongest AP currently advertising this SSID - done once */
    /* per call, not per retry - rescanning every attempt would multiply */
    /* an already several-second cost */
    b_have_bssid = wifi_find_strongest_bssid(str_ssid, ui8_best_bssid, &i32_best_channel, &i32_best_rssi);

    for (i32_attempt = 1; i32_attempt <= WIFI_CONNECT_RETRY_COUNT; i32_attempt++)
    {
        Serial.printf("Connecting to '%s' (attempt %ld/%d)...\r\n",
                      str_ssid.c_str(), i32_attempt, WIFI_CONNECT_RETRY_COUNT);

        /* Pin to the strongest known BSSID/channel if the scan found one, */
        /* otherwise fall back to letting the driver join whichever AP */
        /* answers first */
        if (b_have_bssid)
        {
            WiFi.begin(str_ssid.c_str(), str_pass.c_str(), i32_best_channel, ui8_best_bssid);
        }
        else
        {
            WiFi.begin(str_ssid.c_str(), str_pass.c_str());
        }

        ui32_start = millis();

        /* Wait for connection, a definitive rejection, or timeout - */
        /* WL_CONNECT_FAILED is only trusted after WIFI_CONNECT_FAIL_GRACE_MS, */
        /* since right after WiFi.begin() it can briefly still reflect the */
        /* *previous* attempt */
        while (WiFi.status() != WL_CONNECTED)
        {
            uint32_t ui32_elapsed = millis() - ui32_start;

            if (ui32_elapsed >= WIFI_CONNECT_TIMEOUT_MS)
            {
                break;
            }
            if (ui32_elapsed >= WIFI_CONNECT_FAIL_GRACE_MS && WiFi.status() == WL_CONNECT_FAILED)
            {
                break;
            }
            delay(DELAY_100MS);
        }

        /* Stop retrying as soon as we are connected AND the link can */
        /* actually reach a DNS server - see wifi_check_dns_healthy() */
        if (WiFi.status() == WL_CONNECTED)
        {
            if (wifi_check_dns_healthy())
            {
                break;
            }
            Serial.printf("WiFi connected but DNS check failed - treating as a failed attempt\r\n");
        }
        /* WL_CONNECT_FAILED means the AP actively rejected the handshake */
        /* (wrong password) rather than just being unreachable - retrying */
        /* with the same credentials would only fail the same way again */
        else if (WiFi.status() == WL_CONNECT_FAILED)
        {
            Serial.printf("WiFi connection rejected (wrong password?) - not retrying\r\n");
            wifi_reset_radio();
            break;
        }
        else
        {
            Serial.printf("Connection attempt %ld failed\r\n", i32_attempt);
        }

        /* Reset the radio before the next attempt - a plain disconnect() */
        /* is not enough (see wifi_reset_radio()) */
        wifi_reset_radio();

        if (i32_attempt < WIFI_CONNECT_RETRY_COUNT)
        {
            delay(WIFI_RECONNECT_BACKOFF_BASE_MS * i32_attempt + random(0, 300));
        }
    }

    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.printf("WiFi connection failed after %d attempts\r\n", WIFI_CONNECT_RETRY_COUNT);
        wifi_reset_radio();
        return false;
    }

    Serial.printf("WiFi connected, IP: %s, BSSID: %s, RSSI: %ld\r\n",
                  WiFi.localIP().toString().c_str(), WiFi.BSSIDstr().c_str(), (long)WiFi.RSSI());

    if (b_persist)
    {
        g_preferences.begin(PREF_NAMESPACE, false);
        g_preferences.putString(PREF_KEY_SSID, str_ssid);
        g_preferences.putString(PREF_KEY_PASS, str_pass);
        g_preferences.putBool(PREF_KEY_VALID, true);
        g_preferences.end();
        Serial.printf("Credentials stored to flash\r\n");
    }

    return true;
}

/* Attempt to connect to WiFi using the credentials just received over BLE, */
/* and store them on success - same role as wifi_connect_and_store() in */
/* wifi_bt.ino, minus the claim_device() call (no accounts here). */
void wifi_connect_and_store(void)
{
    String str_ssid;
    String str_pass;

    /* Snapshot before connecting - a new credential write mid-connect */
    /* would otherwise race the globals, same reasoning as */
    /* wifi_connect_and_store() in wifi_bt.ino */
    str_ssid = g_str_received_ssid;
    str_pass = g_str_received_pass;

    send_status("CONNECTING");

    if (wifi_connect_with_credentials(str_ssid, str_pass, true))
    {
        send_status("CONNECT_OK");
        /* Disconnect WiFi - the next measurement cycle will reconnect, */
        /* same as wifi_connect_and_store() in wifi_bt.ino */
        WiFi.disconnect();
    }
    else
    {
        send_status("CONNECT_FAIL");
    }
}

/* Connect using credentials stored in flash from a previous BLE session. */
/* Returns false immediately if none are stored. */
bool wifi_connect_stored(void)
{
    bool b_valid;
    String str_ssid;
    String str_pass;

    g_preferences.begin(PREF_NAMESPACE, true);
    b_valid = g_preferences.getBool(PREF_KEY_VALID, false);
    str_ssid = g_preferences.getString(PREF_KEY_SSID, "");
    str_pass = g_preferences.getString(PREF_KEY_PASS, "");
    g_preferences.end();

    if (!b_valid || str_ssid.isEmpty())
    {
        Serial.printf("No stored WiFi credentials\r\n");
        return false;
    }

    Serial.printf("Found stored WiFi credentials for '%s'\r\n", str_ssid.c_str());
    /* Already stored under these exact values - no need to persist again */
    return wifi_connect_with_credentials(str_ssid, str_pass, false);
}

/* Builds "T<temp>P<pressure>H<humidity>B<battery>" (one decimal place, two
 * for battery) and sends it as ?d=... via HTTPS GET, using the same
 * retry/backoff/WAF-avoidance pattern as send_to_server() in
 * arduino/wifi_bt/wifi_bt.ino. */
void send_weather_status(float f_temperature_c, float f_pressure_hpa, float f_humidity_pct, float f_battery_v)
{
    String str_url;
    int32_t i32_response_code;
    int32_t i32_attempt;

    str_url = DATA_URL;
    str_url += "?d=T";
    str_url += String(f_temperature_c, 1);
    str_url += "P";
    str_url += String(f_pressure_hpa, 1);
    str_url += "H";
    str_url += String(f_humidity_pct, 1);
    str_url += "B";
    str_url += String(f_battery_v, 2);

    for (i32_attempt = 1; i32_attempt <= DATA_UPLOAD_RETRY_COUNT; i32_attempt++)
    {
        WiFiClientSecure wifi_client;
        HTTPClient http_client;

        Serial.printf("Sending data (attempt %ld/%d): %s\r\n",
                      i32_attempt, DATA_UPLOAD_RETRY_COUNT, str_url.c_str());

        /* Skip certificate verification for now - see the same note in wifi_bt.ino */
        wifi_client.setInsecure();
        wifi_client.setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_S);

        if (http_client.begin(wifi_client, str_url))
        {
            http_client.setConnectTimeout(DATA_UPLOAD_TIMEOUT_MS);
            http_client.setTimeout(DATA_UPLOAD_TIMEOUT_MS);

            /* Some hosts silently block/challenge requests with no User-Agent
             * (bot protection), returning 200 with a block page instead of
             * running the script - a bare 200 doesn't prove the script ran. */
            http_client.addHeader("User-Agent", "Mozilla/5.0 (NimbusWeatherStation)");
            http_client.addHeader("Accept", "*/*");

            i32_response_code = http_client.GET();

            if (i32_response_code > 0)
            {
                Serial.printf("Server response: %ld\r\n", i32_response_code);
                Serial.printf("Response body: %s\r\n", http_client.getString().c_str());
                http_client.end();
                return;
            }

            Serial.printf("HTTPS GET failed, error: %s (free heap: %u)\r\n",
                          http_client.errorToString(i32_response_code).c_str(), ESP.getFreeHeap());
            http_client.end();
        }
        else
        {
            Serial.printf("Unable to connect to server (free heap: %u)\r\n", ESP.getFreeHeap());
        }

        if (i32_attempt < DATA_UPLOAD_RETRY_COUNT)
        {
            delay(DATA_UPLOAD_RETRY_DELAY_MS * i32_attempt + random(0, 500));
        }
    }

    Serial.printf("Data upload failed after %d attempts\r\n", DATA_UPLOAD_RETRY_COUNT);
}
