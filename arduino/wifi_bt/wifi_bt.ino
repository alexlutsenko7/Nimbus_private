#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <WiFi.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_mac.h>

/* Route all debug/diagnostic output (every Serial.printf/.begin/.available */
/* call in this file) through the hardware UART on RX_PIN/TX_PIN (D7/D6) */
/* instead of the native USB CDC console, so logs stay visible via an */
/* external USB-UART adapter wired to TX_PIN while the board itself runs */
/* untethered from any PC USB port - needed for battery-only testing, where */
/* no USB cable is connected at all. Only rewrites the token "Serial", not */
/* "Serial1", so the STM32-link code below (Serial1.available()/.read() in */
/* loop()) is unaffected - same pattern already used in the sibling */
/* WifiClient_seeduino_esp32s3.ino sketch (#define Serial Serial0 there) */
#define Serial Serial1

/* Pin definitions */
#define RX_PIN              D7
#define TX_PIN              D6
#define READY_PIN           D5
#define UART_BAUD           9600
#define DELAY_100MS         100u
#define DELAY_300MS         300u
#define PARSE_FIELD_COUNT   4
#define PARSE_SUCCESS       4
#define TEMP_SCALE          100
#define HUM_SCALE           1000

/* Device identifier sent to the server: 6 bytes of the WiFi station MAC   */
/* followed by the last 3 bytes of the Bluetooth MAC (the first 3 bytes   */
/* of the BT MAC are the same OUI as the WiFi MAC on this chip, so only   */
/* the differing tail is worth including) - 9 bytes total, hex-encoded.   */
#define DEVICE_MAC_WIFI_LEN 6
#define DEVICE_MAC_BT_LEN   3
#define DEVICE_MAC_LEN      (DEVICE_MAC_WIFI_LEN + DEVICE_MAC_BT_LEN)
#define DEVICE_MAC_HEX_LEN  (DEVICE_MAC_LEN * 2)

/* Hostname shared by DATA_URL and CLAIM_URL below, used to log which */
/* backend IP DNS handed us on each upload - see wifi_check_dns_healthy() */
#define SERVER_HOST         "alxlabs.ca"
/* Server URL for data upload */
#define DATA_URL            "https://alxlabs.ca/books/env_sensor/web/update_alx.php"
/* WiFi connection timeout for data upload */
#define WIFI_DATA_TIMEOUT   15000u
/* Server URL used once, right after BLE setup connects to WiFi, to link */
/* this device to whichever dashboard account the phone was logged into. */
/* For a brand new device (no row in the devices table yet), temporarily */
/* swap to claim_device_alx.php instead - it just registers the device */
/* with user_id = 0 so a later BLE setup run against the real */
/* claim_device.php below has a row to claim. */
//#define CLAIM_URL            "https://alxlabs.ca/books/env_sensor/web/claim_device_alx.php"
#define CLAIM_URL            "https://alxlabs.ca/books/env_sensor/web/claim_device.php"

/* BLE UUIDs for the GATT service and characteristics */
#define SERVICE_UUID          "12345678-1234-1234-1234-123456789ABC"
#define CHAR_COMMAND_UUID     "12345678-1234-1234-1234-123456789ABD"
#define CHAR_WIFILIST_UUID    "12345678-1234-1234-1234-123456789ABE"
#define CHAR_CREDENTIAL_UUID  "12345678-1234-1234-1234-123456789ABF"
#define CHAR_STATUS_UUID      "12345678-1234-1234-1234-123456789AC0"
/* User credential characteristic UUID - the app writes "username|password_hash" */
/* here during BLE setup, if the phone is logged into the dashboard */
#define CHAR_USER_UUID        "12345678-1234-1234-1234-123456789AC1"

/* BLE device name visible to scanning phones */
#define BLE_DEVICE_NAME     "Weather station"

/* WiFi scan and connection parameters */
#define WIFI_SCAN_TIMEOUT   10000u
#define WIFI_CONNECT_TIMEOUT 15000u
#define WIFI_CONNECT_RETRY_COUNT 5
#define WIFI_CONNECT_FAIL_GRACE_MS 3000u
#define DATA_UPLOAD_RETRY_COUNT 3
#define DATA_UPLOAD_RETRY_DELAY_MS 1000u
/* TCP connect + HTTP response read budget per attempt - NOT the TLS */
/* handshake, which has its own separate, more generous budget below. */
/* Splitting these out came from seeing "connection refused" 3/3 in the */
/* field on a weak-but-usable link (RSSI ~-70dBm) with WiFi + DNS both */
/* working - HTTPClient::errorToString(-1) prints "connection refused" */
/* for ANY client->connect() failure, TLS handshake timeouts included, */
/* so that message does not mean the server actually refused the TCP */
/* connection. */
#define DATA_UPLOAD_TIMEOUT_MS 10000u
/* TLS handshake budget, seconds. A weak/marginal RSSI link (-70 to */
/* -73dBm was enough to reproduce this) can need well over the previous */
/* 8s clamp just for retransmission-heavy handshake round trips, even */
/* though the link is otherwise perfectly usable once established - the */
/* two other ESP32 sketches this board was compared against never */
/* override the handshake timeout at all and don't show this failure */
#define TLS_HANDSHAKE_TIMEOUT_S 20u
/* Backoff between WiFi reconnect attempts (wifi_connect_stored() / */
/* wifi_connect_and_store()). Retrying with only wifi_reset_radio()'s */
/* 100ms gap looked like a deauth flood to at least one AP in the field - */
/* it started rejecting the (correct, working-elsewhere) password with */
/* AUTH_FAIL after a burst of rapid reconnects. Scaled by attempt number */
/* so later retries back off further. */
#define WIFI_RECONNECT_BACKOFF_BASE_MS 500u
#define WIFI_MAX_NETWORKS   20
#define CHUNK_DELIMITER     "\n"

/* Flash storage namespace and key names */
#define PREF_NAMESPACE      "nimbus"
#define PREF_KEY_SSID       "wifi_ssid"
#define PREF_KEY_PASS       "wifi_pass"
#define PREF_KEY_VALID      "wifi_valid"

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
/* Pointer to the user credential characteristic - phone writes */
/* username|password_hash here, if logged into the dashboard */
BLECharacteristic *gp_char_user;
/* Flag indicating whether BLE server is active */
bool g_b_ble_active;
/* Flag indicating whether a phone is connected via BLE */
bool g_b_client_connected;
/* Flag set by command callback when phone requests a wifi scan */
bool g_b_scan_requested;
/* Flag set by credential callback when phone sends wifi credentials */
bool g_b_credential_received = false;
/* Received SSID from the phone */
String g_str_received_ssid;
/* Received password from the phone */
String g_str_received_pass;
/* Flag set by user credential callback when phone sends a dashboard login */
bool g_b_user_credential_received = false;
/* Received dashboard username from the phone */
String g_str_received_username;
/* Received dashboard password hash from the phone */
String g_str_received_password_hash;

