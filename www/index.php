<?php
require_once __DIR__ . '/config.php';
require_once __DIR__ . '/mailer.php';

/* PHP session is used only to hold the current CAPTCHA code between the */
/* time captcha.php renders it and the register form being submitted - */
/* unrelated to the nimbus_user auth cookie */
session_start();

header('Cache-Control: no-cache, no-store, must-revalidate');
header('Pragma: no-cache');
header('Expires: 0');

/* ---- logout: clear the cookie and bounce back to the login form ---- */
if (isset($_GET['logout']))
{
    setcookie($COOKIE_NAME, '', [
        'expires'  => time() - 3600,
        'path'     => '/',
        'secure'   => true,
        'httponly' => true,
        'samesite' => 'Lax'
    ]);
    header('Location: index.php');
    exit;
}

/* --------------------------------------------------------------------- */
/* Auth: no session table/column is used. The cookie holds               */
/* "username|password_hash" - the exact bcrypt hash stored in the users   */
/* table, not the plaintext password. Every request re-checks that hash   */
/* against the current DB value, so changing the account password         */
/* immediately invalidates any cookie issued before the change.           */
/* --------------------------------------------------------------------- */
$auth_error = '';
$registered = isset($_GET['registered']);
$confirmed_msg = isset($_GET['confirmed']);
$password_reset_msg = isset($_GET['password_reset']);
$view_param = $_POST['action'] ?? $_GET['view'] ?? '';
if ($view_param === 'register')
{
    $view = 'register';
}
elseif ($view_param === 'forgot_password' || $view_param === 'request_reset')
{
    $view = 'forgot_password';
}
else
{
    $view = 'login';
}

$mysqli = null;
try
{
    $mysqli = new mysqli($DB_HOST, $DB_USER, $DB_PASS, $DB_NAME);
    /* devices.created_at is a TIMESTAMP column - MySQL converts it through */
    /* the session's time_zone on every read (not just NOW()/literal writes), */
    /* so without pinning this, SELECT hands back a value already shifted by */
    /* the DB server's default zone, which the UTC->America/Toronto */
    /* conversion below would then shift again */
    $mysqli->query("SET time_zone = '+00:00'");
}
catch (Throwable $e)
{
    error_log('index.php: DB connection failed: ' . $e->getMessage());
}

if ($mysqli === null && $_SERVER['REQUEST_METHOD'] === 'POST')
{
    $auth_error = 'Service temporarily unavailable. Please try again later.';
}

