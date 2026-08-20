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
 * account.
 *
 * Idempotent by design: if a row for this device_number already exists
 * (e.g. the same physical board gets erased/re-flashed and re-registered
 * during dev/rework via cpp_production), that old row - whatever user_id
 * or last_reading it had - is deleted and replaced with a completely
 * fresh unclaimed row, rather than failing with "already exists". A
 * re-flashed board is meant to start clean. */

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

$db_error = false;
try
{
    $mysqli = new mysqli($DB_HOST, $DB_USER, $DB_PASS, $DB_NAME);
    /* created_at is a TIMESTAMP column - see the same note in claim_device.php */
    $mysqli->query("SET time_zone = '+00:00'");

    /* device_number is UNIQUE - delete any existing row for it first (a */
    /* re-flashed/re-registered board should start completely clean, not */
    /* keep a previous claim/last_reading around or fail because a row */
    /* already exists), then insert a fresh unclaimed one. Explicit */
    /* PHP-generated UTC value instead of NOW() - see the same note in */
    /* claim_device.php */
    $delete = $mysqli->prepare('DELETE FROM devices WHERE device_number = ?');
    $delete->bind_param('s', $mac_binary);
    $delete->execute();
    $delete->close();

    $created_at_utc = gmdate('Y-m-d H:i:s');
    $insert = $mysqli->prepare(
        'INSERT INTO devices (user_id, device_number, created_at) VALUES (0, ?, ?)'
    );
    $insert->bind_param('ss', $mac_binary, $created_at_utc);
    $insert->execute();
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

http_response_code(200);
echo "OK";
?>