/* Preferences object for reading and writing flash storage */
Preferences g_preferences;

/* Forward declaration - function is defined below */
void send_status(const char *p_message);

/* BLE server connection callbacks - called when phone connects or disconnects */
class NimbusServerCallbacks : public BLEServerCallbacks
{
    void onConnect(BLEServer *pServer)
    {
        /* Set the connected flag */
        g_b_client_connected = true;
        Serial.printf("BLE client connected\r\n");
    }

    void onDisconnect(BLEServer *pServer)
    {
        /* Clear the connected flag */
        g_b_client_connected = false;
        Serial.printf("BLE client disconnected\r\n");

        /* Only now tell the STM32 the setup session is actually over. */
        /* READY_PIN going low is what wakes ConfirmComm()'s wait out of */
        /* WiFiSetup() on the STM32 side, which then powers this board */
        /* down - pulling it low after a single (possibly failed) */
        /* credential attempt instead of here cut power mid-retry the */
        /* moment a wrong WiFi password was entered */
        digitalWrite(READY_PIN, LOW);
    }
};

/* Command characteristic callback - called when the phone writes a command */
class CommandCallbacks : public BLECharacteristicCallbacks
{
    void onWrite(BLECharacteristic *pCharacteristic)
    {
        String str_value;

        /* Read the value written by the phone */
        str_value = pCharacteristic->getValue().c_str();
        str_value.trim();

        Serial.printf("Command received: %s\r\n", str_value.c_str());

        /* Check if the command is a wifi scan request */
        if (str_value == "SCAN")
        {
            /* Set the flag so the main loop handles the scan */
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

        /* Read the value written by the phone */
        str_value = pCharacteristic->getValue().c_str();
        str_value.trim();

        Serial.printf("Credential received: %s\r\n", str_value.c_str());

        /* Find the pipe separator between ssid and password */
        i32_separator = str_value.indexOf('|');

        /* Validate that the separator exists and is not at position 0 */
        if (i32_separator > 0)
        {
            /* Extract SSID (everything before the pipe) */
            g_str_received_ssid = str_value.substring(0, i32_separator);
            /* Extract password (everything after the pipe) */
            g_str_received_pass = str_value.substring(i32_separator + 1);
            /* Set the flag so the main loop handles the connection attempt */
            g_b_credential_received = true;

            Serial.printf("SSID: %s\r\n", g_str_received_ssid.c_str());
            Serial.printf("Pass length: %d\r\n", g_str_received_pass.length());
        }
        else
        {
            /* Notify the phone that the format was invalid */
            send_status("ERROR:Invalid credential format");
        }
    }
};

/* User credential characteristic callback - called when the phone writes */
/* username|password_hash (only sent if the phone is logged into the */
/* dashboard). Silently ignored if malformed - the device just stays */
/* unclaimed, same as if nothing was written at all */
class UserCredentialCallbacks : public BLECharacteristicCallbacks
{
    void onWrite(BLECharacteristic *pCharacteristic)
    {
        String str_value;
        int32_t i32_separator;

        /* Read the value written by the phone */
        str_value = pCharacteristic->getValue().c_str();
        str_value.trim();

        /* Find the pipe separator between username and password hash */
        i32_separator = str_value.indexOf('|');

        /* Validate that the separator exists and is not at position 0 */
        if (i32_separator > 0)
        {
            /* Extract username (everything before the pipe) */
            g_str_received_username = str_value.substring(0, i32_separator);
            /* Extract password hash (everything after the pipe) */
            g_str_received_password_hash = str_value.substring(i32_separator + 1);
            g_b_user_credential_received = true;

            Serial.printf("User credential received for: %s\r\n", g_str_received_username.c_str());
        }
    }
};

/* Translate an esp_wifi disconnect reason code (WiFiEventInfo_t.wifi_sta_disconnected.reason) */
/* into a human-readable string, for the handful of reasons that actually */
/* show up in practice - full list is in esp_wifi_types.h (wifi_err_reason_t) */
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

/* WiFi station disconnect events fire with a reason code that is far more */
/* specific than wl_status_t (which only says WL_CONNECT_FAILED) - this is */
/* what actually distinguishes "AP out of range", "wrong password", and */
/* "signal dropped mid-handshake" from each other in the serial log */
void wifi_event_handler(WiFiEvent_t event, WiFiEventInfo_t info)
{
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
    {
        Serial.printf("WiFi disconnected, reason: %u (%s)\r\n",
                      info.wifi_sta_disconnected.reason,
                      wifi_disconnect_reason_str(info.wifi_sta_disconnected.reason));
    }
}

/* Resolve SERVER_HOST and report whether the result looks usable, logging */
/* the outcome either way. Originally just diagnostic (comparing the */
/* resolved backend IP between failing and succeeding uploads - round-robin/ */
/* anycast DNS was ruled out this way early in this investigation), but */
/* WiFi.hostByName() has a known quirk: it can return true (success) while */
/* still writing an all-zero IPAddress if the underlying query effectively */
/* timed out. Seen in the field as "resolves to 0.0.0.0" ~7s after */
/* WiFi connected (itself right after an AUTH_EXPIRE reassociation blip), */
/* immediately followed by every one of the real upload's 3 retries failing */
/* with "read Timeout" - i.e. WL_CONNECTED doesn't guarantee the path past */
/* the AP is actually healthy. Treat both a false return and an all-zero */
/* result as unhealthy so callers can act on it, not just log it */
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

/* Fully reset the WiFi radio's internal state. A plain WiFi.disconnect() */
/* is not enough to recover from a failed connection attempt - the        */
/* underlying esp_wifi driver can get stuck silently refusing every       */
/* WiFi.begin() afterwards, even with different, correct credentials on a */
/* brand new BLE session, unless the radio is cycled fully off and back   */
/* on between attempts. */
void wifi_reset_radio(void)
{
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(DELAY_100MS);
    WiFi.mode(WIFI_STA);
    /* Keep the radio fully awake - see the setSleep(false) note in */
    /* wifi_connect_stored()/wifi_connect_and_store() for why */
    WiFi.setSleep(false);
}

/* Scan for every AP currently advertising str_ssid and report the strongest */
/* one's BSSID/channel via the output parameters. Needed because "MIS" is */
/* broadcast by more than one physical AP in the field (mesh nodes seen at */
/* 30:DE:4B:B1:A5:0E and :10, both usually around -67 to -72dBm, alongside a */
/* much stronger 9C:53:22:AC:7C:F2 at -40dBm) - a plain WiFi.begin(ssid, pass) */
/* joins whichever one answers first, not necessarily the best one. Pinning */
/* to a specific BSSID+channel via the WiFi.begin() overload below also lets */
/* the driver skip the full-channel scan it would otherwise do during */
/* association, so this isn't pure overhead. Returns false (output */
/* parameters untouched) if the scan doesn't see str_ssid at all - callers */
/* should fall back to a plain, unpinned WiFi.begin() in that case */
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