/* ---- account creation ---- */
if ($mysqli !== null && $_SERVER['REQUEST_METHOD'] === 'POST' && ($_POST['action'] ?? '') === 'register')
{
    $username = trim($_POST['username'] ?? '');
    $password = $_POST['password'] ?? '';
    $email    = trim($_POST['email'] ?? '');
    $captcha  = trim($_POST['captcha'] ?? '');

    /* the code is single-use regardless of outcome - a fresh image is */
    /* generated on every page load anyway */
    $expected_captcha = $_SESSION['captcha_code'] ?? '';
    unset($_SESSION['captcha_code']);

    if ($captcha === '' || strcasecmp($captcha, $expected_captcha) !== 0)
    {
        $auth_error = 'Verification code did not match.';
    }
    elseif (!preg_match('/^[A-Za-z0-9_]{3,32}$/', $username))
    {
        $auth_error = 'Username must be 3-32 characters: letters, numbers, underscore only.';
    }
    elseif (strlen($password) < 8 || !preg_match('/[A-Z]/', $password))
    {
        $auth_error = 'Password must be at least 8 characters and include at least one capital letter.';
    }
    elseif (!filter_var($email, FILTER_VALIDATE_EMAIL))
    {
        $auth_error = 'Please enter a valid email address.';
    }
    else
    {
        try
        {
            $stmt = $mysqli->prepare('SELECT id, confirmed FROM users WHERE username = ? LIMIT 1');
            $stmt->bind_param('s', $username);
            $stmt->execute();
            $stmt->store_result();
            $stmt->bind_result($existing_id, $existing_confirmed);
            $existing_found = ($stmt->num_rows === 1) && $stmt->fetch();
            $stmt->close();

            if ($existing_found && $existing_confirmed)
            {
                $auth_error = 'That username is already taken.';
            }
            else
            {
                /* Either a brand new username, or a previous registration */
                /* that was never confirmed - build the new credentials and */
                /* email before touching the DB, so a failed send never */
                /* leaves a row (new or overwritten) that can't be confirmed */
                $password_hash = password_hash($password, PASSWORD_DEFAULT);
                $confirm_token = bin2hex(random_bytes(32));
                $confirm_link = $SITE_URL . 'confirm.php?token=' . $confirm_token;
                $mail_sent = send_confirmation_email(
                    $SMTP_HOST, $SMTP_PORT, $SMTP_USER, $SMTP_PASS, $SMTP_FROM, $SMTP_FROM_NAME,
                    $email, $username, $confirm_link
                );

                if (!$mail_sent)
                {
                    $auth_error = 'Could not send the confirmation email. Please try again later.';
                }
                elseif ($existing_found)
                {
                    /* Replace the old unconfirmed registration in place - */
                    /* the "confirmed = 0" guard re-checks at write time in */
                    /* case the old link got confirmed between the SELECT */
                    /* above and this UPDATE */
                    $update = $mysqli->prepare(
                        'UPDATE users SET password_hash = ?, email = ?, confirm_token = ? WHERE id = ? AND confirmed = 0'
                    );
                    $update->bind_param('sssi', $password_hash, $email, $confirm_token, $existing_id);
                    $update->execute();
                    $update->close();

                    header('Location: index.php?registered=1');
                    exit;
                }
                else
                {
                    $insert = $mysqli->prepare(
                        'INSERT INTO users (username, password_hash, email, confirmed, confirm_token) VALUES (?, ?, ?, 0, ?)'
                    );
                    $insert->bind_param('ssss', $username, $password_hash, $email, $confirm_token);
                    $insert->execute();
                    $insert->close();

                    header('Location: index.php?registered=1');
                    exit;
                }
            }
        }
        catch (Throwable $e)
        {
            error_log('index.php: registration failed: ' . $e->getMessage());
            $auth_error = 'Something went wrong. Please try again later.';
        }
    }
}

/* ---- login ---- */
if ($mysqli !== null && $_SERVER['REQUEST_METHOD'] === 'POST' && ($_POST['action'] ?? '') === 'login')
{
    $username = trim($_POST['username'] ?? '');
    $password = $_POST['password'] ?? '';

    try
    {
        $stmt = $mysqli->prepare('SELECT password_hash, confirmed FROM users WHERE username = ? LIMIT 1');
        $stmt->bind_param('s', $username);
        $stmt->execute();
        $stmt->store_result();
        $stmt->bind_result($db_hash, $db_confirmed);
        $found = ($stmt->num_rows === 1) && $stmt->fetch();
        $stmt->close();

        if ($found && password_verify($password, $db_hash) && !$db_confirmed)
        {
            $auth_error = 'Please confirm your email address before logging in - check your inbox for the confirmation link.';
        }
        elseif ($found && password_verify($password, $db_hash))
        {
            setcookie($COOKIE_NAME, $username . '|' . $db_hash, [
                'expires'  => $COOKIE_EXPIRE,
                'path'     => '/',
                'secure'   => true,
                'httponly' => true,
                'samesite' => 'Lax'
            ]);
            header('Location: index.php');
            exit;
        }
        else
        {
            $auth_error = 'Invalid username or password.';
        }
    }
    catch (Throwable $e)
    {
        error_log('index.php: login failed: ' . $e->getMessage());
        $auth_error = 'Something went wrong. Please try again later.';
    }
}

/* ---- forgot password: email a reset link ---- */
$reset_error = '';

