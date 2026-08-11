<?php
require_once __DIR__ . '/config.php';

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
$view = (($_POST['action'] ?? $_GET['view'] ?? '') === 'register') ? 'register' : 'login';

$mysqli = null;
try
{
    $mysqli = new mysqli($DB_HOST, $DB_USER, $DB_PASS, $DB_NAME);
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

    if (!preg_match('/^[A-Za-z0-9_]{3,32}$/', $username))
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
            $stmt = $mysqli->prepare('SELECT id FROM users WHERE username = ? LIMIT 1');
            $stmt->bind_param('s', $username);
            $stmt->execute();
            $stmt->store_result();

            if ($stmt->num_rows > 0)
            {
                $auth_error = 'That username is already taken.';
                $stmt->close();
            }
            else
            {
                $stmt->close();
                $password_hash = password_hash($password, PASSWORD_DEFAULT);
                $insert = $mysqli->prepare(
                    'INSERT INTO users (username, password_hash, email, confirmed) VALUES (?, ?, ?, 1)'
                );
                $insert->bind_param('sss', $username, $password_hash, $email);
                $insert->execute();
                $insert->close();

                header('Location: index.php?registered=1');
                exit;
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
        $stmt = $mysqli->prepare('SELECT password_hash FROM users WHERE username = ? LIMIT 1');
        $stmt->bind_param('s', $username);
        $stmt->execute();
        $stmt->store_result();
        $stmt->bind_result($db_hash);
        $found = ($stmt->num_rows === 1) && $stmt->fetch();
        $stmt->close();

        if ($found && password_verify($password, $db_hash))
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

        $auth_error = 'Invalid username or password.';
    }
    catch (Throwable $e)
    {
        error_log('index.php: login failed: ' . $e->getMessage());
        $auth_error = 'Something went wrong. Please try again later.';
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
                <button type="submit">Create account</button>
            </form>
            <div class="switch">Already have an account? <a href="index.php?view=login">Log in</a></div>
        <?php else: ?>
            <h1>Nimbus Weather Station</h1>
            <?php if ($registered): ?><div class="auth-msg ok">Account created - please log in.</div><?php endif; ?>
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

                if ($d !== null)
                {
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

    .no-devices {
        font-size: 0.88rem;
        color: var(--ink-dim);
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

    @media (max-width: 520px) {
        .grid {
            grid-template-columns: 1fr;
        }
        .reading .value {
            font-size: 2.1rem;
        }
    }
</style>
</head>
<body>

    <div class="station-head">
        <h1>Nimbus Weather Station</h1>
        <span class="right">
            <a class="logout" href="index.php?logout=1">Log out</a>
        </span>
    </div>

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
        <?php if ($device === null): ?>
            <div class="no-devices">No devices, add one.</div>
        <?php else: ?>
            <div class="device-row">
                <span class="device-number"><?php echo htmlspecialchars($device['device_number']); ?></span>
                <span class="device-created">Added <?php echo htmlspecialchars($device['created_at']); ?></span>
            </div>
        <?php endif; ?>
    </div>

    <div class="footer">
        <span><?php echo $timestamp; ?></span>
        <span><?php echo $source_ip; ?></span>
    </div>

</body>
</html>