    /* Free the memory used by the scan results */
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

/* Connect to WiFi using stored credentials from flash */
/* Returns true if connected, false if failed or no credentials stored */
bool wifi_connect_stored(void)
{
    uint32_t ui32_timeout;
    String str_ssid;
    String str_pass;
    bool b_valid;
    int32_t i32_attempt;
    uint8_t ui8_best_bssid[6];
    int32_t i32_best_channel;
    int32_t i32_best_rssi;
    bool b_have_bssid;

    /* Initialise variables */
    ui32_timeout = 0;
    b_valid = false;
    i32_best_channel = 0;
    i32_best_rssi = 0;
    b_have_bssid = false;

    /* Open flash storage in read-only mode */
    g_preferences.begin(PREF_NAMESPACE, true);
    /* Check if valid credentials are stored */
    b_valid = g_preferences.getBool(PREF_KEY_VALID, false);

    /* If no valid credentials, return immediately */
    if (b_valid == false)
    {
        Serial.printf("No stored WiFi credentials\r\n");
        g_preferences.end();
        return false;
    }

    /* Read the stored SSID and password */
    str_ssid = g_preferences.getString(PREF_KEY_SSID, "");
    str_pass = g_preferences.getString(PREF_KEY_PASS, "");
    /* Close flash storage */
    g_preferences.end();

    /* Start from a known-clean radio state rather than just WiFi.mode(WIFI_STA) */
    /* - see wifi_reset_radio(). Also disables modem sleep (by default the radio */
    /* drops into WIFI_PS_MIN_MODEM power-save between beacons right after */
    /* associating, which can race the HTTPS connect() that follows immediately */
    /* after WiFi.begin() succeeds on some AP/DTIM-interval combinations; */
    /* keeping it fully awake through the connect+upload sequence removes that */
    /* race) */
    wifi_reset_radio();

    /* Find the strongest AP currently advertising this SSID - see */
    /* wifi_find_strongest_bssid(). Done once per call, not per retry - */
    /* rescanning every attempt would multiply an already several-second cost */
    b_have_bssid = wifi_find_strongest_bssid(str_ssid, ui8_best_bssid, &i32_best_channel, &i32_best_rssi);

    /* Retry the connection attempt up to WIFI_CONNECT_RETRY_COUNT times */
    /* before giving up, since a single failed attempt is often transient */
    for (i32_attempt = 1; i32_attempt <= WIFI_CONNECT_RETRY_COUNT; i32_attempt++)
    {
        Serial.printf("Connecting to stored WiFi: %s (attempt %ld/%d)\r\n",
                      str_ssid.c_str(), i32_attempt, WIFI_CONNECT_RETRY_COUNT);

        /* Pin to the strongest known BSSID/channel if the scan found one - */
        /* see wifi_find_strongest_bssid() - otherwise fall back to letting */
        /* the driver join whichever AP answers first, same as before */
        if (b_have_bssid)
        {
            WiFi.begin(str_ssid.c_str(), str_pass.c_str(), i32_best_channel, ui8_best_bssid);
        }
        else
        {
            WiFi.begin(str_ssid.c_str(), str_pass.c_str());
        }

        /* Record the start time */
        ui32_timeout = millis();

        /* Wait for connection, a definitive rejection, or timeout - same */
        /* reasoning as wifi_connect_and_store(). Without the WL_CONNECT_FAILED */
        /* check, an AP that actively rejects the handshake (seen in the field */
        /* as a burst of AUTH_EXPIRE/ASSOC_EXPIRE/4WAY_HANDSHAKE_TIMEOUT/AUTH_FAIL */
        /* disconnect events within the first couple seconds) still burns the */
        /* full WIFI_DATA_TIMEOUT doing nothing on every one of the 5 attempts. */
        /* Unlike wifi_connect_and_store(), this does NOT give up the whole retry */
        /* loop on WL_CONNECT_FAILED - these are stored, previously-working */
        /* credentials, so a rejection here is far more likely a transient/AP-side */
        /* thing than a suddenly-wrong password, and retrying (with the backoff */
        /* below) is the right response rather than abandoning the upload */
        while (WiFi.status() != WL_CONNECTED)
        {
            uint32_t ui32_elapsed = millis() - ui32_timeout;

            if (ui32_elapsed >= WIFI_DATA_TIMEOUT)
            {
                break;
            }
            if (ui32_elapsed >= WIFI_CONNECT_FAIL_GRACE_MS && WiFi.status() == WL_CONNECT_FAILED)
            {
                break;
            }
            delay(DELAY_100MS);
        }

        /* Stop retrying as soon as we are connected AND the link can actually */
        /* reach a DNS server - WL_CONNECTED only proves 802.11 association */
        /* succeeded, not that the path past the AP is healthy. Seen in the */
        /* field: a clean connect immediately followed by a DNS lookup that */
        /* "succeeded" with 0.0.0.0, then every upload retry failing with */
        /* "read Timeout" - see wifi_check_dns_healthy(). Treating that as a */
        /* failed connection attempt (consuming one of these 5, forcing a */
        /* fresh wifi_reset_radio()) gives it a chance to land on a healthier */
        /* AP/path instead of handing send_to_server() a link that looks fine */
        /* but isn't */
        if (WiFi.status() == WL_CONNECTED)
        {
            /* BSSID identifies the specific physical AP answered by "MIS" - */
            /* if several APs/mesh nodes share that SSID, a "connection */
            /* refused" upload could be landing on one with a bad internet */
            /* uplink even though the local WiFi link itself is fine. Log it */
            /* so a recurring failure can be correlated to one BSSID */
            Serial.printf("WiFi connected, IP: %s, BSSID: %s, RSSI: %ld\r\n",
                          WiFi.localIP().toString().c_str(), WiFi.BSSIDstr().c_str(), (long)WiFi.RSSI());

            if (wifi_check_dns_healthy())
            {
                return true;
            }

            Serial.printf("WiFi connected but DNS check failed - treating as a failed attempt\r\n");
        }
        else
        {
            Serial.printf("WiFi connection attempt %ld failed\r\n", i32_attempt);
        }

        /* Reset the radio before the next attempt - see wifi_reset_radio() */
        wifi_reset_radio();

        /* Back off before retrying - see WIFI_RECONNECT_BACKOFF_BASE_MS definition */
        if (i32_attempt < WIFI_CONNECT_RETRY_COUNT)
        {
            delay(WIFI_RECONNECT_BACKOFF_BASE_MS * i32_attempt + random(0, 300));
        }
    }

    Serial.printf("WiFi connection failed for data upload after %d attempts\r\n", WIFI_CONNECT_RETRY_COUNT);
    return false;
}

/* Build the device identifier: WiFi station MAC (6 bytes) followed by the */
/* last 3 bytes of the Bluetooth MAC, hex-encoded (uppercase, no          */
/* separators) into str_hex_out, which must be at least                  */
/* DEVICE_MAC_HEX_LEN+1 bytes long */
void get_device_mac_hex(char *str_hex_out)
{
    static const char c_hex_digits[] = "0123456789ABCDEF";
    uint8_t ui8_mac_wifi[DEVICE_MAC_WIFI_LEN];
    uint8_t ui8_mac_bt[6];
    uint8_t ui8_mac_combined[DEVICE_MAC_LEN];
    int32_t i32_idx;

    /* WiFi station MAC is the chip's base MAC address */
    WiFi.macAddress(ui8_mac_wifi);
    /* Bluetooth MAC is derived from the base MAC with a small offset */
    esp_read_mac(ui8_mac_bt, ESP_MAC_BT);

    memcpy(ui8_mac_combined, ui8_mac_wifi, DEVICE_MAC_WIFI_LEN);
    memcpy(ui8_mac_combined + DEVICE_MAC_WIFI_LEN,
           ui8_mac_bt + (sizeof(ui8_mac_bt) - DEVICE_MAC_BT_LEN),
           DEVICE_MAC_BT_LEN);

    for (i32_idx = 0; i32_idx < DEVICE_MAC_LEN; i32_idx++)
    {
        str_hex_out[i32_idx * 2]     = c_hex_digits[(ui8_mac_combined[i32_idx] >> 4) & 0x0F];
        str_hex_out[i32_idx * 2 + 1] = c_hex_digits[ui8_mac_combined[i32_idx] & 0x0F];
    }
    str_hex_out[DEVICE_MAC_HEX_LEN] = '\0';
}

/* Percent-encodes str_in per RFC 3986 (unreserved = A-Za-z0-9-._~), so */
/* values placed into a query string can't be misread as part of the URL */
/* structure - or, on this host, mistaken by the WAF for something else. */
/* A bcrypt hash's literal "$2y$10$..." shape in particular reads like a */
/* PHP-injection payload to some WAF rulesets if sent unencoded. */
String url_encode(const String &str_in)
{
    static const char c_hex_digits[] = "0123456789ABCDEF";
    String str_out;
    char c_ch;
    size_t sz_idx;

    for (sz_idx = 0; sz_idx < str_in.length(); sz_idx++)
    {
        c_ch = str_in[sz_idx];
        if (isalnum((unsigned char)c_ch) || c_ch == '-' || c_ch == '.' || c_ch == '_' || c_ch == '~')
        {
            str_out += c_ch;
        }
        else
        {
            str_out += '%';
            str_out += c_hex_digits[((unsigned char)c_ch >> 4) & 0x0F];
            str_out += c_hex_digits[(unsigned char)c_ch & 0x0F];
        }
    }

    return str_out;
}

/* Sends this device's MAC together with the dashboard username/password */
/* hash received over BLE (if any) to claim_device.php, linking the */
/* device to that account. Called once, right after WiFi connects during */
/* BLE setup. No-op if the phone never sent a user credential - the */
/* device just stays unclaimed (user_id=0) */
void claim_device(void)
{
    String str_url;
    char str_mac_hex[DEVICE_MAC_HEX_LEN + 1];
    int32_t i32_response_code;
    int32_t i32_attempt;

    if (!g_b_user_credential_received)
    {
        return;
    }

    get_device_mac_hex(str_mac_hex);

    str_url = CLAIM_URL;
    str_url += "?mac=";
    str_url += str_mac_hex;
    str_url += "&username=";
    str_url += url_encode(g_str_received_username);
    str_url += "&hash=";
    str_url += url_encode(g_str_received_password_hash);

    /* Retry like send_to_server() does - a single transient connection */
    /* failure shouldn't leave the device permanently unclaimed */
    for (i32_attempt = 1; i32_attempt <= DATA_UPLOAD_RETRY_COUNT; i32_attempt++)
    {
        WiFiClientSecure wifi_client;
        HTTPClient http_client;

        Serial.printf("Claiming device (attempt %ld/%d): %s\r\n",
                      i32_attempt, DATA_UPLOAD_RETRY_COUNT, str_url.c_str());

        /* Skip certificate verification for now */
        /* Production should use a root CA certificate */
        wifi_client.setInsecure();
        /* See TLS_HANDSHAKE_TIMEOUT_S definition - kept separate from the */
        /* connect/response budget below so a slow-but-fine handshake on a */
        /* weak link isn't mistaken for a dead connection */
        wifi_client.setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_S);

        if (http_client.begin(wifi_client, str_url))
        {
            http_client.setConnectTimeout(DATA_UPLOAD_TIMEOUT_MS);
            http_client.setTimeout(DATA_UPLOAD_TIMEOUT_MS);
            http_client.addHeader("User-Agent", "Mozilla/5.0 (NimbusWeatherStation)");
            http_client.addHeader("Accept", "*/*");

            i32_response_code = http_client.GET();

            if (i32_response_code == 200)
            {
                /* Check the body, not just the status code - this host has */
                /* previously been seen returning a bare 200 block/challenge */
                /* page instead of running the PHP behind it (see the */
                /* User-Agent fix in send_to_server()), which would otherwise */
                /* look identical to a real claim success here */
                String str_response_body = http_client.getString();
                Serial.printf("Claim device response: %ld\r\n", i32_response_code);
                Serial.printf("Claim response body: %s\r\n", str_response_body.c_str());
                http_client.end();

                if (str_response_body == "OK")
                {
                    /* Tell the phone the claim genuinely succeeded - without */
                    /* this, the app has no way to distinguish "claimed" from */
                    /* "WiFi joined but the claim itself silently failed" and */
                    /* was reporting the latter as success too */
                    send_status("CLAIM_OK");
                    g_b_user_credential_received = false;
                    return;
                }

                Serial.printf("Claim device: 200 but unexpected body - treating as failure\r\n");
                send_status("CLAIM_FAILED");
                g_b_user_credential_received = false;
                return;
            }
            else if (i32_response_code > 0)
            {
                /* server responded but rejected the claim (e.g. hash didn't */
                /* match) - retrying won't change that outcome, so tell the */
                /* phone now instead of burning the remaining attempts */
                Serial.printf("Claim device rejected: %ld\r\n", i32_response_code);
                Serial.printf("Claim response body: %s\r\n", http_client.getString().c_str());
                http_client.end();
                send_status("CLAIM_FAILED");
                g_b_user_credential_received = false;
                return;
            }

            Serial.printf("Claim device HTTPS GET failed, error: %s (free heap: %u)\r\n",
                          http_client.errorToString(i32_response_code).c_str(), ESP.getFreeHeap());
            http_client.end();
        }
        else
        {
            Serial.printf("Claim device: failed to begin HTTPS connection (free heap: %u)\r\n", ESP.getFreeHeap());
        }

        /* Same backoff+jitter reasoning as send_to_server() */
        if (i32_attempt < DATA_UPLOAD_RETRY_COUNT)
        {
            delay(DATA_UPLOAD_RETRY_DELAY_MS * i32_attempt + random(0, 500));
        }
    }