if ($mysqli !== null && $_SERVER['REQUEST_METHOD'] === 'POST' && ($_POST['action'] ?? '') === 'request_reset')
{
    $reset_email = trim($_POST['email'] ?? '');

    if (!filter_var($reset_email, FILTER_VALIDATE_EMAIL))
    {
        $reset_error = 'Please enter a valid email address.';
    }
    else
    {
        try
        {
            /* Only confirmed accounts get a reset link - an unconfirmed */
            /* registration should be redone from scratch (same username) */
            /* rather than "recovered" */
            $stmt = $mysqli->prepare('SELECT id, username FROM users WHERE email = ? AND confirmed = 1 LIMIT 1');
            $stmt->bind_param('s', $reset_email);
            $stmt->execute();
            $stmt->store_result();
            $stmt->bind_result($reset_user_id, $reset_username);
            $reset_found = ($stmt->num_rows === 1) && $stmt->fetch();
            $stmt->close();

            if ($reset_found)
            {
                /* Only a pending reset_token/reset_expires pair is written */
                /* here - the account's password_hash is untouched until the */
                /* link is actually opened and a new password submitted on */
                /* reset_password.php */
                $reset_token = bin2hex(random_bytes(32));
                $reset_expires = date('Y-m-d H:i:s', time() + 3600);

                $update = $mysqli->prepare('UPDATE users SET reset_token = ?, reset_expires = ? WHERE id = ?');
                $update->bind_param('ssi', $reset_token, $reset_expires, $reset_user_id);
                $update->execute();
                $update->close();

                $reset_link = $SITE_URL . 'reset_password.php?token=' . $reset_token;
                send_password_reset_email(
                    $SMTP_HOST, $SMTP_PORT, $SMTP_USER, $SMTP_PASS, $SMTP_FROM, $SMTP_FROM_NAME,
                    $reset_email, $reset_username, $reset_link
                );
            }
        }
        catch (Throwable $e)
        {
            error_log('index.php: password reset request failed: ' . $e->getMessage());
        }

        /* Same redirect regardless of whether the email matched an account */
        /* or the mail send succeeded - otherwise the response would leak */
        /* which addresses are registered */
        header('Location: index.php?view=forgot_password&sent=1');
        exit;
    }
}

/* ---- check whether the current cookie matches a real user ---- */
$current_user = null;
$current_user_id = null;
if ($mysqli !== null && isset($_COOKIE[$COOKIE_NAME]))
{
    $cookie_parts = explode('|', $_COOKIE[$COOKIE_NAME], 2);
    if (count($cookie_parts) === 2)
    {
        list($cookie_username, $cookie_hash) = $cookie_parts;
        try
        {
            $stmt = $mysqli->prepare('SELECT id, username, password_hash FROM users WHERE username = ? LIMIT 1');
            $stmt->bind_param('s', $cookie_username);
            $stmt->execute();
            $stmt->store_result();
            $stmt->bind_result($db_id, $db_username, $db_hash);
            $found = ($stmt->num_rows === 1) && $stmt->fetch();
            $stmt->close();

            if ($found && hash_equals($db_hash, $cookie_hash))
            {
                $current_user = $db_username;
                $current_user_id = $db_id;
            }
        }
        catch (Throwable $e)
        {
            error_log('index.php: session check failed: ' . $e->getMessage());
        }
    }
}

/* ---- change password (must already be logged in) ---- */
$change_pw_error = '';
$change_pw_success = isset($_GET['password_changed']);

if ($current_user !== null && $mysqli !== null && $_SERVER['REQUEST_METHOD'] === 'POST' && ($_POST['action'] ?? '') === 'change_password')
{
    $current_password = $_POST['current_password'] ?? '';
    $new_password      = $_POST['new_password'] ?? '';
    $confirm_password  = $_POST['confirm_password'] ?? '';

    try
    {
        $stmt = $mysqli->prepare('SELECT password_hash FROM users WHERE id = ? LIMIT 1');
        $stmt->bind_param('i', $current_user_id);
        $stmt->execute();
        $stmt->store_result();
        $stmt->bind_result($db_hash);
        $found = ($stmt->num_rows === 1) && $stmt->fetch();
        $stmt->close();

        if (!$found || !password_verify($current_password, $db_hash))
        {
            $change_pw_error = 'Current password is incorrect.';
        }
        elseif (strlen($new_password) < 8 || !preg_match('/[A-Z]/', $new_password))
        {
            $change_pw_error = 'New password must be at least 8 characters and include at least one capital letter.';
        }
        elseif ($new_password !== $confirm_password)
        {
            $change_pw_error = 'New passwords do not match.';
        }
        else
        {
            $new_hash = password_hash($new_password, PASSWORD_DEFAULT);
            $update = $mysqli->prepare('UPDATE users SET password_hash = ? WHERE id = ?');
            $update->bind_param('si', $new_hash, $current_user_id);
            $update->execute();
            $update->close();

            /* the auth cookie embeds the password hash - it must be */
            /* re-issued with the new hash or the user gets logged out */
            setcookie($COOKIE_NAME, $current_user . '|' . $new_hash, [
                'expires'  => $COOKIE_EXPIRE,
                'path'     => '/',
                'secure'   => true,
                'httponly' => true,
                'samesite' => 'Lax'
            ]);

            header('Location: index.php?password_changed=1');
            exit;
        }
    }
    catch (Throwable $e)
    {
        error_log('index.php: password change failed: ' . $e->getMessage());
        $change_pw_error = 'Something went wrong. Please try again later.';
    }
}

