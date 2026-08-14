<?php
/* update_alx.php - receives weather data from ESP32 and stores it in the */
/* devices table (devices.last_reading) */

require_once __DIR__ . '/config.php';

/* prevent any caching of this response */
header('Cache-Control: no-cache, no-store, must-revalidate');
header('Pragma: no-cache');
header('Expires: 0');

/* parse the query string: T2334H6722P98823B3294S-65MD83BDA75F32C011 */
/* S is the WiFi RSSI in dBm (negative) at upload time */
/* M is the 9-byte device id (6 bytes WiFi MAC + 3 bytes BT MAC), hex-encoded */
/* - the same id update_alx_setup.php registered in the devices table */
$raw = $_SERVER['QUERY_STRING'];
$match_count = preg_match('/T(-?\d+)H(\d+)P(\d+)B(\d+)S(-?\d+)M([0-9A-Fa-f]{18})/', $raw, $matches);

/* validate that all six fields were found */
if ($match_count !== 1)
{
    http_response_code(400);
    echo "ERROR: Invalid format";
    exit;
}

/* extract the values */
$temperature = intval($matches[1]);
$humidity    = intval($matches[2]);
$pressure    = intval($matches[3]);
$battery     = intval($matches[4]);
$wifi_signal = intval($matches[5]);
$mac_hex     = strtoupper($matches[6]);
$mac_binary  = hex2bin($mac_hex);

/* build the reading record */
$record = array(
    'temperature' => $temperature,
    'humidity'    => $humidity,
    'pressure'   => $pressure,
    'battery'    => $battery,
    'wifi_signal' => $wifi_signal,
    'timestamp'  => gmdate('Y-m-d H:i:s'),
    'ip'         => $_SERVER['REMOTE_ADDR']
);
$record_json = json_encode($record);

/* only accept data from a device that was registered via */
/* update_alx_setup.php - anything else is rejected. A DB error is */
/* treated the same as "not found" so an outage can't be used to bypass */
/* the check */
$saved = false;
try
{
    $mysqli = new mysqli($DB_HOST, $DB_USER, $DB_PASS, $DB_NAME);

    $stmt = $mysqli->prepare('SELECT 1 FROM devices WHERE device_number = ? LIMIT 1');
    $stmt->bind_param('s', $mac_binary);
    $stmt->execute();
    $stmt->store_result();
    $device_known = $stmt->num_rows > 0;
    $stmt->close();

    if ($device_known)
    {
        $update = $mysqli->prepare('UPDATE devices SET last_reading = ? WHERE device_number = ?');
        $update->bind_param('ss', $record_json, $mac_binary);
        $update->execute();
        $update->close();
        $saved = true;
    }

    $mysqli->close();
}
catch (Throwable $e)
{
    error_log('update_alx.php: failed to save reading for MAC ' . $mac_hex . ': ' . $e->getMessage());
}

if (!$saved)
{
    http_response_code(403);
    echo "ERROR: Unknown device";
    exit;
}

http_response_code(200);
echo "OK";
?>