    /* Every attempt failed at the connection level (never got a response */
    /* at all) - let the phone know instead of leaving it thinking CONNECT_OK */
    /* (WiFi joined fine) meant the whole setup succeeded */
    Serial.printf("Claim device failed after %d attempts\r\n", DATA_UPLOAD_RETRY_COUNT);
    send_status("CLAIM_FAILED");
    g_b_user_credential_received = false;
}

/* Send measurement data to the server via HTTPS GET */
/* URL format: update_alx_setup.php?T2334H6722P98823B3294MD83BDA75F32C011 */
void send_to_server(int32_t i32_temperature, int32_t i32_pressure, int32_t i32_humidity, int32_t i32_battery)
{
    String str_url;
    int32_t i32_response_code;
    int32_t i32_attempt;
    int32_t i32_rssi;
    char str_mac_hex[DEVICE_MAC_HEX_LEN + 1];

    /* Initialise variables */
    i32_response_code = 0;
    get_device_mac_hex(str_mac_hex);
    /* current link strength, dBm (negative) - read now since it's only */
    /* meaningful right after wifi_connect_stored() brought the radio up */
    i32_rssi = WiFi.RSSI();

    /* Build the URL with measurement data as query parameter */
    str_url = DATA_URL;
    str_url += "?T";
    str_url += String(i32_temperature);
    str_url += "H";
    str_url += String(i32_humidity);
    str_url += "P";
    str_url += String(i32_pressure);
    str_url += "B";
    str_url += String(i32_battery);
    str_url += "S";
    str_url += String(i32_rssi);
    str_url += "M";
    str_url += str_mac_hex;

    /* Retry the upload up to DATA_UPLOAD_RETRY_COUNT times - transient */
    /* failures like "connection refused" often succeed on the next try */
    for (i32_attempt = 1; i32_attempt <= DATA_UPLOAD_RETRY_COUNT; i32_attempt++)
    {
        WiFiClientSecure wifi_client;
        HTTPClient http_client;

        Serial.printf("Sending data (attempt %ld/%d): %s\r\n",
                      i32_attempt, DATA_UPLOAD_RETRY_COUNT, str_url.c_str());

        /* Skip certificate verification for now */
        /* Production should use a root CA certificate */
        wifi_client.setInsecure();
        /* Bound the TLS handshake, but with its own more generous budget - */
        /* see TLS_HANDSHAKE_TIMEOUT_S definition */
        wifi_client.setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_S);

        /* Begin the HTTPS connection */
        if (http_client.begin(wifi_client, str_url))
        {
            /* Bound both the TCP connect and the response-read wait - without */
            /* this, a stalled socket can hang far longer than expected per attempt */
            http_client.setConnectTimeout(DATA_UPLOAD_TIMEOUT_MS);
            http_client.setTimeout(DATA_UPLOAD_TIMEOUT_MS);

            /* Identify as a normal client - some hosts silently block/challenge */
            /* requests with no User-Agent (bot protection), returning 200 with */
            /* a block page instead of running the script */
            http_client.addHeader("User-Agent", "Mozilla/5.0 (NimbusWeatherStation)");
            http_client.addHeader("Accept", "*/*");

            /* Send the GET request */
            i32_response_code = http_client.GET();

            /* Check the response */
            if (i32_response_code > 0)
            {
                /* Log the status code AND the response body - a 200 status */
                /* does not by itself prove update_alx.php actually ran; a */
                /* WAF/bot-protection block page can also return 200 */
                Serial.printf("Server response: %ld\r\n", i32_response_code);
                Serial.printf("Response body: %s\r\n", http_client.getString().c_str());
                /* Close the connection and stop retrying - request succeeded */
                http_client.end();
                return;
            }

            /* Free heap alongside the error - a "connection refused" that */
            /* only shows up when heap is low points at local TLS/socket */
            /* resource exhaustion rather than the host rejecting us */
            Serial.printf("HTTPS GET failed, error: %s (free heap: %u)\r\n",
                          http_client.errorToString(i32_response_code).c_str(), ESP.getFreeHeap());
            /* Close the connection before the next attempt */
            http_client.end();
        }
        else
        {
            Serial.printf("Unable to connect to server (free heap: %u)\r\n", ESP.getFreeHeap());
        }

        /* Back off before retrying, unless this was the last attempt. */
        /* Delay grows with each attempt plus random jitter so 3 retries */
        /* in a row don't look like a connection flood to the host's */
        /* firewall/rate-limiter - a flat 1s gap between identical requests */
        /* is exactly what trips that kind of protection */
        if (i32_attempt < DATA_UPLOAD_RETRY_COUNT)
        {
            delay(DATA_UPLOAD_RETRY_DELAY_MS * i32_attempt + random(0, 500));
        }
    }

    Serial.printf("Data upload failed after %d attempts\r\n", DATA_UPLOAD_RETRY_COUNT);
}

