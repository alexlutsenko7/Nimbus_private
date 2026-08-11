<?php

require_once "config.php";



echo "<h1>Nimbus Database Installer</h1>";



// Simple "Are you sure?"

if (!isset($_POST['confirm'])) {

    echo "<p>This will <b>DROP and RECREATE</b> the database <code>$DB_NAME</code>.</p>";

    echo "<form method='POST'>";

    echo "<input type='hidden' name='confirm' value='1'>";

    echo "<input type='submit' value='Yes, install / reset database'>";

    echo "</form>";

    exit;

}



// Connect to MySQL (no DB yet)

$mysqli = new mysqli($DB_HOST, $DB_USER, $DB_PASS);

if ($mysqli->connect_errno) {

    die("Connection failed: " . $mysqli->connect_error);

}



// Drop existing DB

//$mysqli->query("DROP DATABASE IF EXISTS `$DB_NAME`");



// Create DB

//if (!$mysqli->query("CREATE DATABASE `$DB_NAME`")) {
//
//    die("Error creating database: " . $mysqli->error);

//}

echo "Database <b>$DB_NAME</b> created.<br><br>";



// Select DB

$mysqli->select_db($DB_NAME);



// Users table

$sql_users = "

CREATE TABLE users (

    id INT AUTO_INCREMENT PRIMARY KEY,

    username VARCHAR(100) NOT NULL UNIQUE,

    password_hash VARCHAR(255) NOT NULL,

    email VARCHAR(255) NOT NULL,

    confirmed TINYINT(1) NOT NULL DEFAULT 0,

    confirm_token VARCHAR(64) DEFAULT NULL,

    reset_token VARCHAR(64) DEFAULT NULL,

    reset_expires DATETIME DEFAULT NULL,

    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP

) ENGINE=InnoDB;

";



if (!$mysqli->query($sql_users)) {

    die("Error creating users table: " . $mysqli->error);

}

echo "Table <b>users</b> created.<br><br>";



// Devices table WITH voltage

$sql_devices = "

CREATE TABLE devices (

    id INT AUTO_INCREMENT PRIMARY KEY,

    user_id INT,

    device_number BINARY(9) NOT NULL UNIQUE,

    last_reading JSON DEFAULT NULL,

    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP

) ENGINE=InnoDB;

";



if (!$mysqli->query($sql_devices)) {

    die("Error creating devices table: " . $mysqli->error);

}

echo "Table <b>devices</b> created.<br><br>";



echo "<h2>Installation complete!</h2>";

echo "<a href='index.php'>Go to login</a>";

?>

