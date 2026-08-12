<?php
/* reset_password.php - landing page for the "forgot password" email link.
 * Verifies the token (and that it hasn't expired) before showing the "set
 * new password" form. Nothing in the database changes here until that
 * form is actually submitted - requesting the link (index.php) only ever
 * wrote the pending reset_token/reset_expires pair, not the password
 * itself, so an unopened or ignored link leaves the account untouched. */

require_once __DIR__ . '/config.php';

header('Cache-Control: no-cache, no-store, must-revalidate');
header('Pragma: no-cache');
header('Expires: 0');

$token = $_POST['token'] ?? $_GET['token'] ?? '';
$reset_pw_error = '';
$token_user_id = null;

if (preg_match('/^[0-9a-f]{64}$/', $token))
{
    try
    {
        $mysqli = new mysqli($DB_HOST, $DB_USER, $DB_PASS, $DB_NAME);

        $stmt = $mysqli->prepare('SELECT id FROM users WHERE reset_token = ? AND reset_expires > NOW() LIMIT 1');
        $stmt->bind_param('s', $token);
        $stmt->execute();
        $stmt->store_result();
        $stmt->bind_result($found_id);
        if (($stmt->num_rows === 1) && $stmt->fetch())
        {
            $token_user_id = $found_id;
        }
        $stmt->close();

        /* ---- set the new password (still on a valid token) ---- */
        if ($token_user_id !== null && $_SERVER['REQUEST_METHOD'] === 'POST')
        {
            $new_password = $_POST['new_password'] ?? '';
            $confirm_password = $_POST['confirm_password'] ?? '';

            if (strlen($new_password) < 8 || !preg_match('/[A-Z]/', $new_password))
            {
                $reset_pw_error = 'New password must be at least 8 characters and include at least one capital letter.';
            }
            elseif ($new_password !== $confirm_password)
            {
                $reset_pw_error = 'New passwords do not match.';
            }
            else
            {
                $new_hash = password_hash($new_password, PASSWORD_DEFAULT);
                $update = $mysqli->prepare('UPDATE users SET password_hash = ?, reset_token = NULL, reset_expires = NULL WHERE id = ?');
                $update->bind_param('si', $new_hash, $token_user_id);
                $update->execute();
                $update->close();

                header('Location: index.php?password_reset=1');
                exit;
            }
        }

        $mysqli->close();
    }
    catch (Throwable $e)
    {
        error_log('reset_password.php: failed: ' . $e->getMessage());
        $token_user_id = null;
    }
}
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
        <?php if ($token_user_id === null): ?>
            <h1>Reset link invalid</h1>
            <div class="auth-msg error">This password reset link is invalid or has expired.</div>
            <div class="switch"><a href="index.php?view=forgot_password">Request a new link</a></div>
        <?php else: ?>
            <h1>Set new password</h1>
            <?php if ($reset_pw_error !== ''): ?><div class="auth-msg error"><?php echo htmlspecialchars($reset_pw_error); ?></div><?php endif; ?>
            <form method="post" action="reset_password.php">
                <input type="hidden" name="token" value="<?php echo htmlspecialchars($token); ?>">
                <label for="new_password">New password</label>
                <input type="password" id="new_password" name="new_password" minlength="8" pattern="(?=.*[A-Z]).{8,}" title="At least 8 characters, including one capital letter" required>
                <div class="pw-rules">At least 8 characters, including one capital letter</div>
                <label for="confirm_password">Confirm new password</label>
                <input type="password" id="confirm_password" name="confirm_password" required>
                <button type="submit">Set password</button>
            </form>
        <?php endif; ?>
    </div>
</body>
</html>