/* Send a status notification string to the phone via the status characteristic */
void send_status(const char *p_message)
{
    /* Only send if a client is connected and the characteristic exists */
    if (g_b_client_connected && gp_char_status != NULL)
    {
        /* Set the value on the characteristic */
        gp_char_status->setValue(p_message);
        /* Send the notification to the phone */
        gp_char_status->notify();
        Serial.printf("Status sent: %s\r\n", p_message);
    }
}

/* Initialise the BLE GATT server with all services and characteristics */
void ble_setup(void)
{
    BLEService *p_service;

    /* Initialise all BLE state flags */
    g_b_ble_active = false;
    g_b_client_connected = false;
    g_b_scan_requested = false;
    g_b_credential_received = false;
    g_b_user_credential_received = false;

    /* Initialise the BLE stack with the device name */
    BLEDevice::init(BLE_DEVICE_NAME);

    /* Create the BLE server and register connection callbacks */
    gp_server = BLEDevice::createServer();
    gp_server->setCallbacks(new NimbusServerCallbacks());
  
    /* Create the GATT service using the service UUID */
    p_service = gp_server->createService(SERVICE_UUID);

    /* Create the command characteristic - phone writes commands here */
    gp_char_command = p_service->createCharacteristic(
        CHAR_COMMAND_UUID,
        BLECharacteristic::PROPERTY_WRITE
    );
    /* Register the command callback */
    gp_char_command->setCallbacks(new CommandCallbacks());

    /* Create the wifi list characteristic - esp sends scan results here */
    gp_char_wifilist = p_service->createCharacteristic(
        CHAR_WIFILIST_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );
    /* Add the client characteristic configuration descriptor for notifications */
    gp_char_wifilist->addDescriptor(new BLE2902());

    /* Create the credential characteristic - phone writes ssid|password here */
    gp_char_credential = p_service->createCharacteristic(
        CHAR_CREDENTIAL_UUID,
        BLECharacteristic::PROPERTY_WRITE
    );
    /* Register the credential callback */
    gp_char_credential->setCallbacks(new CredentialCallbacks());

    /* Create the user credential characteristic - phone writes */
    /* username|password_hash here, if logged into the dashboard */
    gp_char_user = p_service->createCharacteristic(
        CHAR_USER_UUID,
        BLECharacteristic::PROPERTY_WRITE
    );
    /* Register the user credential callback */
    gp_char_user->setCallbacks(new UserCredentialCallbacks());

    /* Create the status characteristic - esp sends status notifications here */
    gp_char_status = p_service->createCharacteristic(
        CHAR_STATUS_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );
    /* Add the client characteristic configuration descriptor for notifications */
    gp_char_status->addDescriptor(new BLE2902());

    /* Start the GATT service */
    p_service->start();

    BLEAdvertising *p_advertising;
    BLEAdvertisementData adv_data;

    /* Get the advertising handle */
    p_advertising = BLEDevice::getAdvertising();
    /* Put the device name in the advertisement data */
    adv_data.setName(BLE_DEVICE_NAME);
    /* Include the service UUID so the phone can filter by it */
    adv_data.setCompleteServices(BLEUUID(SERVICE_UUID));
    /* Load the data into the primary advertisement packet */
    p_advertising->setAdvertisementData(adv_data);
    /* Enable the scan response packet for additional data space */
    p_advertising->setScanResponse(true);
    /* Start broadcasting */
    BLEDevice::startAdvertising();

    /* Set the global flag */
    g_b_ble_active = true;

    Serial.printf("BLE GATT server started as '%s'\r\n", BLE_DEVICE_NAME);
}

