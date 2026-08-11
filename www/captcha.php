<?php
/* captcha.php - streams a PNG verification image and stashes the code it */
/* shows in the PHP session, for index.php's registration form to check */
/* against on submit. Requires the GD extension. */

require_once __DIR__ . '/config.php';

session_start();

header('Content-Type: image/png');
header('Cache-Control: no-cache, no-store, must-revalidate');
header('Pragma: no-cache');

/* excludes visually ambiguous characters (0/O, 1/I/l) */
$chars = 'ABCDEFGHJKMNPQRSTUVWXYZ23456789';
$code = '';
for ($i = 0; $i < 5; $i++)
{
    $code .= $chars[random_int(0, strlen($chars) - 1)];
}
$_SESSION['captcha_code'] = $code;

$width  = 140;
$height = 50;
$image  = imagecreatetruecolor($width, $height);

/* colours match the dashboard's dark theme */
$bg    = imagecolorallocate($image, 22, 30, 38);
$fg    = imagecolorallocate($image, 230, 237, 243);
$noise = imagecolorallocate($image, 35, 47, 58);

imagefill($image, 0, 0, $bg);

/* noise lines and dots so the code isn't trivially readable by simple OCR */
for ($i = 0; $i < 6; $i++)
{
    imageline($image, random_int(0, $width), random_int(0, $height), random_int(0, $width), random_int(0, $height), $noise);
}
for ($i = 0; $i < 80; $i++)
{
    imagesetpixel($image, random_int(0, $width - 1), random_int(0, $height - 1), $noise);
}

/* draw each character separately with a random vertical jitter */
$font_size = 5;
$char_width = intdiv($width - 20, strlen($code));
$x = 15;
for ($i = 0; $i < strlen($code); $i++)
{
    $y = random_int(8, 20);
    imagechar($image, $font_size, $x, $y, $code[$i], $fg);
    $x += $char_width;
}

imagepng($image);
imagedestroy($image);
?>
