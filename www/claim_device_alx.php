<?php
/* claim_device_alx.php - pre-registration endpoint for brand new devices,
 * meant to be called by hand (paste the URL into a browser) rather than by
 * the firmware. claim_device.php can only ever claim a device that already
 * has a row in the devices table, and normally that row is created by the
 * first measurement upload - but a fresh device has never uploaded
 * anything yet, so its very first BLE setup (which calls CLAIM_URL) has
 * nothing to claim and fails.
 *
 * To bring a new device online: read its id off the native USB port (see
 * print_device_id_over_usb() in wifi_bt.ino - "0x" + a fixed 5-byte sync
 * marker + the 9-byte device id, e.g.
 * "0xc0bec0b0b00600040000005a8035"), then hit this URL with it as the "id"
 * query param. That registers the device with user_id = 0 (unclaimed) -
 * BLE setup against claim_device.php can then claim it for a dashboard
 * account. */

require_once __DIR__ . '/config.php';

header('Content-Type: text/plain; charset=Windows-1252');
header('Cache-Control: no-cache, no-store, must-revalidate');
header('Pragma: no-cache');
header('Expires: 0');

/* "id" is the exact string wifi_bt.ino's print_device_id_over_usb() prints
 * over the native USB port: an optional "0x" prefix, followed by the fixed
 * 5-byte sync marker (C0BEC0B0B0), followed by the 18-hex-char device id. */
$id_hex = strtoupper($_GET['id'] ?? '');
if (strpos($id_hex, '0X') === 0)
{
    $id_hex = substr($id_hex, 2);
}

/* Verifying the marker here catches a mis-copied/truncated id early */
/* instead of silently registering garbage - see g_ui8_usb_id_magic in */
/* wifi_bt.ino */
if (!preg_match('/^C0BEC0B0B0([0-9A-F]{18})$/', $id_hex, $matches))
{
    http_response_code(400);
    echo "ERROR: Invalid id format";
    exit;
}

$mac_hex = $matches[1];

$mac_binary = hex2bin($mac_hex);

$already_exists = false;
$db_error = false;
try
{
    $mysqli = new mysqli($DB_HOST, $DB_USER, $DB_PASS, $DB_NAME);
    /* created_at is a TIMESTAMP column - see the same note in claim_device.php */
    $mysqli->query("SET time_zone = '+00:00'");

    /* device_number is UNIQUE, so INSERT IGNORE atomically does nothing if */
    /* a row for this device already exists - affected_rows then */
    /* distinguishes "just inserted" (1) from "already there" (0) with no */
    /* separate SELECT and no race condition between the check and the */
    /* insert. Explicit PHP-generated UTC value instead of NOW() - see the */
    /* same note in claim_device.php */
    $created_at_utc = gmdate('Y-m-d H:i:s');
    $insert = $mysqli->prepare(
        'INSERT IGNORE INTO devices (user_id, device_number, created_at) VALUES (0, ?, ?)'
    );
    $insert->bind_param('ss', $mac_binary, $created_at_utc);
    $insert->execute();
    $already_exists = ($insert->affected_rows === 0);
    $insert->close();

    $mysqli->close();
}
catch (Throwable $e)
{
    $db_error = true;
    error_log('claim_device_alx.php: failed to register device for MAC ' . $mac_hex . ': ' . $e->getMessage());
}

if ($db_error)
{
    http_response_code(500);
    echo "ERROR: Could not register device";
    exit;
}

if ($already_exists)
{
    http_response_code(409);
    echo "ERROR: Device already exists";
    exit;
}

http_response_code(200);
echo "OK";
?>
