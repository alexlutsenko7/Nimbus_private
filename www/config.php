<?php
// --- DATABASE CONFIGURATION ---

// --- EMAIL SETTINGS (GMAIL SMTP) ---
$SMTP_HOST = "smtp.gmail.com";
$SMTP_PORT = 587;
$SMTP_USER = "alxlabs@gmail.com";  // your Gmail address
$SMTP_PASS = 'ewhf mymc amou jegg';    // app password from Google
$SMTP_FROM = "alxlabs@gmail.com";
$SMTP_FROM_NAME = "Nimbus";

// Base URL this app is deployed at - used to build the link inside
// confirmation emails. Must end with a trailing slash.
$SITE_URL = "https://alxlabs.ca/books/env_sensor/web/";


$DB_HOST = 'localhost';
$DB_USER = 'nqzcrcmy_alxlabs';
$DB_PASS = 'Rehbwf_alxlabs_12';
$DB_NAME = 'nqzcrcmy_nimbus';

// --- COOKIE SETTINGS ---
$COOKIE_NAME = "nimbus_user";
$COOKIE_EXPIRE = time() + (86400 * 30); // 30 days

// --- SESSION STORAGE ---
// Some cPanel accounts have a broken default session.save_path (the
// configured directory doesn't exist / isn't writable for this account's
// PHP version), which makes session_start() fail outright. Point sessions
// at our own directory instead - protected from direct HTTP access by
// the .htaccess placed alongside it.
$session_path = __DIR__ . '/.sessions';
if (!is_dir($session_path))
{
    @mkdir($session_path, 0700);
}
if (is_dir($session_path))
{
    session_save_path($session_path);
}

?>