/* Scan for 2.4GHz WiFi networks and send the results to the phone via BLE */
/* Scan for 2.4GHz WiFi networks and send the results to the phone via BLE */
/* Each network is sent as a separate notification to avoid MTU limits */
void wifi_scan_and_send(void)
{
    int32_t i32_network_count;
    String str_entry;
    int32_t i32_i;
    int32_t i32_sent;

    /* Notify the phone that scanning has started */
    send_status("SCANNING");
    Serial.printf("Starting WiFi scan...\r\n");

    /* Set WiFi to station mode and disconnect from any previous connection */
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    /* Short delay to let the WiFi stack settle */
    delay(DELAY_100MS);

    /* Perform a synchronous scan of all available networks */
    i32_network_count = WiFi.scanNetworks();

    Serial.printf("Scan complete, found %ld networks\r\n", i32_network_count);

    /* If no networks found, notify the phone and return */
    if (i32_network_count <= 0)
    {
        String str_status;
        str_status = "SCAN_DONE:0";
        send_status(str_status.c_str());
        return;
    }

    /* Send each network as a separate BLE notification */
    i32_i = 0;
    i32_sent = 0;

    while (i32_i < i32_network_count && i32_i < WIFI_MAX_NETWORKS)
    {
        /* Only include 2.4GHz networks (channels 1 through 14) */
        if (WiFi.channel(i32_i) <= 14)
        {
            /* Build the entry string: "SSID,RSSI,encrypted" */
            str_entry = WiFi.SSID(i32_i);
            str_entry += ",";
            str_entry += String(WiFi.RSSI(i32_i));
            str_entry += ",";
            str_entry += (WiFi.encryptionType(i32_i) != WIFI_AUTH_OPEN) ? "1" : "0";

            /* Set the value on the characteristic */
            gp_char_wifilist->setValue(str_entry.c_str());
            /* Send the notification to the phone */
            gp_char_wifilist->notify();

            Serial.printf("Sent network: %s\r\n", str_entry.c_str());

            /* Increment the sent counter */
            i32_sent++;

            /* Small delay between notifications to avoid BLE congestion */
            delay(DELAY_100MS);
        }
        i32_i++;
    }

    /* Free the memory used by the scan results */
    WiFi.scanDelete();

    /* Notify the phone that scanning is complete */
    String str_done;
    str_done = "SCAN_DONE:" + String(i32_sent);
    send_status(str_done.c_str());
}
/* Attempt to connect to WiFi using the received credentials */
/* On success, store the credentials to flash for future use */
void wifi_connect_and_store(void)
{
    uint32_t ui32_timeout;
    int32_t i32_attempt;
    String str_ssid;
    String str_pass;
    uint8_t ui8_best_bssid[6];
    int32_t i32_best_channel;
    int32_t i32_best_rssi;
    bool b_have_bssid;

    i32_best_channel = 0;
    i32_best_rssi = 0;
    b_have_bssid = false;

    /* Snapshot the credentials this call is connecting with. The phone can */
    /* write a new credential over BLE at any time, including while this */
    /* function is still retrying an earlier one - delay() below yields to */
    /* the BLE stack, so its callback can run and overwrite */
    /* g_str_received_ssid/pass mid-loop. Reading the globals fresh on each */
    /* retry let a stale attempt silently swap to different credentials */
    /* partway through and confused the WiFi driver into refusing to */
    /* reconnect ("sta is connecting, cannot set config"). Any credential */
    /* that arrives during this call is left for the next one, once */
    /* g_b_credential_received is set again. */
    str_ssid = g_str_received_ssid;
    str_pass = g_str_received_pass;

    /* Notify the phone that connection is being attempted */
    send_status("CONNECTING");

    /* Start from a known-clean radio state rather than just WiFi.mode(WIFI_STA) */
    /* - see wifi_reset_radio(). Particularly relevant here: wifi_scan_and_send() */
    /* normally just ran on this same BLE session (the phone scans before */
    /* sending credentials), leaving the radio in a post-scan state right */
    /* before this connect attempt - exactly the "brand new BLE session" */
    /* scenario wifi_reset_radio()'s own comment calls out */
    wifi_reset_radio();

    /* Find the strongest AP currently advertising this SSID - see */
    /* wifi_find_strongest_bssid(). Same reasoning as wifi_connect_stored(): */
    /* a plain WiFi.begin(ssid, pass) joins whichever AP answers first, which */
    /* can be a weak mesh node even when a much stronger one is in range */
    b_have_bssid = wifi_find_strongest_bssid(str_ssid, ui8_best_bssid, &i32_best_channel, &i32_best_rssi);

    /* Retry the connection attempt up to WIFI_CONNECT_RETRY_COUNT times */
    /* before giving up, since a single failed attempt is often transient */
    for (i32_attempt = 1; i32_attempt <= WIFI_CONNECT_RETRY_COUNT; i32_attempt++)
    {
        Serial.printf("Connecting to '%s' (attempt %ld/%d)...\r\n",
                      str_ssid.c_str(), i32_attempt, WIFI_CONNECT_RETRY_COUNT);

        /* Pin to the strongest known BSSID/channel if the scan found one - */
        /* see wifi_find_strongest_bssid() - otherwise fall back to letting */
        /* the driver join whichever AP answers first, same as before */
        if (b_have_bssid)
        {
            WiFi.begin(str_ssid.c_str(), str_pass.c_str(), i32_best_channel, ui8_best_bssid);
        }
        else
        {
            WiFi.begin(str_ssid.c_str(), str_pass.c_str());
        }

        /* Record the start time for timeout detection */
        ui32_timeout = millis();

        /* Wait for connection to establish, a definitive rejection (e.g. */
        /* wrong password), or timeout to expire. WL_CONNECT_FAILED is only */
        /* trusted after WIFI_CONNECT_FAIL_GRACE_MS - right after */
        /* WiFi.begin(), status() can still briefly return the *previous* */
        /* attempt's leftover WL_CONNECT_FAILED before the driver updates it */
        /* to reflect this attempt, which otherwise looks identical to a */
        /* real, immediate rejection of a perfectly correct password */
        while (WiFi.status() != WL_CONNECTED)
        {
            uint32_t ui32_elapsed = millis() - ui32_timeout;

            if (ui32_elapsed >= WIFI_CONNECT_TIMEOUT)
            {
                break;
            }
            if (ui32_elapsed >= WIFI_CONNECT_FAIL_GRACE_MS && WiFi.status() == WL_CONNECT_FAILED)
            {
                break;
            }
            /* Poll at 100ms intervals */
            delay(DELAY_100MS);
        }

        /* Stop retrying as soon as we are connected AND the link can actually */
        /* reach a DNS server - see the matching gate + wifi_check_dns_healthy() */
        /* in wifi_connect_stored() for why WL_CONNECTED alone isn't enough */
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
        /* with the same credentials would only fail the same way again, */
        /* so give up immediately instead of burning the remaining attempts */
        else if (WiFi.status() == WL_CONNECT_FAILED)
        {
            Serial.printf("WiFi connection rejected (wrong password?) - not retrying\r\n");
            /* Reset the radio, not just disconnect - see wifi_reset_radio(). */
            /* Without this, the driver is left in a bad state and every */
            /* future WiFi.begin() silently fails too, even a later, */
            /* correct credential on a fresh BLE session. */
            wifi_reset_radio();
            break;
        }
        else
        {
            Serial.printf("WiFi connection attempt %ld failed\r\n", i32_attempt);
        }

        /* Reset the radio before the next attempt - a plain disconnect() */
        /* is not enough here either (see wifi_reset_radio()), and also */
        /* avoids "sta is connecting, cannot set config" from starting a */
        /* new WiFi.begin() while the previous attempt is still unwinding. */
        /* Shared by both the "connected but DNS unhealthy" and plain */
        /* "didn't connect" cases above - the WL_CONNECT_FAILED case already */
        /* reset the radio and broke out before reaching here */
        wifi_reset_radio();

        /* Back off before retrying - see WIFI_RECONNECT_BACKOFF_BASE_MS definition */
        if (i32_attempt < WIFI_CONNECT_RETRY_COUNT)
        {
            delay(WIFI_RECONNECT_BACKOFF_BASE_MS * i32_attempt + random(0, 300));
        }
    }

    /* If still not connected after all retries, report failure */
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.printf("WiFi connection failed after %d attempts\r\n", WIFI_CONNECT_RETRY_COUNT);
        /* Notify the phone that connection failed */
        send_status("CONNECT_FAIL");
        /* Reset the radio - loop() may invoke this function again */
        /* immediately if a newer credential is already waiting (see the */
        /* snapshot note above), and it needs a clean radio to succeed */
        wifi_reset_radio();
        return;
    }

    /* See the BSSID note in wifi_connect_stored() */
    Serial.printf("WiFi connected, IP: %s, BSSID: %s, RSSI: %ld\r\n",
                  WiFi.localIP().toString().c_str(), WiFi.BSSIDstr().c_str(), (long)WiFi.RSSI());

    /* Open the flash storage namespace for writing */
    g_preferences.begin(PREF_NAMESPACE, false);
    /* Store the SSID */
    g_preferences.putString(PREF_KEY_SSID, str_ssid);
    /* Store the password */
    g_preferences.putString(PREF_KEY_PASS, str_pass);
    /* Set the valid flag so future boots know credentials are stored */
    g_preferences.putBool(PREF_KEY_VALID, true);
    /* Close the flash storage */
    g_preferences.end();

    Serial.printf("Credentials stored to flash\r\n");

    /* Notify the phone that connection was successful */
    send_status("CONNECT_OK");

    /* While WiFi is still up, claim this device for whichever dashboard */
    /* account the phone was logged into (no-op if none was received) */
    claim_device();

    /* Disconnect WiFi - the next measurement cycle will reconnect */
    WiFi.disconnect();
}

