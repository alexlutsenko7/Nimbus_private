<?php
/* data.php - receives weather data from ESP32 and stores to file */

/* prevent any caching of this response */
header('Cache-Control: no-cache, no-store, must-revalidate');
header('Pragma: no-cache');
header('Expires: 0');

/* data.php - receives weather data from ESP32 and stores to file */

/* output file path - same directory as this script */
$data_file = __DIR__ . '/data.json';

/* parse the query string: T2334H6722P98823B3294 */
$raw = $_SERVER['QUERY_STRING'];
$match_count = preg_match('/T(-?\d+)H(\d+)P(\d+)B(\d+)/', $raw, $matches);

/* validate that all four fields were found */
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

/* build the data record */
$record = array(
    'temperature' => $temperature,
    'humidity'    => $humidity,
    'pressure'   => $pressure,
    'battery'    => $battery,
    'timestamp'  => gmdate('Y-m-d H:i:s'),
    'ip'         => $_SERVER['REMOTE_ADDR']
);

/* write to file - overwrites with latest reading */

/* Write to a temp file first, then atomically rename over the real file */
/* rename() on the same filesystem is atomic - readers see either the old */
/* file or the new file, never a partial or locked one */
$tmp_file = $data_file . '.tmp';
$result = file_put_contents($tmp_file, json_encode($record, JSON_PRETTY_PRINT), LOCK_EX);

if ($result !== false)
{
    rename($tmp_file, $data_file);
}

/* respond to the ESP32 */
if ($result !== false)
{
    http_response_code(200);
    echo "OK";
}
else
{
    http_response_code(500);
    echo "ERROR: Write failed";
}

/* append to CSV log file */
$log_file = __DIR__ . '/log_' . date('Y') . '.csv';

/* convert timestamp to Toronto local time */
$utc_time = new DateTime($record['timestamp'], new DateTimeZone('UTC'));
$utc_time->setTimezone(new DateTimeZone('America/Toronto'));
$local_time = $utc_time->format('Y-m-d H:i:s');

/* if file does not exist, create it with a header row */
if (!file_exists($log_file))
{
    file_put_contents($log_file, "Timestamp,Temperature,Pressure,Humidity,Battery,IP\n", LOCK_EX);
}

/* build the CSV line */
/* build the CSV line with human-readable values */
$csv_line = "=\"". $local_time. "\"" . ','
          . number_format($temperature / 100, 2) . ','
          . number_format($pressure / 100, 2) . ','
          . number_format($humidity / 1000, 2) . ','
          . number_format($battery / 1000, 3) . ','
          . $_SERVER['REMOTE_ADDR'] . "\n";

/* append to the log file */
file_put_contents($log_file, $csv_line, FILE_APPEND | LOCK_EX);
?>