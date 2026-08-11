<?php
/* update_alx_setup.php - receives weather data from ESP32, stores it like */
/* update_alx.php, and additionally registers the device in MySQL         */

require_once __DIR__ . '/config.php';

/* prevent any caching of this response */
header('Cache-Control: no-cache, no-store, must-revalidate');
header('Pragma: no-cache');
header('Expires: 0');

/* parse the query string: T2334H6722P98823B3294MD83BDA75F32C011 */
/* M is the 9-byte device id (6 bytes WiFi MAC + 3 bytes BT MAC), hex-encoded */
$raw = $_SERVER['QUERY_STRING'];
$match_count = preg_match('/T(-?\d+)H(\d+)P(\d+)B(\d+)M([0-9A-Fa-f]{18})/', $raw, $matches);

/* validate that all five fields were found */
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
$mac_hex     = strtoupper($matches[5]);

/* build the data record */
$record = array(
    'temperature' => $temperature,
    'humidity'    => $humidity,
    'pressure'   => $pressure,
    'battery'    => $battery,
    'timestamp'  => gmdate('Y-m-d H:i:s'),
    'ip'         => $_SERVER['REMOTE_ADDR']
);

/* respond to the ESP32 */
http_response_code(200);
echo "OK";

/* register the device in MySQL if it is not already known - device_number */
/* is 9 raw bytes (6 bytes WiFi MAC + 3 bytes BT MAC) matching the */
/* devices.device_number BINARY(9) column in install_alx.php */
/* PHP 8.1+ defaults mysqli to throwing on error rather than returning */
/* false, so this is wrapped in try/catch and logged instead of silently */
/* dying after the "OK" response above has already been sent */
$mac_binary = hex2bin($mac_hex);

/* TEMPORARY: this whole block echoes diagnostics into the response for */
/* on-screen debugging - strip the echo calls once this is confirmed working */
try
{
    $mysqli = new mysqli($DB_HOST, $DB_USER, $DB_PASS, $DB_NAME);
    if ($mysqli->connect_errno)
    {
        echo "\nDEBUG connect failed (" . $mysqli->connect_errno . "): " . $mysqli->connect_error;
    }
    else
    {
        /* device_number is UNIQUE, so INSERT IGNORE atomically does nothing */
        /* if a record for this device already exists - no separate SELECT */
        /* needed, and no race condition between the check and the insert */
        $stmt = $mysqli->prepare(
            'INSERT IGNORE INTO devices (user_id, device_number, created_at) VALUES (0, ?, ?)'
        );
        if ($stmt === false)
        {
            echo "\nDEBUG prepare failed: " . $mysqli->error;
        }
        else
        {
            $stmt->bind_param('ss', $mac_binary, $record['timestamp']);
            if (!$stmt->execute())
            {
                echo "\nDEBUG execute failed: " . $stmt->error;
            }
            $stmt->close();
        }
        $mysqli->close();
    }
}
catch (Throwable $e)
{
    error_log('update_alx_setup.php: device registration failed for MAC '
        . $mac_hex . ': ' . $e->getMessage());
}
?>
