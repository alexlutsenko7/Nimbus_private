<?php
/* simple_dev/index.php - minimal, self-contained weather status endpoint.
 * No sensor ids, no accounts, no database - a single global "latest
 * reading" persisted to a JSON file next to this script.
 *
 * Ingest: GET ?d=T<temp>P<pressure>H<humidity>B<battery>, e.g.
 *   ?d=T23.5P1013.2H45.6B3.7  ->  temperature=23.5, pressure=1013.2, humidity=45.6, battery=3.7V
 * Responds "OK" (200) or "ERROR: <message>" (400/500), same convention as
 * ../claim_device_alx.php.
 *
 * No "d" param: renders a minimal dashboard showing the latest reading. */

$data_file = __DIR__ . '/latest_reading.json';

if (isset($_GET['d']))
{
    header('Content-Type: text/plain; charset=UTF-8');
    header('Cache-Control: no-cache, no-store, must-revalidate');
    header('Pragma: no-cache');
    header('Expires: 0');

    if (!preg_match('/^T(-?\d+(?:\.\d+)?)P(-?\d+(?:\.\d+)?)H(-?\d+(?:\.\d+)?)B(-?\d+(?:\.\d+)?)$/', $_GET['d'], $matches))
    {
        http_response_code(400);
        echo "ERROR: Invalid data format";
        exit;
    }

    $reading = array(
        'temperature' => (float)$matches[1],
        'pressure'    => (float)$matches[2],
        'humidity'    => (float)$matches[3],
        'battery'     => (float)$matches[4],
        'received_at' => gmdate('Y-m-d H:i:s') . ' UTC',
    );

    if (file_put_contents($data_file, json_encode($reading), LOCK_EX) === false)
    {
        http_response_code(500);
        echo "ERROR: Could not store reading";
        exit;
    }

    http_response_code(200);
    echo "OK";
    exit;
}

$reading = null;
if (file_exists($data_file))
{
    $reading = json_decode(file_get_contents($data_file), true);
}
?>
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<meta http-equiv="refresh" content="10">
<link rel="icon" href="../icon.png" type="image/png">
<title>Weather Status (simple_dev)</title>
<style>
    /* Design elements below are carried over as-is from ../index.php's
     * dashboard view - same palette/typography/card treatment, including
     * the battery low-voltage warning color. Anything account-specific
     * (auth-card, devices list, wifi signal on the battery card) is left
     * out since this endpoint has no accounts or device rows. */
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
        .footer {
            font-size: 0.66rem;
        }
    }
</style>
</head>
<body>

    <div class="station-head">
        <h1>Weather Status</h1>
    </div>

    <?php
        /* Same 2.5V low-battery threshold as ../index.php's 2500mV cutoff */
        $batt_class = ($reading !== null && $reading['battery'] < 2.5) ? ' low' : '';
    ?>
    <?php if ($reading === null): ?>
        <div class="empty-state">No data received yet</div>
    <?php else: ?>
        <div class="grid">
            <div class="reading">
                <div class="label">Temperature</div>
                <div class="value"><?php echo htmlspecialchars(number_format($reading['temperature'], 1)); ?><span class="unit">&deg;C</span></div>
            </div>

            <div class="reading">
                <div class="label">Pressure</div>
                <div class="value"><?php echo htmlspecialchars(number_format($reading['pressure'], 1)); ?><span class="unit">hPa</span></div>
            </div>

            <div class="reading">
                <div class="label">Humidity</div>
                <div class="value"><?php echo htmlspecialchars(number_format($reading['humidity'], 1)); ?><span class="unit">%</span></div>
            </div>

            <div class="reading battery">
                <div class="label">Battery</div>
                <div class="value<?php echo $batt_class; ?>"><?php echo htmlspecialchars(number_format($reading['battery'], 2)); ?><span class="unit">V</span></div>
            </div>
        </div>

        <div class="footer">
            <span>Last reading: <?php echo htmlspecialchars($reading['received_at']); ?></span>
        </div>
    <?php endif; ?>

</body>
</html>
