<?php
/* confirm.php - lands here from the link in the registration confirmation
 * email. Activates the account the token belongs to, then bounces to the
 * login form. */

require_once __DIR__ . '/config.php';

header('Cache-Control: no-cache, no-store, must-revalidate');
header('Pragma: no-cache');
header('Expires: 0');

$token = $_GET['token'] ?? '';

if (!preg_match('/^[0-9a-f]{64}$/', $token))
{
    http_response_code(400);
    echo 'Invalid confirmation link.';
    exit;
}

try
{
    $mysqli = new mysqli($DB_HOST, $DB_USER, $DB_PASS, $DB_NAME);

    $stmt = $mysqli->prepare('SELECT id FROM users WHERE confirm_token = ? AND confirmed = 0 LIMIT 1');
    $stmt->bind_param('s', $token);
    $stmt->execute();
    $stmt->store_result();
    $stmt->bind_result($user_id);
    $found = ($stmt->num_rows === 1) && $stmt->fetch();
    $stmt->close();

    if ($found)
    {
        $update = $mysqli->prepare('UPDATE users SET confirmed = 1, confirm_token = NULL WHERE id = ?');
        $update->bind_param('i', $user_id);
        $update->execute();
        $update->close();
    }

    $mysqli->close();
}
catch (Throwable $e)
{
    error_log('confirm.php: confirmation failed: ' . $e->getMessage());
    $found = false;
}

if ($found)
{
    header('Location: index.php?confirmed=1');
    exit;
}

http_response_code(404);
echo 'This confirmation link is invalid or has already been used.';
?>
