<?php
/* claim_device.php - links a device to a dashboard account. Called once */
/* by the ESP32 right after WiFi connects during BLE setup, if the phone */
/* handed over a logged-in user's credential */

require_once __DIR__ . '/config.php';

/* prevent any caching of this response */
header('Cache-Control: no-cache, no-store, must-revalidate');
header('Pragma: no-cache');
header('Expires: 0');

$mac_hex  = strtoupper($_GET['mac'] ?? '');
$username = $_GET['username'] ?? '';
$hash     = $_GET['hash'] ?? '';

if (!preg_match('/^[0-9A-F]{18}$/', $mac_hex) || $username === '' || $hash === '')
{
    http_response_code(400);
    echo "ERROR: Invalid format";
    exit;
}

$mac_binary = hex2bin($mac_hex);

/* only claim the device if the username/password_hash actually match a */
/* real account - this is the same hash comparison index.php uses to */
/* validate the dashboard's login cookie */
$claimed = false;
try
{
    $mysqli = new mysqli($DB_HOST, $DB_USER, $DB_PASS, $DB_NAME);

    $stmt = $mysqli->prepare('SELECT id, password_hash FROM users WHERE username = ? LIMIT 1');
    $stmt->bind_param('s', $username);
    $stmt->execute();
    $stmt->store_result();
    $stmt->bind_result($user_id, $db_hash);
    $found = ($stmt->num_rows === 1) && $stmt->fetch();
    $stmt->close();

    if ($found && hash_equals($db_hash, $hash))
    {
        $update = $mysqli->prepare('UPDATE devices SET user_id = ? WHERE device_number = ?');
        $update->bind_param('is', $user_id, $mac_binary);
        $update->execute();
        $update->close();

        /* Don't just trust that the UPDATE above took effect - re-read the */
        /* row and confirm it now belongs to this user before reporting     */
        /* success. Without this, a request that never actually reaches    */
        /* this script's DB call (e.g. swallowed by the host's WAF, which  */
        /* has previously been seen returning a bare HTTP 200 without      */
        /* running the PHP behind it) would still echo "OK", leaving a     */
        /* device silently owned by its previous claimant. */
        $verify = $mysqli->prepare('SELECT user_id FROM devices WHERE device_number = ? LIMIT 1');
        $verify->bind_param('s', $mac_binary);
        $verify->execute();
        $verify->store_result();
        $verify->bind_result($current_owner_id);
        $verify_found = ($verify->num_rows === 1) && $verify->fetch();
        $verify->close();

        $claimed = $verify_found && ((int)$current_owner_id === (int)$user_id);
    }

    $mysqli->close();
}
catch (Throwable $e)
{
    error_log('claim_device.php: failed to claim device for MAC ' . $mac_hex . ': ' . $e->getMessage());
}

if (!$claimed)
{
    http_response_code(403);
    echo "ERROR: Could not claim device";
    exit;
}

http_response_code(200);
echo "OK";
?>