/* show the change-password form instead of the dashboard, either because */
/* the user navigated to it or because a submission above just failed */
if ($current_user !== null && (($_GET['view'] ?? '') === 'change_password' || $change_pw_error !== ''))
{
    ?>
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<link rel="icon" href="icon.png" type="image/png">
<title>Nimbus Weather Station</title>
<style>
    :root {
        --bg: #0e1419;
        --panel: #161e26;
        --panel-edge: #232f3a;
        --ink: #e6edf3;
        --ink-dim: #7d8b99;
        --accent: #4cc4d6;
        --warn: #e0a458;
        --sans: -apple-system, 'Segoe UI', Roboto, sans-serif;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
        background: var(--bg);
        color: var(--ink);
        font-family: var(--sans);
        min-height: 100vh;
        display: flex;
        align-items: center;
        justify-content: center;
        padding: 1rem;
    }
    .auth-card {
        width: 100%;
        max-width: 360px;
        background: var(--panel);
        border: 1px solid var(--panel-edge);
        border-radius: 10px;
        padding: 1.8rem;
    }
    .auth-card h1 {
        font-size: 1.1rem;
        font-weight: 600;
        margin-bottom: 1.4rem;
    }
    .auth-card label {
        display: block;
        font-size: 0.78rem;
        color: var(--ink-dim);
        margin-bottom: 0.3rem;
    }
    .auth-card input {
        width: 100%;
        background: var(--bg);
        border: 1px solid var(--panel-edge);
        color: var(--ink);
        border-radius: 6px;
        padding: 0.55rem 0.65rem;
        margin-bottom: 1rem;
        font-size: 0.9rem;
    }
    .pw-rules {
        font-size: 0.74rem;
        color: var(--ink-dim);
        margin-top: -0.7rem;
        margin-bottom: 1rem;
    }
    .auth-card button {
        width: 100%;
        background: var(--accent);
        color: #0e1419;
        border: none;
        border-radius: 6px;
        padding: 0.6rem;
        font-size: 0.9rem;
        font-weight: 600;
        cursor: pointer;
    }
    .auth-card .switch {
        text-align: center;
        margin-top: 1rem;
        font-size: 0.82rem;
        color: var(--ink-dim);
    }
    .auth-card .switch a {
        color: var(--accent);
        text-decoration: none;
    }
    .auth-msg {
        font-size: 0.82rem;
        margin-bottom: 1rem;
    }
    .auth-msg.error { color: var(--warn); }
    .auth-msg.ok { color: var(--accent); }
</style>
</head>
<body>
    <div class="auth-card">
        <h1>Change password</h1>
        <?php if ($change_pw_error !== ''): ?><div class="auth-msg error"><?php echo htmlspecialchars($change_pw_error); ?></div><?php endif; ?>
        <form method="post" action="index.php">
            <input type="hidden" name="action" value="change_password">
            <label for="current_password">Current password</label>
            <input type="password" id="current_password" name="current_password" required>
            <label for="new_password">New password</label>
            <input type="password" id="new_password" name="new_password" minlength="8" pattern="(?=.*[A-Z]).{8,}" title="At least 8 characters, including one capital letter" required>
            <div class="pw-rules">At least 8 characters, including one capital letter</div>
            <label for="confirm_password">Confirm new password</label>
            <input type="password" id="confirm_password" name="confirm_password" required>
            <button type="submit">Change password</button>
        </form>
        <div class="switch"><a href="index.php">Cancel</a></div>
    </div>
</body>
</html>
    <?php
    exit;
}

