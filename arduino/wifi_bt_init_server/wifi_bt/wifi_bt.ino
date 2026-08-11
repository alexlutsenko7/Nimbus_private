#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <WiFi.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_mac.h>

/* Pin definitions */
#define RX_PIN              D7
#define TX_PIN              D6
#define READY_PIN           D5
#define UART_BAUD           9600
#define UARTUSB_BAUD        115200
#define UART_INIT_TIME      200u
#define DELAY_10MS          10u
#define DELAY_100MS         100u
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

/* Server URL for data upload */
//#define DATA_URL            "https://alxlabs.ca/books/env_sensor/web/data.php"
#define DATA_URL            "https://alxlabs.ca/books/env_sensor/web/update_alx_setup.php"
/* WiFi connection timeout for data upload */
#define WIFI_DATA_TIMEOUT   15000u

/* BLE UUIDs for the GATT service and characteristics */
#define SERVICE_UUID          "12345678-1234-1234-1234-123456789ABC"
#define CHAR_COMMAND_UUID     "12345678-1234-1234-1234-123456789ABD"
#define CHAR_WIFILIST_UUID    "12345678-1234-1234-1234-123456789ABE"
#define CHAR_CREDENTIAL_UUID  "12345678-1234-1234-1234-123456789ABF"
#define CHAR_STATUS_UUID      "12345678-1234-1234-1234-123456789AC0"

/* BLE device name visible to scanning phones */
#define BLE_DEVICE_NAME     "Weather station"

/* WiFi scan and connection parameters */
#define WIFI_SCAN_TIMEOUT   10000u
#define WIFI_CONNECT_TIMEOUT 15000u
#define WIFI_CONNECT_RETRY_COUNT 5
#define DATA_UPLOAD_RETRY_COUNT 3
#define DATA_UPLOAD_RETRY_DELAY_MS 1000u
#define DATA_UPLOAD_TIMEOUT_MS 8000u
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