/* Parse the measurement data string received from the STM32 via UART */
/* Then upload the data to the server via HTTPS */
void parse_measurement(String str_data)
{
    int32_t i32_temperature;
    int32_t i32_pressure;
    int32_t i32_humidity;
    int32_t i32_battery;
    int32_t i32_parsed;

    /* Initialise all variables */
    i32_temperature = 0;
    i32_pressure = 0;
    i32_humidity = 0;
    i32_battery = 0;
    i32_parsed = 0;

    /* Parse the four named fields from the STM32 data string */
    i32_parsed = sscanf(str_data.c_str(),
                        "Temperature: %ld Pressure: %ld Humidity: %ld Battery: %ld",
                        &i32_temperature,
                        &i32_pressure,
                        &i32_humidity,
                        &i32_battery);

    /* Confirm all four fields were successfully extracted */
    if (i32_parsed == PARSE_SUCCESS)
    {
        /* Split display only, not the actual uploaded value - i32_temperature */
        /* itself is signed and sent/parsed correctly end-to-end. But splitting */
        /* it into integer + fraction parts for this log line loses the sign */
        /* whenever the integer part truncates to exactly 0 (e.g. -57 -> "0.57", */
        /* silently reading as +0.57C instead of -0.57C) since plain 0 has no */
        /* separate negative representation - print the sign explicitly instead */
        /* of relying on the integer part to carry it */
        Serial.printf("Temperature: %s%ld.%02ld C\r\n",
                      (i32_temperature < 0) ? "-" : "",
                      abs(i32_temperature / TEMP_SCALE), abs(i32_temperature % TEMP_SCALE));
        Serial.printf("Pressure:    %ld Pa\r\n", i32_pressure);
        Serial.printf("Humidity:    %ld.%03ld %%\r\n", i32_humidity / HUM_SCALE, abs(i32_humidity % HUM_SCALE));
        Serial.printf("Battery:     %ld mV\r\n", i32_battery);

        /* Connect to WiFi using stored credentials */
        if (wifi_connect_stored())
        {
            /* Upload data to the server */
            send_to_server(i32_temperature, i32_pressure, i32_humidity, i32_battery);
            /* Disconnect WiFi after upload */
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

void setup()
{
    /* Initialise the BLE active flag */
    g_b_ble_active = false;
    /* Initialise all BLE communication flags */
    g_b_client_connected = false;
    g_b_scan_requested = false;
    g_b_credential_received = false;
    g_b_user_credential_received = false;
    /* Clear any stale credential strings */
    g_str_received_ssid = "";
    g_str_received_pass = "";
    g_str_received_username = "";
    g_str_received_password_hash = "";

    /* Init for GPIO to signal the readiness to STM32 */
    pinMode(READY_PIN, OUTPUT);
    digitalWrite(READY_PIN, LOW);

    /* Builtin LED, just turn on to indicate that system is working */
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);

    /* Debug UART + STM32 link share this one physical port (see the */
    /* #define Serial Serial1 above), so it has to run at the STM32's fixed */
    /* UART_BAUD (9600) rather than a faster debug-only rate - an external */
    /* USB-UART adapter reading TX_PIN just needs to be set to 9600 8N1 */
    Serial.begin(UART_BAUD, SERIAL_8N1, RX_PIN, TX_PIN);
    /* Flush any bootloader garbage sitting in RX buffer */
    delay(DELAY_100MS);
    while (Serial.available())
    {
        Serial.read();
    }

    /* Log the specific reason for every WiFi disconnect (see */
    /* wifi_event_handler) - wl_status_t alone can't tell "AP not found" */
    /* apart from "wrong password" apart from "signal dropped mid-connect" */
    WiFi.onEvent(wifi_event_handler);

    Serial.printf("Debug UART initialized\r\n");
}

void loop()
{
    String str_data;
    static uint32_t s_ui32_last_heartbeat = 0;

    /* Heartbeat print every 5 seconds to confirm loop is running */
    if ((millis() - s_ui32_last_heartbeat) >= 5000u)
    {
        Serial.printf("Loop running, BLE=%d, scan_req=%d, cred_req=%d\r\n",
                      g_b_client_connected, g_b_scan_requested, g_b_credential_received);
        s_ui32_last_heartbeat = millis();
    }

    /* Check if data is available from the STM32 */
    if (Serial1.available())
    {
        /* Read the incoming line until newline */
        str_data = Serial1.readStringUntil('\n');
        /* Remove any trailing carriage return or whitespace */
        str_data.trim();

        /* Check if this is a measurement data line */
        if (str_data.indexOf("Temperature") >= 0)
        {
            /* Signal readiness to STM32 */
            digitalWrite(READY_PIN, HIGH);
            /* Parse the measurement data */
            parse_measurement(str_data);
            digitalWrite(READY_PIN, LOW);
        }
        /* Check if this is a setup request */
        else if (str_data.indexOf("Setup") >= 0)
        {
            /* Signal readiness to STM32 */
            digitalWrite(READY_PIN, HIGH);
            /* Enter setup mode - start BLE GATT server for provisioning */
            ble_setup();
        }
    }

    /* Handle a WiFi scan request from the phone (set by command callback) */
    if (g_b_scan_requested)
    {
        /* Clear the flag before processing */
        g_b_scan_requested = false;
        /* Perform the WiFi scan and send results to the phone */
        wifi_scan_and_send();
    }

    /* Handle received WiFi credentials from the phone (set by credential callback) */
    if (g_b_credential_received)
    {
        /* Clear the flag before processing */
        g_b_credential_received = false;
        /* Attempt to connect and store credentials on success. Deliberately */
        /* does NOT pull READY_PIN low here - that would tell the STM32 the */
        /* whole setup session is finished after just one attempt, even */
        /* though the phone may still be about to retry with a different */
        /* password. READY_PIN only goes low once BLE actually disconnects */
        /* (see NimbusServerCallbacks::onDisconnect) */
        wifi_connect_and_store();
        Serial.printf ("DONE, network connected and stored\r\n");
    }
}