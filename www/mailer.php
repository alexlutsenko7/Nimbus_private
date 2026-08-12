<?php
/* mailer.php - thin wrapper around PHPMailer for the emails this app sends
 * (registration confirmation, password reset). Uses the Gmail SMTP
 * settings from config.php ($SMTP_*). */

require_once __DIR__ . '/vendor/autoload.php';

use PHPMailer\PHPMailer\PHPMailer;
use PHPMailer\PHPMailer\Exception;

/* Builds a PHPMailer instance configured with the given SMTP settings and */
/* recipient - caller still needs to set Subject/Body/AltBody and send() it */
function nimbus_mailer($SMTP_HOST, $SMTP_PORT, $SMTP_USER, $SMTP_PASS, $SMTP_FROM, $SMTP_FROM_NAME, $to_email, $to_username)
{
    $mail = new PHPMailer(true);
    $mail->isSMTP();
    $mail->Host       = $SMTP_HOST;
    $mail->Port       = $SMTP_PORT;
    $mail->SMTPAuth   = true;
    $mail->Username   = $SMTP_USER;
    $mail->Password   = $SMTP_PASS;
    $mail->SMTPSecure = PHPMailer::ENCRYPTION_STARTTLS;
    $mail->CharSet    = 'UTF-8';

    $mail->setFrom($SMTP_FROM, $SMTP_FROM_NAME);
    $mail->addAddress($to_email, $to_username);
    $mail->isHTML(true);

    return $mail;
}

/* Sends the confirmation email. Returns true on success, false on failure
 * (caller decides what to do - e.g. roll back the pending registration). */
function send_confirmation_email($SMTP_HOST, $SMTP_PORT, $SMTP_USER, $SMTP_PASS, $SMTP_FROM, $SMTP_FROM_NAME, $to_email, $to_username, $confirm_link)
{
    try
    {
        $mail = nimbus_mailer($SMTP_HOST, $SMTP_PORT, $SMTP_USER, $SMTP_PASS, $SMTP_FROM, $SMTP_FROM_NAME, $to_email, $to_username);

        $mail->Subject = 'Confirm your Nimbus account';
        $safe_username = htmlspecialchars($to_username, ENT_QUOTES);
        $safe_link     = htmlspecialchars($confirm_link, ENT_QUOTES);
        $mail->Body    = "<p>Hi {$safe_username},</p>"
            . "<p>Click the link below to activate your Nimbus Weather Station account:</p>"
            . "<p><a href=\"{$safe_link}\">{$safe_link}</a></p>"
            . "<p>If you did not create this account, you can ignore this email.</p>";
        $mail->AltBody = "Hi {$to_username},\n\nActivate your Nimbus Weather Station account by opening this link:\n{$confirm_link}\n\nIf you did not create this account, you can ignore this email.";

        $mail->send();
        return true;
    }
    catch (Exception $e)
    {
        error_log('mailer.php: failed to send confirmation email to ' . $to_email . ': ' . $mail->ErrorInfo);
        return false;
    }
}

/* Sends the "reset your password" email. Returns true on success, false */
/* on failure. The reset_token this link references is already written to */
/* the DB by the caller before this is sent - the password itself only */
/* changes once the link is opened and a new password is actually */
/* submitted on reset_password.php */
function send_password_reset_email($SMTP_HOST, $SMTP_PORT, $SMTP_USER, $SMTP_PASS, $SMTP_FROM, $SMTP_FROM_NAME, $to_email, $to_username, $reset_link)
{
    try
    {
        $mail = nimbus_mailer($SMTP_HOST, $SMTP_PORT, $SMTP_USER, $SMTP_PASS, $SMTP_FROM, $SMTP_FROM_NAME, $to_email, $to_username);

        $mail->Subject = 'Reset your Nimbus password';
        $safe_username = htmlspecialchars($to_username, ENT_QUOTES);
        $safe_link     = htmlspecialchars($reset_link, ENT_QUOTES);
        $mail->Body    = "<p>Hi {$safe_username},</p>"
            . "<p>Click the link below to set a new password for your Nimbus Weather Station account. This link expires in 1 hour.</p>"
            . "<p><a href=\"{$safe_link}\">{$safe_link}</a></p>"
            . "<p>If you did not request this, you can ignore this email - your password will not change unless you open the link and set a new one.</p>";
        $mail->AltBody = "Hi {$to_username},\n\nSet a new password for your Nimbus Weather Station account by opening this link (expires in 1 hour):\n{$reset_link}\n\nIf you did not request this, you can ignore this email - your password will not change unless you open the link and set a new one.";

        $mail->send();
        return true;
    }
    catch (Exception $e)
    {
        error_log('mailer.php: failed to send password reset email to ' . $to_email . ': ' . $mail->ErrorInfo);
        return false;
    }
}
?>
