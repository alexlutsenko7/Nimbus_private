<?php
/* claim_device_alx.php - TEMPORARY registration endpoint for brand new
 * devices. claim_device.php can only ever claim a device that already has
 * a row in the devices table, and normally that row is created by the
 * first measurement upload - but a fresh device has never uploaded
 * anything yet, so its very first BLE setup (which calls CLAIM_URL) has
 * nothing to claim and fails.
 *
 * To bring a new device online: temporarily point CLAIM_URL (in
 * wifi_bt.ino) at this file, run BLE setup once - this registers the
 * device with user_id = 0 (unclaimed) - then switch CLAIM_URL back to
 * claim_device.php and run BLE setup again to actually claim it for a
 * dashboard account. */

require_once __DIR__ . '/config.php';

header('Cache-Control: no-cache, no-store, must-revalidate');
header('Pragma: no-cache');
header('Expires: 0');

$mac_hex = strtoupper($_GET['mac'] ?? '');

if (!preg_match('/^[0-9A-F]{18}$/', $mac_hex))
{
    http_response_code(400);
    echo "ERROR: Invalid format";
    exit;
}

$mac_binary = hex2bin($mac_hex);

$registered = false;
try
{
    $mysqli = new mysqli($DB_HOST, $DB_USER, $DB_PASS, $DB_NAME);
    /* created_at is a TIMESTAMP column - see the same note in claim_device.php */
    $mysqli->query("SET time_zone = '+00:00'");

    /* device_number is UNIQUE, so INSERT IGNORE atomically does nothing if */
    /* a row for this device already exists - no separate SELECT needed, */
    /* and no race condition between the check and the insert */
    /* Explicit PHP-generated UTC value instead of NOW() - see the same */
    /* note in claim_device.php */
    $created_at_utc = gmdate('Y-m-d H:i:s');
    $insert = $mysqli->prepare(
        'INSERT IGNORE INTO devices (user_id, device_number, created_at) VALUES (0, ?, ?)'
    );
    $insert->bind_param('ss', $mac_binary, $created_at_utc);
    $insert->execute();
    $insert->close();

    /* confirm a row now exists for this MAC, whether it was just inserted */
    /* or already there from an earlier attempt */
    $verify = $mysqli->prepare('SELECT id FROM devices WHERE device_number = ? LIMIT 1');
    $verify->bind_param('s', $mac_binary);
    $verify->execute();
    $verify->store_result();
    $registered = ($verify->num_rows === 1);
    $verify->close();

    $mysqli->close();
}
catch (Throwable $e)
{
    error_log('claim_device_alx.php: failed to register device for MAC ' . $mac_hex . ': ' . $e->getMessage());
}

if (!$registered)
{
    http_response_code(500);
    echo "ERROR: Could not register device";
    exit;
}

http_response_code(200);
echo "OK";
?>
