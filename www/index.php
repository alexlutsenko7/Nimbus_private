<?php
header('Cache-Control: no-cache, no-store, must-revalidate');
header('Pragma: no-cache');
header('Expires: 0');

/* read the latest data from json file */
$data_file = __DIR__ . '/data.json';
$temp_val = '--';
$press_val = '--';
$hum_val = '--';
$batt_val = '--';
$timestamp = 'No data received yet';
$source_ip = '';
$batt_class = '';

if (file_exists($data_file))
{
    $json = file_get_contents($data_file);
    $d = json_decode($json, true);

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

    .station-head .id {
        font-family: var(--mono);
        font-size: 0.78rem;
        color: var(--ink-dim);
    }

    .status-dot {
        display: inline-block;
        width: 7px;
        height: 7px;
        border-radius: 50%;
        background: var(--accent);
        margin-right: 0.45rem;
        vertical-align: middle;
        box-shadow: 0 0 6px var(--accent);
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
        <span class="id"><span class="status-dot"></span>ID&nbsp;D83BDA75F32C</span>
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

    <div class="footer">
        <span><?php echo $timestamp; ?></span>
        <span><?php echo $source_ip; ?></span>
    </div>

</body>
</html>