if ($current_user === null)
{
    ?>
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<link rel="icon" href="icon.png" type="image/png">
<title>Nimbus Weather Station</title>
<style>
    :root {
        --bg: #0e1419;
        --panel: #161e26;
        --panel-edge: #232f3a;
        --ink: #e6edf3;
        --ink-dim: #7d8b99;
        --accent: #4cc4d6;
        --warn: #e0a458;
        --sans: -apple-system, 'Segoe UI', Roboto, sans-serif;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
        background: var(--bg);
        color: var(--ink);
        font-family: var(--sans);
        min-height: 100vh;
        display: flex;
        align-items: center;
        justify-content: center;
        padding: 1rem;
    }
    .auth-card {
        width: 100%;
        max-width: 360px;
        background: var(--panel);
        border: 1px solid var(--panel-edge);
        border-radius: 10px;
        padding: 1.8rem;
    }
    .auth-card h1 {
        font-size: 1.1rem;
        font-weight: 600;
        margin-bottom: 1.4rem;
    }
    .auth-card label {
        display: block;
        font-size: 0.78rem;
        color: var(--ink-dim);
        margin-bottom: 0.3rem;
    }
    .auth-card input {
        width: 100%;
        background: var(--bg);
        border: 1px solid var(--panel-edge);
        color: var(--ink);
        border-radius: 6px;
        padding: 0.55rem 0.65rem;
        margin-bottom: 1rem;
        font-size: 0.9rem;
    }
    .captcha-img {
        display: block;
        border-radius: 6px;
        border: 1px solid var(--panel-edge);
        margin-bottom: 0.6rem;
    }
    .pw-rules {
        font-size: 0.74rem;
        color: var(--ink-dim);
        margin-top: -0.7rem;
        margin-bottom: 1rem;
    }
    .auth-card button {
        width: 100%;
        background: var(--accent);
        color: #0e1419;
        border: none;
        border-radius: 6px;
        padding: 0.6rem;
        font-size: 0.9rem;
        font-weight: 600;
        cursor: pointer;
    }
    .auth-card .switch {
        text-align: center;
        margin-top: 1rem;
        font-size: 0.82rem;
        color: var(--ink-dim);
    }
    .auth-card .switch a {
        color: var(--accent);
        text-decoration: none;
    }
    .auth-msg {
        font-size: 0.82rem;
        margin-bottom: 1rem;
    }
    .auth-msg.error { color: var(--warn); }
    .auth-msg.ok { color: var(--accent); }
</style>
</head>
<body>
    <div class="auth-card">
        <?php if ($view === 'register'): ?>
            <h1>Create account</h1>
            <?php if ($auth_error !== ''): ?><div class="auth-msg error"><?php echo htmlspecialchars($auth_error); ?></div><?php endif; ?>
            <form method="post" action="index.php">
                <input type="hidden" name="action" value="register">
                <label for="username">Username</label>
                <input type="text" id="username" name="username" maxlength="32" required>
                <label for="email">Email</label>
                <input type="email" id="email" name="email" maxlength="255" required>
                <label for="password">Password</label>
                <input type="password" id="password" name="password" minlength="8" pattern="(?=.*[A-Z]).{8,}" title="At least 8 characters, including one capital letter" required>
                <div class="pw-rules">At least 8 characters, including one capital letter</div>
                <label for="captcha">Verification code</label>
                <img class="captcha-img" src="captcha.php?<?php echo time(); ?>" alt="Verification code" width="140" height="50">
                <input type="text" id="captcha" name="captcha" maxlength="8" autocomplete="off" required>
                <button type="submit">Create account</button>
            </form>
            <div class="switch">Already have an account? <a href="index.php?view=login">Log in</a></div>
        <?php elseif ($view === 'forgot_password'): ?>
            <h1>Reset password</h1>
            <?php if (isset($_GET['sent'])): ?>
                <div class="auth-msg ok">If that email is registered, a reset link has been sent - check your spam folder too.</div>
            <?php endif; ?>
            <?php if ($reset_error !== ''): ?><div class="auth-msg error"><?php echo htmlspecialchars($reset_error); ?></div><?php endif; ?>
            <form method="post" action="index.php">
                <input type="hidden" name="action" value="request_reset">
                <label for="email">Email</label>
                <input type="email" id="email" name="email" maxlength="255" required>
                <button type="submit">Send reset link</button>
            </form>
            <div class="switch"><a href="index.php?view=login">Back to login</a></div>
        <?php else: ?>
            <h1>Nimbus Weather Station</h1>
            <?php if ($registered): ?><div class="auth-msg ok">Account created - check your email for a confirmation link before logging in. Didn't get it? Check your spam folder, or register again with the same username to get a new link.</div><?php endif; ?>
            <?php if ($confirmed_msg): ?><div class="auth-msg ok">Email confirmed - please log in.</div><?php endif; ?>
            <?php if ($password_reset_msg): ?><div class="auth-msg ok">Password reset - please log in with your new password.</div><?php endif; ?>
            <?php if ($auth_error !== ''): ?><div class="auth-msg error"><?php echo htmlspecialchars($auth_error); ?></div><?php endif; ?>
            <form method="post" action="index.php">
                <input type="hidden" name="action" value="login">
                <label for="username">Username</label>
                <input type="text" id="username" name="username" maxlength="32" required>
                <label for="password">Password</label>
                <input type="password" id="password" name="password" required>
                <button type="submit">Log in</button>
            </form>
            <div class="switch">No account yet? <a href="index.php?view=register">Create one</a></div>
            <div class="switch"><a href="index.php?view=forgot_password">Forgot password?</a></div>
        <?php endif; ?>
    </div>
</body>
</html>
    <?php
    exit;
}