/* Connect to WiFi using stored credentials from flash */
/* Returns true if connected, false if failed or no credentials stored */
bool wifi_connect_stored(void)
{
    uint32_t ui32_timeout;
    String str_ssid;
    String str_pass;
    bool b_valid;
    int32_t i32_attempt;

    /* Initialise variables */
    ui32_timeout = 0;
    b_valid = false;

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

    /* Set WiFi to station mode */
    WiFi.mode(WIFI_STA);

    /* Retry the connection attempt up to WIFI_CONNECT_RETRY_COUNT times */
    /* before giving up, since a single failed attempt is often transient */
    for (i32_attempt = 1; i32_attempt <= WIFI_CONNECT_RETRY_COUNT; i32_attempt++)
    {
        Serial.printf("Connecting to stored WiFi: %s (attempt %ld/%d)\r\n",
                      str_ssid.c_str(), i32_attempt, WIFI_CONNECT_RETRY_COUNT);

        /* Begin the connection attempt */
        WiFi.begin(str_ssid.c_str(), str_pass.c_str());

        /* Record the start time */
        ui32_timeout = millis();

        /* Wait for connection or timeout */
        while (WiFi.status() != WL_CONNECTED)
        {
            /* Check if timeout expired */
            if ((millis() - ui32_timeout) >= WIFI_DATA_TIMEOUT)
            {
                break;
            }
            delay(DELAY_100MS);
        }

        /* Stop retrying as soon as we are connected */
        if (WiFi.status() == WL_CONNECTED)
        {
            Serial.printf("WiFi connected, IP: %s\r\n", WiFi.localIP().toString().c_str());
            return true;
        }

        Serial.printf("WiFi connection attempt %ld failed\r\n", i32_attempt);
        /* Disconnect cleanly before the next attempt */
        WiFi.disconnect();
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

/* Send measurement data to the server via HTTPS GET */
/* URL format: update_alx_setup.php?T2334H6722P98823B3294MD83BDA75F32C011 */
void send_to_server(int32_t i32_temperature, int32_t i32_pressure, int32_t i32_humidity, int32_t i32_battery)
{
    String str_url;
    int32_t i32_response_code;
    int32_t i32_attempt;
    char str_mac_hex[DEVICE_MAC_HEX_LEN + 1];

    /* Initialise variables */
    i32_response_code = 0;
    get_device_mac_hex(str_mac_hex);

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
        /* Bound the TLS handshake so a stalled connection can't hang past this */
        wifi_client.setHandshakeTimeout(DATA_UPLOAD_TIMEOUT_MS / 1000u);

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

            Serial.printf("HTTPS GET failed, error: %s\r\n", http_client.errorToString(i32_response_code).c_str());
            /* Close the connection before the next attempt */
            http_client.end();
        }
        else
        {
            Serial.printf("Unable to connect to server\r\n");
        }

        /* Short delay before retrying, unless this was the last attempt */
        if (i32_attempt < DATA_UPLOAD_RETRY_COUNT)
        {
            delay(DATA_UPLOAD_RETRY_DELAY_MS);
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

    /* Notify the phone that connection is being attempted */
    send_status("CONNECTING");

    /* Set WiFi to station mode */
    WiFi.mode(WIFI_STA);

    /* Retry the connection attempt up to WIFI_CONNECT_RETRY_COUNT times */
    /* before giving up, since a single failed attempt is often transient */
    for (i32_attempt = 1; i32_attempt <= WIFI_CONNECT_RETRY_COUNT; i32_attempt++)
    {
        Serial.printf("Connecting to '%s' (attempt %ld/%d)...\r\n",
                      g_str_received_ssid.c_str(), i32_attempt, WIFI_CONNECT_RETRY_COUNT);

        /* Begin the connection attempt with the received credentials */
        WiFi.begin(g_str_received_ssid.c_str(), g_str_received_pass.c_str());

        /* Record the start time for timeout detection */
        ui32_timeout = millis();

        /* Wait for connection to establish or timeout to expire */
        while (WiFi.status() != WL_CONNECTED)
        {
            /* Check if the timeout has been exceeded */
            if ((millis() - ui32_timeout) >= WIFI_CONNECT_TIMEOUT)
            {
                break;
            }
            /* Poll at 100ms intervals */
            delay(DELAY_100MS);
        }

        /* Stop retrying as soon as we are connected */
        if (WiFi.status() == WL_CONNECTED)
        {
            break;
        }

        Serial.printf("WiFi connection attempt %ld failed\r\n", i32_attempt);
        /* Disconnect cleanly before the next attempt */
        WiFi.disconnect();
    }

    /* If still not connected after all retries, report failure */
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.printf("WiFi connection failed after %d attempts\r\n", WIFI_CONNECT_RETRY_COUNT);
        /* Notify the phone that connection failed */
        send_status("CONNECT_FAIL");
        /* Disconnect cleanly */
        WiFi.disconnect();
        return;
    }

    Serial.printf("WiFi connected, IP: %s\r\n", WiFi.localIP().toString().c_str());

    /* Open the flash storage namespace for writing */
    g_preferences.begin(PREF_NAMESPACE, false);
    /* Store the SSID */
    g_preferences.putString(PREF_KEY_SSID, g_str_received_ssid);
    /* Store the password */
    g_preferences.putString(PREF_KEY_PASS, g_str_received_pass);
    /* Set the valid flag so future boots know credentials are stored */
    g_preferences.putBool(PREF_KEY_VALID, true);
    /* Close the flash storage */
    g_preferences.end();

    Serial.printf("Credentials stored to flash\r\n");

    /* Notify the phone that connection was successful */
    send_status("CONNECT_OK");

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
        Serial.printf("Temperature: %ld.%02ld C\r\n", i32_temperature / TEMP_SCALE, abs(i32_temperature % TEMP_SCALE));
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
    uint32_t ui32_timeout;
    ui32_timeout = 0;

    /* Initialise the BLE active flag */
    g_b_ble_active = false;
    /* Initialise all BLE communication flags */
    g_b_client_connected = false;
    g_b_scan_requested = false;
    g_b_credential_received = false;
    /* Clear any stale credential strings */
    g_str_received_ssid = "";
    g_str_received_pass = "";

    /* Init for GPIO to signal the readiness to STM32 */
    pinMode(READY_PIN, OUTPUT);
    digitalWrite(READY_PIN, LOW);

    /* Builtin LED, just turn on to indicate that system is working */
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);

    /* Standard serial port via the USB, it might be available (USB connected) or not */
    Serial.begin(UARTUSB_BAUD);
    /* Wait for USB serial with timeout - continues without it on battery power */
    ui32_timeout = millis();
    while (!Serial && ((millis() - ui32_timeout) < UART_INIT_TIME))
    {
        delay(DELAY_10MS);
    }

    /* Hardware UART for STM32 communication - no ready check needed */
    Serial1.begin(UART_BAUD, SERIAL_8N1, RX_PIN, TX_PIN);
    /* Flush any bootloader garbage sitting in RX buffer */
    delay(DELAY_100MS);
    while (Serial1.available())
    {
        Serial1.read();
    }

    Serial.printf("UART to USB is initialized\r\n");
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
        /* Attempt to connect and store credentials on success */
        wifi_connect_and_store();
        /* Print this info */
        Serial.printf ("DONE, network connected and stored\r\n");
        /* Signal readiness to STM32 */
        digitalWrite(READY_PIN, LOW);
    }
}