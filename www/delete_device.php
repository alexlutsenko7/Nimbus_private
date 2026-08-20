<?php
/* delete_device.php - "delete sensor": unlinks one of the calling account's */
/* devices by setting its devices.user_id back to 0, the same release path  */
/* claim_device.php already uses when a device is reclaimed by a different  */
/* account. Nothing is actually deleted from the devices table - the row    */
/* (and any last_reading) just goes back to unclaimed, exactly like it      */
/* would sit before ever being claimed. Accounts can own multiple devices,  */
/* so the specific device is identified by its device_number (same 18-hex- */
/* char MAC format claim_device.php validates) - only that one row is       */
/* touched, any other devices the account owns are left alone. Called via   */
/* fetch() from the dashboard's trash icon (index.php), and natively from   */
/* the Android app, which forwards the same nimbus_user cookie a WebView    */
/* request would send */

require_once __DIR__ . '/config.php';

header('Cache-Control: no-cache, no-store, must-revalidate');
header('Pragma: no-cache');
header('Expires: 0');

if ($_SERVER['REQUEST_METHOD'] !== 'POST')
{
    http_response_code(405);
    echo "ERROR: Method not allowed";
    exit;
}

$mac_hex = strtoupper($_POST['device_number'] ?? '');
if (!preg_match('/^[0-9A-F]{18}$/', $mac_hex))
{
    http_response_code(400);
    echo "ERROR: Invalid format";
    exit;
}
$mac_binary = hex2bin($mac_hex);

$deleted = false;
try
{
    $mysqli = new mysqli($DB_HOST, $DB_USER, $DB_PASS, $DB_NAME);

    /* same cookie check index.php uses to resolve the logged-in user - */
    /* "username|password_hash", re-verified against the current DB hash */
    $current_user_id = null;
    if (isset($_COOKIE[$COOKIE_NAME]))
    {
        $cookie_parts = explode('|', $_COOKIE[$COOKIE_NAME], 2);
        if (count($cookie_parts) === 2)
        {
            list($cookie_username, $cookie_hash) = $cookie_parts;
            $stmt = $mysqli->prepare('SELECT id, password_hash FROM users WHERE username = ? LIMIT 1');
            $stmt->bind_param('s', $cookie_username);
            $stmt->execute();
            $stmt->store_result();
            $stmt->bind_result($db_id, $db_hash);
            $found = ($stmt->num_rows === 1) && $stmt->fetch();
            $stmt->close();

            if ($found && hash_equals($db_hash, $cookie_hash))
            {
                $current_user_id = $db_id;
            }
        }
    }

    if ($current_user_id !== null)
    {
        $update = $mysqli->prepare('UPDATE devices SET user_id = 0 WHERE user_id = ? AND device_number = ?');
        $update->bind_param('is', $current_user_id, $mac_binary);
        $update->execute();
        $affected = $update->affected_rows;
        $update->close();
        /* affected_rows is 0 if this device_number didn't belong to the */
        /* calling account (or didn't exist) - don't report success then */
        $deleted = ($affected > 0);
    }

    $mysqli->close();
}
catch (Throwable $e)
{
    error_log('delete_device.php: failed to delete device: ' . $e->getMessage());
}

if (!$deleted)
{
    http_response_code(403);
    echo "ERROR: Not logged in, or not your device";
    exit;
}

http_response_code(200);
echo "OK";
?>