/* --------------------------------------------------------------------- */
/* Authenticated - render the dashboard as before                        */
/* --------------------------------------------------------------------- */

/* the (single) device belonging to the current user, along with its last */
/* reported reading (devices.last_reading, updated by update_alx.php) - */
/* no data.json file is used any more */
$device = null;
$has_reading = false;
$temp_val = '--';
$press_val = '--';
$hum_val = '--';
$batt_val = '--';
$timestamp = 'No data received yet';
$source_ip = '';
$batt_class = '';

if ($mysqli !== null)
{
    try
    {
        $stmt = $mysqli->prepare('SELECT device_number, last_reading, created_at FROM devices WHERE user_id = ? LIMIT 1');
        $stmt->bind_param('i', $current_user_id);
        $stmt->execute();
        $stmt->store_result();
        $stmt->bind_result($db_device_number, $db_last_reading, $db_created_at);

        if ($stmt->fetch())
        {
            $created_utc = new DateTime($db_created_at, new DateTimeZone('UTC'));
            $created_utc->setTimezone(new DateTimeZone('America/Toronto'));
            $device = array(
                'device_number' => strtoupper(bin2hex($db_device_number)),
                'created_at'    => $created_utc->format('Y-m-d H:i:s')
            );

            if ($db_last_reading !== null)
            {
                $d = json_decode($db_last_reading, true);

                /* treat a decoded-but-incomplete reading (e.g. "{}") the */
                /* same as no reading at all, rather than emitting warnings */
                /* for missing keys and rendering a half-populated dashboard */
                if ($d !== null && isset($d['temperature'], $d['pressure'], $d['humidity'], $d['battery'], $d['timestamp']))
                {
                    $has_reading = true;
                    /* temperature: stored as integer * 100, display as XX.XX */
                    $temp_val = number_format($d['temperature'] / 100, 2);
                    /* pressure: stored in Pa, display as hPa */
                    $press_val = number_format($d['pressure'] / 100, 2);
                    /* humidity: stored as integer * 1000, display as XX.XX */
                    $hum_val = number_format($d['humidity'] / 1000, 2);
                    /* battery: stored in mV */
                    $batt_val = $d['battery'];

                    /* timestamp from the server - convert to Toronto local time */
                    $utc_time = new DateTime($d['timestamp'], new DateTimeZone('UTC'));
                    $utc_time->setTimezone(new DateTimeZone('America/Toronto'));
                    $timestamp = 'Last reading: ' . $utc_time->format('Y-m-d H:i:s');

                    /* source ip */
                    $source_ip = isset($d['ip']) ? 'From: ' . $d['ip'] : '';
                    /* battery warning below 2500mV */
                    $batt_class = ($d['battery'] < 2500) ? ' low' : '';
                }
            }
        }
        $stmt->close();
    }
    catch (Throwable $e)
    {
        error_log('index.php: device lookup failed: ' . $e->getMessage());
    }
}
?>
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<meta http-equiv="refresh" content="10">
<link rel="icon" href="icon.png" type="image/png">
<title>Nimbus Weather Station</title>
<style>
    :root {
        --bg: #0e1419;
        --panel: #161e26;
        --panel-edge: #232f3a;
        --ink: #e6edf3;
        --ink-dim: #7d8b99;
        --accent: #4cc4d6;
        --grid: #1c2630;
        --warn: #e0a458;
        --mono: 'SFMono-Regular', 'Consolas', 'Liberation Mono', monospace;
        --sans: -apple-system, 'Segoe UI', Roboto, sans-serif;
    }

    * {
        box-sizing: border-box;
        margin: 0;
        padding: 0;
    }

    body {
        background: var(--bg);
        color: var(--ink);
        font-family: var(--sans);
        min-height: 100vh;
        padding: 2rem 1rem;
        display: flex;
        flex-direction: column;
        align-items: center;
    }

    .station-head {
        width: 100%;
        max-width: 760px;
        display: flex;
        align-items: baseline;
        justify-content: space-between;
        border-bottom: 1px solid var(--panel-edge);
        padding-bottom: 0.9rem;
        margin-bottom: 1.6rem;
    }

    .station-head h1 {
        font-size: 1.15rem;
        font-weight: 600;
        letter-spacing: 0.02em;
    }

    .station-head .right {
        display: flex;
        align-items: center;
        gap: 0.9rem;
    }

    .station-head .logout {
        font-family: var(--mono);
        font-size: 0.74rem;
        color: var(--ink-dim);
        text-decoration: none;
        border: 1px solid var(--panel-edge);
        border-radius: 5px;
        padding: 0.2rem 0.5rem;
    }

    .station-head .logout:hover {
        color: var(--ink);
        border-color: var(--accent);
    }

    .banner-ok {
        width: 100%;
        max-width: 760px;
        font-size: 0.82rem;
        color: var(--accent);
        margin-top: -1rem;
        margin-bottom: 1.2rem;
    }

    .grid {
        width: 100%;
        max-width: 760px;
        display: grid;
        grid-template-columns: repeat(2, 1fr);
        gap: 1rem;
    }

    .reading {
        background: var(--panel);
        border: 1px solid var(--panel-edge);
        border-radius: 10px;
        padding: 1.3rem 1.4rem;
        position: relative;
        overflow: hidden;
    }

    .reading::before {
        content: "";
        position: absolute;
        inset: 0;
        background-image:
            linear-gradient(var(--grid) 1px, transparent 1px),
            linear-gradient(90deg, var(--grid) 1px, transparent 1px);
        background-size: 18px 18px;
        opacity: 0.35;
        pointer-events: none;
    }

    .reading .label {
        font-family: var(--mono);
        font-size: 0.72rem;
        text-transform: uppercase;
        letter-spacing: 0.08em;
        color: var(--ink-dim);
        position: relative;
    }

    .reading .value {
        font-family: var(--mono);
        font-size: 2.4rem;
        font-weight: 600;
        line-height: 1.1;
        margin-top: 0.5rem;
        position: relative;
    }

    .reading .unit {
        font-size: 1rem;
        color: var(--ink-dim);
        font-weight: 400;
        margin-left: 0.2rem;
    }

    .reading.battery .value {
        color: var(--ink);
    }

    .reading.battery .value.low {
        color: var(--warn);
    }

    .devices {
        width: 100%;
        max-width: 760px;
        margin-top: 1rem;
        background: var(--panel);
        border: 1px solid var(--panel-edge);
        border-radius: 10px;
        padding: 1rem 1.4rem;
    }

    .devices-title {
        font-family: var(--mono);
        font-size: 0.72rem;
        text-transform: uppercase;
        letter-spacing: 0.08em;
        color: var(--ink-dim);
        margin-bottom: 0.7rem;
    }

    .device-row {
        display: flex;
        justify-content: space-between;
        align-items: baseline;
        font-family: var(--mono);
        font-size: 0.84rem;
        padding: 0.4rem 0;
        border-top: 1px solid var(--grid);
    }

    .device-row:first-child {
        border-top: none;
    }

    .device-created {
        font-size: 0.72rem;
        color: var(--ink-dim);
    }

    .footer {
        width: 100%;
        max-width: 760px;
        margin-top: 1.6rem;
        padding-top: 0.9rem;
        border-top: 1px solid var(--panel-edge);
        display: flex;
        justify-content: space-between;
        font-family: var(--mono);
        font-size: 0.74rem;
        color: var(--ink-dim);
    }

    .empty-state {
        width: 100%;
        max-width: 760px;
        background: var(--panel);
        border: 1px solid var(--panel-edge);
        border-radius: 10px;
        padding: 2.4rem 1.4rem;
        text-align: center;
        font-size: 0.95rem;
        color: var(--ink-dim);
    }

    @media (max-width: 520px) {
        .grid {
            grid-template-columns: 1fr;
            gap: 0.6rem;
        }
        .station-head h1 {
            font-size: 1rem;
        }
        .reading {
            padding: 0.9rem 1rem;
        }
        .reading .label {
            font-size: 0.64rem;
        }
        .reading .value {
            font-size: 1.7rem;
        }
        .reading .unit {
            font-size: 0.82rem;
        }
        .empty-state {
            padding: 1.6rem 1rem;
            font-size: 0.85rem;
        }
        .devices {
            padding: 0.8rem 1rem;
        }
        .devices-title {
            font-size: 0.64rem;
        }
        .device-row {
            font-size: 0.76rem;
        }
        .device-created {
            font-size: 0.64rem;
        }
        .footer {
            font-size: 0.66rem;
        }
    }
</style>
</head>
<body>

    <div class="station-head">
        <h1>Nimbus Weather Station</h1>
        <span class="right">
            <a class="logout" href="index.php?view=change_password">Change password</a>
            <a class="logout" href="index.php?logout=1">Log out</a>
        </span>
    </div>

    <?php if ($change_pw_success): ?>
    <div class="banner-ok">Password changed.</div>
    <?php endif; ?>

    <?php if ($device === null): ?>
        <div class="empty-state">No sensors added</div>
    <?php elseif (!$has_reading): ?>
        <div class="empty-state">Device has been added, now waiting for data - this may take ~15 minutes.</div>

        <div class="devices">
            <div class="devices-title">Device</div>
            <div class="device-row">
                <span class="device-number"><?php echo htmlspecialchars($device['device_number']); ?></span>
                <span class="device-created">Added <?php echo htmlspecialchars($device['created_at']); ?></span>
            </div>
        </div>
    <?php else: ?>
        <div class="grid">
            <div class="reading">
                <div class="label">Temperature</div>
                <div class="value"><?php echo $temp_val; ?><span class="unit">&deg;C</span></div>
            </div>

            <div class="reading">
                <div class="label">Pressure</div>
                <div class="value"><?php echo $press_val; ?><span class="unit">hPa</span></div>
            </div>

            <div class="reading">
                <div class="label">Humidity</div>
                <div class="value"><?php echo $hum_val; ?><span class="unit">%</span></div>
            </div>

            <div class="reading battery">
                <div class="label">Battery</div>
                <div class="value<?php echo $batt_class; ?>"><?php echo $batt_val; ?><span class="unit">mV</span></div>
            </div>
        </div>

        <div class="devices">
            <div class="devices-title">Device</div>
            <div class="device-row">
                <span class="device-number"><?php echo htmlspecialchars($device['device_number']); ?></span>
                <span class="device-created">Added <?php echo htmlspecialchars($device['created_at']); ?></span>
            </div>
        </div>

        <div class="footer">
            <span><?php echo $timestamp; ?></span>
            <span><?php echo $source_ip; ?></span>
        </div>
    <?php endif; ?>

</body>
</html>
