/* Main package declaration for the Nimbus companion app */
package ca.alxlabs.nimbus

/* Android system imports for permissions and Bluetooth */
import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.BluetoothLeScanner
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.util.Log
/* WebView imports for displaying the dashboard */
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.Toast
/* Activity and Compose framework imports */
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
/* Compose UI layout imports */
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.text.KeyboardOptions
/* Material 3 component imports */
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExtendedFloatingActionButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.darkColorScheme
/* Compose runtime and state management imports */
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.delay
/* Compose UI styling and positioning imports */
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
/* AndroidView allows embedding native Android views in Compose */
import androidx.compose.ui.viewinterop.AndroidView
/* Permission checking utility */
import androidx.core.content.ContextCompat
/* UUID for BLE service and characteristic identification */
import java.util.UUID
/* State management for password visibility toggle */
import androidx.compose.runtime.remember
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.material3.IconButton

/*
 * MainActivity - the single activity for the Nimbus companion app.
 * This activity manages three screens:
 *   1. Dashboard - WebView showing the weather data from the server
 *   2. BLE Scan - scans for "Weather station" devices
 *   3. WiFi Select - shows WiFi networks found by the ESP32 and handles provisioning
 */
class MainActivity : ComponentActivity() {

    /* URL of the web dashboard hosted on alxlabs.ca */
    /* The WebView loads this page to display current sensor readings */
    private val dashboardUrl = "https://alxlabs.ca/books/env_sensor/web/"

    /* Reference to the WebView instance so we can reload it from onResume */
    /* This is null until the WebView is created by the composable */
    private var dashboardWebView: WebView? = null

    /* The BLE device name that the ESP32 XIAO advertises */
    /* Only devices with this exact name will appear in scan results */
    private val targetDeviceName = "Weather station"

    /* How long to scan for BLE devices before stopping (milliseconds) */
    private val scanDurationMs = 10000L

    /* BLE GATT service UUID - must match SERVICE_UUID in the ESP32 firmware */
    private val serviceUuid = UUID.fromString("12345678-1234-1234-1234-123456789ABC")
    /* Command characteristic UUID - the app writes "SCAN" here to trigger WiFi scanning */
    private val commandUuid = UUID.fromString("12345678-1234-1234-1234-123456789ABD")
    /* WiFi list characteristic UUID - the ESP32 sends scan results here as notifications */
    private val wifiListUuid = UUID.fromString("12345678-1234-1234-1234-123456789ABE")
    /* Credential characteristic UUID - the app writes "SSID|password" here */
    private val credentialUuid = UUID.fromString("12345678-1234-1234-1234-123456789ABF")
    /* Status characteristic UUID - the ESP32 sends status updates here as notifications */
    private val statusUuid = UUID.fromString("12345678-1234-1234-1234-123456789AC0")
    /* User credential characteristic UUID - the app writes "username|password_hash" */
    /* here so the device can claim itself for the logged-in dashboard account */
    private val credUserUuid = UUID.fromString("12345678-1234-1234-1234-123456789AC1")
    /* Client Characteristic Configuration Descriptor UUID - standard BLE descriptor for enabling notifications */
    private val cccdUuid = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

    /* Name of the dashboard's login cookie - must match $COOKIE_NAME in config.php */
    private val dashboardCookieName = "nimbus_user"

    /* Tracks which screen is currently visible: "dashboard", "scan", or "wifi_select" */
    private val currentScreen = mutableStateOf("dashboard")

    /* Flag indicating whether a BLE scan is currently in progress */
    private val isScanning = mutableStateOf(false)
    /* List of discovered BLE devices matching the target name */
    private val foundDevices = mutableStateListOf<BleDeviceInfo>()
    /* Reference to the BLE scanner, obtained from the Bluetooth adapter */
    private var bleScanner: BluetoothLeScanner? = null

    /* Reference to the active GATT connection with the ESP32 */
    private var bluetoothGatt: BluetoothGatt? = null
    /* Flag indicating whether a GATT connection attempt is in progress */
    private val isConnecting = mutableStateOf(false)
    /* Flag indicating whether we have an active GATT connection */
    private val isConnected = mutableStateOf(false)

    /* List of WiFi networks received from the ESP32 scan */
    private val wifiNetworks = mutableStateListOf<WifiNetworkInfo>()
    /* Flag indicating whether the ESP32 is currently scanning WiFi */
    private val isWifiScanning = mutableStateOf(false)

    /* Controls visibility of the password entry dialog */
    private val showPasswordDialog = mutableStateOf(false)
    /* The WiFi network the user tapped on in the list */
    private val selectedNetwork = mutableStateOf<WifiNetworkInfo?>(null)
    /* The password text the user has typed into the dialog */
    private var passwordText by mutableStateOf("")

    /* Status message shown on the WiFi selection screen */
    /* Updated by handleStatusUpdate when the ESP32 sends notifications */
    private val provisioningStatus = mutableStateOf("")

    /* Watchdog for the WiFi-connect step: if the ESP32 never reports a */
    /* terminal status (CONNECT_OK/CONNECT_FAIL/CLAIM_FAILED/ERROR) after */
    /* credentials are sent - e.g. a BLE notification gets dropped, or the */
    /* firmware's own retry loop takes its full ~75s worst case for a wrong */
    /* password - the UI would otherwise sit on "Connecting to WiFi..." */
    /* indefinitely. */
    private val wifiConnectHandler = android.os.Handler(android.os.Looper.getMainLooper())
    private var wifiConnectTimeoutRunnable: Runnable? = null
    private val wifiConnectTimeoutMs = 90000L

    /* WiFi credential (ssid, password) waiting to be written once the */
    /* user-credential write ahead of it finishes - see onCharacteristicWrite */
    private var pendingWifiCredential: Pair<String, String>? = null

    /* Whether this setup session actually sent a dashboard user credential */
    /* (i.e. the phone was logged in), set by sendCredentials(). If true, */
    /* CONNECT_OK alone is NOT the final outcome - a claim attempt is still */
    /* coming and handleStatusUpdate must wait for CLAIM_OK/CLAIM_FAILED */
    /* before declaring success, otherwise a claim that fails right after */
    /* WiFi joins (e.g. a network blip) gets reported as a success */
    private var claimExpected = false

    /* Controls visibility of the "No internet connection" overlay */
    /* Set true by the WebView error handler, cleared when a page loads */
    private val showNoInternet = mutableStateOf(false)

    /* True until the dashboard's first page load finishes - shows "Loading..." */
    /* in place of the blank WebView instead of an empty/white gap */
    private val isPageLoading = mutableStateOf(true)

    /* Bumped on every onResume - lets AppScreen re-arm the offline overlay's */
    /* debounce on wake, so a "no internet" state left over from before the */
    /* phone slept doesn't reappear instantly; it gets the same fresh grace */
    /* period a brand new failure would, while the resume retry (below) runs */
    private val resumeSignal = mutableStateOf(0)

    /*
     * Permission launcher - registered with the activity result API.
     * When the system permission dialog closes, this callback checks
     * whether all requested permissions were granted.
     * If granted: switches to the scan screen and starts BLE scanning.
     * If denied: shows a toast explaining why permissions are needed.
     */
    private val permissionLauncher =
        registerForActivityResult(
            ActivityResultContracts.RequestMultiplePermissions()
        ) { result ->
            /* Check if every requested permission was granted */
            val allGranted = result.values.all { it }
            if (allGranted) {
                /* Permissions granted - switch to scan screen */
                currentScreen.value = "scan"
                /* Start scanning for BLE devices */
                startBleScan()
            } else {
                /* Permissions denied - inform the user */
                Toast.makeText(
                    this,
                    "Wi-Fi and Bluetooth permissions are needed to add a sensor",
                    Toast.LENGTH_LONG
                ).show()
            }
        }

    /*
     * Called when the activity resumes from background - this fires on
     * every screen wake/unlock too, not just after being truly backgrounded,
     * since Android pauses/resumes the foreground activity around screen
     * off/on. The dashboard page already auto-refreshes itself every 10s
     * (see index.php's meta refresh), so a forced reload here is only
     * needed to escape a stuck "no internet" error page - doing it
     * unconditionally instead re-fetches the whole page over the network
     * on every wake, which is what caused the multi-second white flash.
     * Uses loadUrl instead of reload because reload on an error
     * page just reloads the error, not the original URL.
     */
    override fun onResume() {
        super.onResume()
        /* Give the offline overlay's debounce a fresh start on every wake */
        resumeSignal.value++
        /* Only force a reload if we're stuck showing the offline overlay */
        if (currentScreen.value == "dashboard" && showNoInternet.value)
        {
            /* Force a fresh navigation to the dashboard URL */
            dashboardWebView?.loadUrl(dashboardUrl)
        }
    }
    /* Force-clear the Android BLE GATT cache using hidden API via reflection */
    /* This is not an official API but works on Samsung and most Android devices */
    @SuppressLint("MissingPermission")
    private fun refreshGattCache(gatt: BluetoothGatt): Boolean {
        try {
            val refreshMethod = gatt.javaClass.getMethod("refresh")
            val result = refreshMethod.invoke(gatt) as Boolean
            Log.d("BLE_GATT", "GATT cache refresh result: $result")
            return result
        } catch (e: Exception) {
            Log.e("BLE_GATT", "GATT cache refresh failed: ${e.message}")
            return false
        }
    }
    /*
     * Called when the activity is first created.
     * Sets up the Compose UI with the dark color scheme matching
     * the web dashboard, and configures the screen navigation.
     */
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        /* Set the Compose content for this activity */
        setContent {
            /* Define the dark color scheme to match the web dashboard */
            /* primaryContainer: button background color */
            /* onPrimaryContainer: button text color (teal accent) */
            /* surface: app background color */
            /* onSurface: general text color */
            val nimbusColors = darkColorScheme(
                primaryContainer = Color(0xFF1A3040),
                onPrimaryContainer = Color(0xFF4CC4D6),
                surface = Color(0xFF0E1419),
                onSurface = Color(0xFFE6EDF3)
            )
            /* Apply the color scheme to the entire app */
            MaterialTheme(colorScheme = nimbusColors) {
                /* Surface fills the entire screen with the background color */
                Surface(modifier = Modifier.fillMaxSize()) {
                    /* Navigate between screens based on currentScreen state */
                    when (currentScreen.value) {
                        /* Dashboard screen - shows web dashboard with Add sensor button */
                        "dashboard" -> AppScreen(
                            url = dashboardUrl,
                            onAddSensor = { requestSensorPermissions() },
                            onWebViewCreated = { dashboardWebView = it },
                            noInternet = showNoInternet.value,
                            isLoading = isPageLoading.value,
                            resumeSignal = resumeSignal.value,
                            onNetworkError = { showNoInternet.value = true },
                            onLoadStarted = { isPageLoading.value = true },
                            onPageLoaded = {
                                showNoInternet.value = false
                                isPageLoading.value = false
                            }
                        )
                        /* BLE scan screen - shows discovered Weather station devices */
                        "scan" -> ScanScreen(
                            devices = foundDevices,
                            isScanning = isScanning.value,
                            onRescan = { startBleScan() },
                            onBack = {
                                /* Stop any active scan before going back */
                                stopBleScan()
                                /* Return to the dashboard */
                                currentScreen.value = "dashboard"
                            },
                            onDeviceSelected = { device -> connectToDevice(device) }
                        )
                        /* WiFi selection screen - shows networks found by ESP32 */
                        "wifi_select" -> WifiSelectScreen(
                            networks = wifiNetworks,
                            isScanning = isWifiScanning.value,
                            isConnecting = isConnecting.value,
                            status = provisioningStatus.value,
                            showPasswordDialog = showPasswordDialog.value,
                            selectedNetwork = selectedNetwork.value,
                            passwordText = passwordText,
                            /* Update password text when the user types */
                            onPasswordChanged = { passwordText = it },
                            onNetworkSelected = { network ->
                                /* Store the selected network */
                                selectedNetwork.value = network
                                /* Clear any previous password */
                                passwordText = ""
                                /* If the network is encrypted, show password dialog */
                                if (network.encrypted) {
                                    showPasswordDialog.value = true
                                } else {
                                    /* Open network - send empty password directly */
                                    sendCredentials(network.ssid, "")
                                }
                            },
                            onPasswordSubmit = {
                                /* Close the password dialog */
                                showPasswordDialog.value = false
                                /* Send the SSID and password to the ESP32 */
                                sendCredentials(
                                    selectedNetwork.value?.ssid ?: "",
                                    passwordText
                                )
                            },
                            onPasswordDismiss = {
                                /* User cancelled the password dialog */
                                showPasswordDialog.value = false
                            },
                            onBack = {
                                /* Disconnect GATT before going back to scan */
                                disconnectGatt()
                                currentScreen.value = "scan"
                            }
                        )
                    }
                }
            }
        }
    }

    /*
     * Builds the list of runtime permissions needed for BLE scanning.
     * Android 12 introduced new Bluetooth-specific permissions,
     * while older versions require location permission for BLE scanning.
     */
    private fun requiredPermissions(): Array<String> {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            /* Android 12+ uses dedicated Bluetooth runtime permissions */
            arrayOf(
                Manifest.permission.BLUETOOTH_SCAN,
                Manifest.permission.BLUETOOTH_CONNECT
            )
        } else {
            /* Android 11 and older require location for BLE scanning */
            arrayOf(
                Manifest.permission.ACCESS_FINE_LOCATION
            )
        }
    }

    /*
     * Checks which permissions are already granted and requests
     * only the missing ones. If all are granted, proceeds to scanning.
     */
    private fun requestSensorPermissions() {
        /* Filter the required permissions to find ones not yet granted */
        val needed = requiredPermissions().filter { permission ->
            ContextCompat.checkSelfPermission(this, permission) !=
                    PackageManager.PERMISSION_GRANTED
        }

        if (needed.isEmpty()) {
            /* All permissions already granted - go directly to scan */
            currentScreen.value = "scan"
            startBleScan()
        } else {
            /* Request the missing permissions via the system dialog */
            permissionLauncher.launch(needed.toTypedArray())
        }
    }

    /*
     * Starts a BLE scan for devices advertising the target name.
     * Clears previous results, obtains the scanner, and starts
     * scanning with an automatic timeout.
     */
    @SuppressLint("MissingPermission")
    private fun startBleScan() {
        /* Clear any devices from a previous scan */
        foundDevices.clear()
        /* Set the scanning flag so the UI shows the progress indicator */
        isScanning.value = true

        Log.d("BLE_SCAN", "Starting scan...")

        /* Get the Bluetooth adapter from the system service */
        val bluetoothManager =
            getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
        val adapter = bluetoothManager.adapter

        /* Check that Bluetooth is available and enabled */
        if (adapter == null || !adapter.isEnabled) {
            Log.e("BLE_SCAN", "Bluetooth adapter null or not enabled")
            /* Inform the user to enable Bluetooth */
            Toast.makeText(this, "Please enable Bluetooth", Toast.LENGTH_LONG).show()
            /* Clear the scanning flag */
            isScanning.value = false
            return
        }

        /* Obtain the BLE scanner from the adapter */
        bleScanner = adapter.bluetoothLeScanner

        /* Verify the scanner was obtained successfully */
        if (bleScanner == null) {
            Log.e("BLE_SCAN", "BLE scanner is null")
            isScanning.value = false
            return
        }

        /* Start the actual BLE scan with our callback */
        Log.d("BLE_SCAN", "Scanner obtained, starting scan now")
        bleScanner?.startScan(scanCallback)

        /* Schedule automatic scan stop after the timeout period */
        android.os.Handler(mainLooper).postDelayed({
            /* Stop scanning when the timeout expires */
            stopBleScan()
            Log.d("BLE_SCAN", "Scan stopped after timeout, found ${foundDevices.size} devices")
        }, scanDurationMs)
    }

    /*
     * Stops an active BLE scan if one is running.
     * Safe to call even if no scan is active.
     */
    @SuppressLint("MissingPermission")
    private fun stopBleScan() {
        /* Only attempt to stop if we are currently scanning */
        if (isScanning.value) {
            /* Tell the BLE scanner to stop */
            bleScanner?.stopScan(scanCallback)
            /* Clear the scanning flag so the UI updates */
            isScanning.value = false
        }
    }

    /*
     * BLE scan callback - receives results from the BLE scanner.
     * Filters by device name and deduplicates by MAC address.
     */
    private val scanCallback = object : ScanCallback() {
        @SuppressLint("MissingPermission")
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            /* Extract the device name and MAC address */
            val deviceName = result.device.name
            val deviceAddress = result.device.address

            /* Log every discovered device for debugging */
            Log.d("BLE_SCAN", "Found: name=$deviceName addr=$deviceAddress rssi=${result.rssi}")

            /* Skip devices without a name or address */
            if (deviceName == null || deviceAddress == null) {
                return
            }

            /* Only accept devices matching our target name */
            if (deviceName != targetDeviceName) {
                return
            }

            /* Check if we already found this device by MAC address */
            val alreadyFound = foundDevices.any { it.address == deviceAddress }
            /* Only add new devices to avoid duplicates */
            if (!alreadyFound) {
                foundDevices.add(
                    BleDeviceInfo(
                        name = deviceName,
                        address = deviceAddress,
                        rssi = result.rssi
                    )
                )
            }
        }

        /* Called if the BLE scan fails to start */
        override fun onScanFailed(errorCode: Int) {
            Log.e("BLE_SCAN", "Scan failed with error code $errorCode")
            /* Clear the scanning flag */
            isScanning.value = false
        }
    }

    /*
     * Initiates a GATT connection to the selected BLE device.
     * Stops any active scan first, then connects using the MAC address.
     */
    @SuppressLint("MissingPermission")
    private fun connectToDevice(device: BleDeviceInfo) {
        /* Stop scanning before connecting */
        stopBleScan()
        /* Set UI state to show connection in progress */
        isConnecting.value = true
        provisioningStatus.value = "Connecting..."
        /* Switch to the WiFi selection screen */
        currentScreen.value = "wifi_select"
        /* Clear any WiFi networks from a previous session */
        wifiNetworks.clear()

        /* Get the Bluetooth adapter to resolve the MAC address */
        val bluetoothManager =
            getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
        val adapter = bluetoothManager.adapter
        /* Create a BluetoothDevice object from the MAC address */
        val bleDevice: BluetoothDevice = adapter.getRemoteDevice(device.address)

        /* Initiate the GATT connection */
        bluetoothGatt = bleDevice.connectGatt(this, false, gattCallback)

        /* Timeout: if services not discovered in 10 seconds, force refresh */
        android.os.Handler(mainLooper).postDelayed({
            if (isConnecting.value || !isConnected.value)
            {
                Log.d("BLE_GATT", "Connection timeout - forcing cache refresh")
                provisioningStatus.value = "Retrying connection..."
                /* Disconnect and close the stale connection */
                bluetoothGatt?.disconnect()
                bluetoothGatt?.close()
                /* Reconnect with TRANSPORT_LE to force a fresh GATT discovery */
                bluetoothGatt = bleDevice.connectGatt(
                    this,
                    false,
                    gattCallback,
                    BluetoothDevice.TRANSPORT_LE
                )
            }
        }, 10000)
    }

    /*
     * Disconnects and cleans up the GATT connection.
     * Resets all connection state flags.
     */
    @SuppressLint("MissingPermission")
    private fun disconnectGatt() {
        /* Disconnect the active GATT connection if any */
        bluetoothGatt?.disconnect()
        /* Release the GATT client resources */
        bluetoothGatt?.close()
        /* Clear the reference */
        bluetoothGatt = null
        /* Reset connection state flags */
        isConnected.value = false
        isConnecting.value = false
        cancelWifiConnectTimeout()
    }

    /*
     * Starts (restarting if already running) the WiFi-connect watchdog.
     * Call right after the WiFi credential write is issued; cancel via
     * cancelWifiConnectTimeout() as soon as a terminal status arrives.
     */
    private fun startWifiConnectTimeout() {
        cancelWifiConnectTimeout()
        val runnable = Runnable {
            Log.w("BLE_GATT", "WiFi connect timed out - no response from device")
            provisioningStatus.value = "WiFi connection failed. Check the password and try again."
            Toast.makeText(this, "WiFi connection failed", Toast.LENGTH_LONG).show()
        }
        wifiConnectTimeoutRunnable = runnable
        wifiConnectHandler.postDelayed(runnable, wifiConnectTimeoutMs)
    }

    /*
     * Cancels the WiFi-connect watchdog, if one is pending.
     */
    private fun cancelWifiConnectTimeout() {
        wifiConnectTimeoutRunnable?.let { wifiConnectHandler.removeCallbacks(it) }
        wifiConnectTimeoutRunnable = null
    }

    /*
     * Parses a single WiFi network entry from the ESP32.
     * Format: "SSID,RSSI,encrypted". Deduplicates by SSID
     * and keeps the list sorted by signal strength.
     */
    private fun parseAndAddWifiNetwork(data: String) {
        /* Split the comma-delimited entry into fields */
        val parts = data.split(",")
        /* Validate we have at least 3 fields */
        if (parts.size >= 3) {
            /* Extract the SSID */
            val ssid = parts[0]
            /* Check if this SSID is already in the list */
            val alreadyFound = wifiNetworks.any { it.ssid == ssid }
            /* Only add if not a duplicate */
            if (!alreadyFound) {
                /* Add the parsed network to the list */
                wifiNetworks.add(
                    WifiNetworkInfo(
                        ssid = ssid,
                        rssi = parts[1].toIntOrNull() ?: -99,
                        encrypted = parts[2] == "1"
                    )
                )
                /* Re-sort so strongest signals appear first */
                wifiNetworks.sortByDescending { it.rssi }
            }
        }
    }

    /*
     * GATT callback - handles all BLE connection events and data exchange.
     * Called on a background thread by the Android BLE stack.
     * UI updates must use runOnUiThread.
     */
    private val gattCallback = object : BluetoothGattCallback() {

        /*
         * Called when the GATT connection state changes.
         * Connected: triggers service discovery.
         * Disconnected: resets all connection state.
         */
        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            /* Check if we just connected */
            if (newState == BluetoothProfile.STATE_CONNECTED) if (newState == BluetoothProfile.STATE_CONNECTED) {
                Log.d("BLE_GATT", "Connected, refreshing GATT cache...")
                /* Force-clear any stale cached GATT data */
                refreshGattCache(gatt)
                /* Small delay to let the cache clear take effect */
                runOnUiThread {
                    isConnected.value = true
                    isConnecting.value = false
                    provisioningStatus.value = "Connected, discovering services..."
                }
                /* Delay service discovery to let cache refresh complete */
                android.os.Handler(mainLooper).postDelayed({
                    gatt.discoverServices()
                }, 500)
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                Log.d("BLE_GATT", "Disconnected")
                cancelWifiConnectTimeout()
                /* Reset connection state on the main thread */
                runOnUiThread {
                    isConnected.value = false
                    isConnecting.value = false
                    provisioningStatus.value = "Disconnected"
                }
            }
        }

        /*
         * Called when GATT service discovery completes.
         * Subscribes to notifications on WiFi list and status characteristics,
         * then sends the SCAN command.
         * Writes are staggered with delays because BLE allows only one
         * pending GATT operation at a time.
         */
        @SuppressLint("MissingPermission")
        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            /* Check if service discovery succeeded */
            if (status != BluetoothGatt.GATT_SUCCESS) {
                Log.e("BLE_GATT", "Service discovery failed with status $status")
                return
            }

            Log.d("BLE_GATT", "Services discovered")

            /* Look for our custom service by UUID */
            val service = gatt.getService(serviceUuid)
            /* If not found, the device may be running wrong firmware */
            if (service == null) {
                Log.e("BLE_GATT", "Nimbus service not found")
                runOnUiThread {
                    provisioningStatus.value = "Service not found on device"
                }
                return
            }

            /* Subscribe to WiFi list notifications */
            /* ESP32 sends each network as a separate notification here */
            val wifiListChar = service.getCharacteristic(wifiListUuid)
            if (wifiListChar != null) {
                /* Enable notifications locally on Android side */
                gatt.setCharacteristicNotification(wifiListChar, true)
                /* Write CCCD descriptor to tell ESP32 to send notifications */
                val descriptor = wifiListChar.getDescriptor(cccdUuid)
                if (descriptor != null) {
                    descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                    gatt.writeDescriptor(descriptor)
                }
            }

            /* Subscribe to status notifications */
            /* ESP32 sends SCANNING, CONNECT_OK, CONNECT_FAIL etc. here */
            val statusChar = service.getCharacteristic(statusUuid)
            if (statusChar != null) {
                /* Enable notifications locally */
                gatt.setCharacteristicNotification(statusChar, true)
                val descriptor = statusChar.getDescriptor(cccdUuid)
                if (descriptor != null) {
                    descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                    /* Delay 500ms to avoid GATT operation collision */
                    android.os.Handler(mainLooper).postDelayed({
                        gatt.writeDescriptor(descriptor)
                    }, 500)
                }
            }

            /* Send SCAN command after all subscriptions are set up */
            /* 1500ms delay ensures both descriptor writes have completed */
            android.os.Handler(mainLooper).postDelayed({
                sendScanCommand()
            }, 1500)
        }

        /*
         * Called when the ESP32 sends a notification.
         * Routes data to the appropriate handler based on characteristic UUID.
         * Uses deprecated API because it works across all Android versions.
         */
        @Deprecated("Deprecated in API 33")
        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic
        ) {
            /* Convert the raw bytes to a string */
            val value = String(characteristic.value)
            Log.d("BLE_GATT", "Notification from ${characteristic.uuid}: $value")

            /* Route based on which characteristic sent the notification */
            when (characteristic.uuid) {
                /* WiFi list data - parse and add to the network list */
                wifiListUuid -> {
                    runOnUiThread {
                        parseAndAddWifiNetwork(value)
                    }
                }
                /* Status update - handle the status message */
                statusUuid -> {
                    runOnUiThread {
                        handleStatusUpdate(value)
                    }
                }
            }
        }

        /*
         * Called when a characteristic write completes. BLE only allows one
         * pending GATT operation at a time, so the user-credential write in
         * sendCredentials() chains the WiFi-credential write off this
         * callback instead of a fixed delay - a fixed delay raced with the
         * write and silently dropped it.
         */
        @Deprecated("Deprecated in API 33")
        override fun onCharacteristicWrite(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int
        ) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                Log.e("BLE_GATT", "Write failed for ${characteristic.uuid}, status=$status")
            }

            if (characteristic.uuid == credUserUuid) {
                val pending = pendingWifiCredential
                pendingWifiCredential = null
                if (pending != null) {
                    writeWifiCredential(pending.first, pending.second)
                }
            }
        }
    }

    /*
     * Sends the SCAN command to the ESP32 via the command characteristic.
     * Triggers WiFi network scanning on the device.
     */
    @SuppressLint("MissingPermission")
    private fun sendScanCommand() {
        /* Get the GATT service - return if not connected */
        val service = bluetoothGatt?.getService(serviceUuid) ?: return
        /* Get the command characteristic */
        val commandChar = service.getCharacteristic(commandUuid) ?: return

        /* Update UI to show scanning state */
        isWifiScanning.value = true
        provisioningStatus.value = "Scanning WiFi networks..."

        /* Write the SCAN command as bytes */
        commandChar.value = "SCAN".toByteArray()
        /* Send the write to the ESP32 */
        bluetoothGatt?.writeCharacteristic(commandChar)

        Log.d("BLE_GATT", "SCAN command sent")
    }

    /*
     * Sends WiFi credentials to the ESP32 via the credential characteristic.
     * Payload format: "SSID|password"
     *
     * If a dashboard user credential is available, that write is sent
     * first and this one is queued to follow once it completes (BLE only
     * allows one pending GATT operation at a time - see
     * onCharacteristicWrite in gattCallback) so the device can claim
     * itself for that account once WiFi setup finishes on its end.
     */
    @SuppressLint("MissingPermission")
    private fun sendCredentials(ssid: String, password: String) {
        /* Update UI to show we are sending */
        provisioningStatus.value = "Sending credentials..."

        claimExpected = trySendUserCredential()
        if (claimExpected) {
            /* Wait for onCharacteristicWrite(credUserUuid) to fire before */
            /* sending this - sending both back-to-back drops the second */
            /* write silently since BLE only allows one pending op */
            pendingWifiCredential = Pair(ssid, password)
        } else {
            writeWifiCredential(ssid, password)
        }
    }

    /*
     * Actually writes "SSID|password" to the credential characteristic.
     * Split out from sendCredentials() so it can be called either
     * immediately or after the user-credential write completes.
     */
    @SuppressLint("MissingPermission")
    private fun writeWifiCredential(ssid: String, password: String) {
        /* Get the GATT service - return if not connected */
        val service = bluetoothGatt?.getService(serviceUuid) ?: return
        /* Get the credential characteristic */
        val credentialChar = service.getCharacteristic(credentialUuid) ?: return

        /* Build the pipe-delimited payload */
        val payload = "$ssid|$password"
        /* Write the payload as bytes */
        credentialChar.value = payload.toByteArray()
        /* Send the write to the ESP32 */
        bluetoothGatt?.writeCharacteristic(credentialChar)
        /* Bound how long we wait for a terminal status back */
        startWifiConnectTimeout()

        Log.d("BLE_GATT", "Credentials sent for SSID: $ssid")
    }

    /*
     * Reads the dashboard's login cookie (set by index.php after a
     * successful web login) via Android's CookieManager and forwards
     * "username|password_hash" to the ESP32 via the user credential
     * characteristic. If the phone isn't logged into the dashboard, this
     * is skipped entirely and the device stays unclaimed (user_id=0) -
     * the user can claim it later by logging in and re-running setup.
     * Returns true if a write was actually issued, so the caller knows
     * whether to wait for it to complete before writing anything else.
     */
    @SuppressLint("MissingPermission")
    private fun trySendUserCredential(): Boolean {
        /* Get the GATT service - bail out if not connected */
        val service = bluetoothGatt?.getService(serviceUuid) ?: return false
        /* Get the user credential characteristic */
        val userChar = service.getCharacteristic(credUserUuid) ?: return false

        /* Read the raw Cookie header value for the dashboard URL */
        val cookieHeader = android.webkit.CookieManager.getInstance().getCookie(dashboardUrl)
        /* Find the nimbus_user cookie among any others and pull its value */
        val rawValue = cookieHeader
            ?.split("; ")
            ?.map { it.split("=", limit = 2) }
            ?.firstOrNull { it.size == 2 && it[0] == dashboardCookieName }
            ?.get(1)

        if (rawValue == null) {
            Log.w("BLE_GATT", "Not logged into dashboard - device will stay unclaimed")
            Toast.makeText(
                this,
                "Log in to the dashboard first to link this sensor to your account",
                Toast.LENGTH_LONG
            ).show()
            return false
        }

        /* Cookie values are URL-encoded by PHP's setcookie() - decode back */
        /* to the raw "username|password_hash" the ESP32/server expect */
        val payload = java.net.URLDecoder.decode(rawValue, "UTF-8")

        userChar.value = payload.toByteArray()
        bluetoothGatt?.writeCharacteristic(userChar)

        Log.d("BLE_GATT", "User credential sent")
        return true
    }

    /*
     * Legacy bulk parser for WiFi network list.
     * Kept for backward compatibility. Current firmware sends
     * networks individually via parseAndAddWifiNetwork instead.
     */
    private fun parseWifiList(data: String) {
        /* Clear any existing networks */
        wifiNetworks.clear()

        /* Split into individual entries by newline */
        val lines = data.split("\n")
        /* Parse each line as "SSID,RSSI,encrypted" */
        for (line in lines) {
            val parts = line.split(",")
            /* Validate field count */
            if (parts.size >= 3) {
                /* Add the parsed network */
                wifiNetworks.add(
                    WifiNetworkInfo(
                        ssid = parts[0],
                        rssi = parts[1].toIntOrNull() ?: -99,
                        encrypted = parts[2] == "1"
                    )
                )
            }
        }

        /* Sort by signal strength, strongest first */
        wifiNetworks.sortByDescending { it.rssi }

        Log.d("BLE_GATT", "Parsed ${wifiNetworks.size} WiFi networks")
    }

    /*
     * Handles status notifications from the ESP32.
     * Updates the UI status message and shows toast for final outcomes.
     */
    private fun handleStatusUpdate(status: String) {
        Log.d("BLE_GATT", "Status update: $status")

        when {
            /* ESP32 has started scanning WiFi */
            status == "SCANNING" -> {
                provisioningStatus.value = "Device is scanning WiFi..."
            }
            /* ESP32 finished scanning */
            status.startsWith("SCAN_DONE") -> {
                provisioningStatus.value = "WiFi scan complete"
                /* Clear the scanning flag */
                isWifiScanning.value = false
            }
            /* ESP32 is connecting to the selected network */
            status == "CONNECTING" -> {
                provisioningStatus.value = "Connecting to WiFi..."
            }
            /* ESP32 joined WiFi and stored credentials. If this session never */
            /* sent a user credential (phone wasn't logged in), there's no */
            /* claim attempt coming - this IS the final outcome. Otherwise a */
            /* claim attempt is still in flight and CLAIM_OK/CLAIM_FAILED */
            /* below decides the real outcome - showing success here too */
            /* would be a lie if that claim then fails from a network blip */
            status == "CONNECT_OK" -> {
                if (claimExpected) {
                    /* Restart the watchdog with a fresh window for the claim */
                    /* step - the original one was budgeted for the WiFi */
                    /* connect retries alone and could otherwise expire while */
                    /* claim_device()'s own retries are still in flight */
                    startWifiConnectTimeout()
                    provisioningStatus.value = "WiFi connected, linking sensor to your account..."
                } else {
                    cancelWifiConnectTimeout()
                    provisioningStatus.value = "WiFi connected! Credentials saved."
                    Toast.makeText(this, "Sensor configured successfully!", Toast.LENGTH_LONG).show()
                }
            }
            /* ESP32 failed to connect */
            status == "CONNECT_FAIL" -> {
                cancelWifiConnectTimeout()
                provisioningStatus.value = "WiFi connection failed. Check password."
                /* Show failure toast */
                Toast.makeText(this, "WiFi connection failed", Toast.LENGTH_LONG).show()
            }
            /* WiFi joined AND the sensor was successfully linked to the */
            /* dashboard account - the actual final success signal when a */
            /* claim was expected */
            status == "CLAIM_OK" -> {
                cancelWifiConnectTimeout()
                provisioningStatus.value = "Success! Sensor linked to your account."
                Toast.makeText(this, "Sensor configured successfully!", Toast.LENGTH_LONG).show()
            }
            /* ESP32 joined WiFi (CONNECT_OK already fired) but couldn't */
            /* reach the server to link this sensor to the account */
            status == "CLAIM_FAILED" -> {
                cancelWifiConnectTimeout()
                provisioningStatus.value = "WiFi connected, but linking to your account failed"
                Toast.makeText(this, "WiFi connected, but linking to your account failed", Toast.LENGTH_LONG).show()
            }
            /* Any error from the ESP32 */
            status.startsWith("ERROR") -> {
                cancelWifiConnectTimeout()
                provisioningStatus.value = status
            }
        }
    }

    /*
     * Called when the activity is being destroyed.
     * Cleans up BLE resources to avoid leaking connections.
     */
    override fun onDestroy() {
        super.onDestroy()
        /* Stop any active BLE scan */
        stopBleScan()
        /* Disconnect and close any active GATT connection */
        disconnectGatt()
    }
}

/*
 * Data class representing a discovered BLE device.
 * Holds the device name, MAC address, and signal strength.
 */
data class BleDeviceInfo(
    val name: String,
    val address: String,
    val rssi: Int
)

/*
 * Data class representing a WiFi network found by the ESP32.
 * Holds the SSID, signal strength, and whether it requires a password.
 */
data class WifiNetworkInfo(
    val ssid: String,
    val rssi: Int,
    val encrypted: Boolean
)

/*
 * Dashboard screen composable.
 * Displays the web dashboard in a WebView with an "Add sensor" button.
 * Shows a "No internet connection" overlay when the network is unavailable.
 * The error handler catches connection failures, replaces Chrome's error
 * page with a dark blank page, and retries after 5 seconds.
 */
@Composable
fun AppScreen(
    url: String,
    onAddSensor: () -> Unit,
    onWebViewCreated: (WebView) -> Unit,
    noInternet: Boolean,
    isLoading: Boolean,
    resumeSignal: Int,
    onNetworkError: () -> Unit,
    onLoadStarted: () -> Unit,
    onPageLoaded: () -> Unit
) {
    /* Box allows stacking the overlay and button on top of the WebView */
    Box(modifier = Modifier.fillMaxSize()) {
        /* AndroidView bridges native Android WebView into Compose */
        AndroidView(
            modifier = Modifier.fillMaxSize(),
            factory = { context ->
                /* Create the WebView instance */
                WebView(context).apply {
                    /* WebView paints white by default until the page's own */
                    /* CSS background applies - match the dashboard's dark */
                    /* theme so any load-in-progress gap is dark, not a flash */
                    /* of white (same color as the offline placeholder page) */
                    setBackgroundColor(android.graphics.Color.parseColor("#0e1419"))
                    /* Set up the WebView client with error handling */
                    webViewClient = object : WebViewClient() {
                        /*
                         * WebView still calls onPageFinished with the original URL
                         * after a FAILED navigation, right after the error handler
                         * fires - so onPageFinished can't tell success from failure
                         * from the URL alone. Set by the error handlers below, checked
                         * (and reset) in onPageFinished so a failed load's callback
                         * doesn't immediately clear the offline overlay it just set.
                         */
                        private var lastLoadFailed = false

                        /*
                         * Called at the start of every navigation, including the
                         * periodic meta-refresh reload the dashboard page does on
                         * its own every 10s. Re-arms the loading state so a slow
                         * reload (e.g. right after the phone wakes from idle) can
                         * show "Loading..." again, not just on the very first load.
                         */
                        override fun onPageStarted(view: WebView?, startedUrl: String?, favicon: android.graphics.Bitmap?) {
                            if (startedUrl != null && startedUrl.startsWith("http"))
                            {
                                lastLoadFailed = false
                                onLoadStarted()
                            }
                        }

                        /*
                         * Deprecated error handler - catches connection-level errors
                         * like ERR_CONNECTION_ABORTED and ERR_NAME_NOT_RESOLVED.
                         * The modern API does not catch these on many devices.
                         */
                        @Deprecated("Deprecated in API 23")
                        override fun onReceivedError(
                            view: WebView?,
                            errorCode: Int,
                            description: String?,
                            failingUrl: String?
                        ) {
                            lastLoadFailed = true
                            /* Replace Chrome's white error page with dark blank */
                            /* Background color matches the dashboard theme */
                            view?.loadData(
                                "<html><body style='background:#0e1419'></body></html>",
                                "text/html",
                                "UTF-8"
                            )
                            /* Show the "No internet" overlay */
                            onNetworkError()
                            /* Schedule automatic retry after 5 seconds */
                            view?.postDelayed({
                                /* Load the actual dashboard URL, not the error page */
                                view.loadUrl(url)
                            }, 5000)
                        }

                        /*
                         * Modern connection-error handler (API 23+). Catches the same
                         * class of errors as the deprecated onReceivedError above
                         * (ERR_INTERNET_DISCONNECTED, ERR_NAME_NOT_RESOLVED, etc), but
                         * which of the two actually fires for a given failure is
                         * inconsistent across devices/WebView versions - e.g. a total
                         * offline failure (airplane mode) can be reported only through
                         * this overload on some devices, leaving the deprecated one
                         * silent. Without this override, WebView still calls
                         * onPageFinished with the original URL despite the failed
                         * load, which cleared the loading state with no offline
                         * overlay ever shown - just a blank screen. Overriding both
                         * closes that gap regardless of which path a given device uses.
                         */
                        override fun onReceivedError(
                            view: WebView?,
                            request: android.webkit.WebResourceRequest?,
                            error: android.webkit.WebResourceError?
                        ) {
                            /* Only handle main page errors */
                            if (request?.isForMainFrame == true)
                            {
                                lastLoadFailed = true
                                /* Replace Chrome's white error page with dark blank */
                                view?.loadData(
                                    "<html><body style='background:#0e1419'></body></html>",
                                    "text/html",
                                    "UTF-8"
                                )
                                /* Show the "No internet" overlay */
                                onNetworkError()
                                /* Schedule automatic retry after 5 seconds */
                                view?.postDelayed({
                                    view.loadUrl(url)
                                }, 5000)
                            }
                        }

                        /*
                         * Catches HTTP-level errors (4xx, 5xx responses).
                         * Only handles main frame errors, not subresources.
                         */
                        override fun onReceivedHttpError(
                            view: WebView?,
                            request: android.webkit.WebResourceRequest?,
                            errorResponse: android.webkit.WebResourceResponse?
                        ) {
                            /* Only handle main page errors */
                            if (request?.isForMainFrame == true)
                            {
                                lastLoadFailed = true
                                /* Show the "No internet" overlay */
                                onNetworkError()
                                /* Retry after 5 seconds */
                                view?.postDelayed({
                                    view?.loadUrl(url)
                                }, 5000)
                            }
                        }

                        /*
                         * Called when any page finishes loading - including a page
                         * that just failed to load, with the original URL still
                         * passed in (see lastLoadFailed above). Clears the error
                         * overlay only for a real, successfully-loaded dashboard page.
                         */
                        override fun onPageFinished(view: WebView?, finishedUrl: String?) {
                            /* Only clear error for real HTTP/HTTPS URLs that actually loaded */
                            if (finishedUrl != null && finishedUrl.startsWith("http") && !lastLoadFailed)
                            {
                                /* Dashboard loaded - hide the overlay */
                                onPageLoaded()
                            }
                        }
                    }
                    /* Enable JavaScript for the dashboard */
                    settings.javaScriptEnabled = true
                    /* Enable DOM storage */
                    settings.domStorageEnabled = true
                    /* Disable caching to always show fresh data */
                    settings.cacheMode = android.webkit.WebSettings.LOAD_NO_CACHE
                    /* Load the dashboard URL */
                    loadUrl(url)
                    /* Pass WebView reference back for reload on resume */
                    onWebViewCreated(this)
                }
            }
        )

        /* Debounced offline state - a spurious noInternet blip can happen right */
        /* after wakeup even when the connection is actually fine (e.g. the OS */
        /* reports connectivity a moment before it's really usable), so delay */
        /* showing the offline overlay by 500ms rather than reacting instantly. */
        /* The actual retry logic (WebViewClient's postDelayed reload) is */
        /* unaffected - this only debounces what's displayed. */
        /* Also keyed on resumeSignal (bumped every onResume): a "no internet" */
        /* state left over from before the phone slept must not just stay */
        /* visible across the wake with no debounce - it's hidden and given */
        /* the same fresh 500ms grace period as a brand new failure, while */
        /* MainActivity's onResume kicks off a retry in the background. */
        var showOfflineOverlay by remember { mutableStateOf(false) }
        LaunchedEffect(noInternet, resumeSignal) {
            if (noInternet) {
                showOfflineOverlay = false
                delay(500)
                showOfflineOverlay = true
            } else {
                showOfflineOverlay = false
            }
        }

        /* "Loading..." overlay - shown while a page load is in progress, so */
        /* the WebView's blank/dark gap during a slow load (e.g. right after */
        /* the phone wakes from idle) shows text instead. Re-armed on every */
        /* navigation (see onPageStarted), not just the very first load - the */
        /* dashboard's own 10s meta-refresh (index.php) re-navigates on its */
        /* own even while the app just sits there. That routine refresh is */
        /* normally fast enough to be invisible - delay showing the text by */
        /* 500ms so only a load that's actually slow (the wake-from-idle */
        /* case) shows it, instead of a blink on every 10s cycle. */
        var showLoadingText by remember { mutableStateOf(false) }
        LaunchedEffect(isLoading) {
            if (isLoading) {
                delay(500)
                showLoadingText = true
            } else {
                showLoadingText = false
            }
        }
        if (showLoadingText && !showOfflineOverlay) {
            Box(
                modifier = Modifier.fillMaxSize(),
                contentAlignment = Alignment.Center
            ) {
                Text(
                    "Loading...",
                    color = Color(0xFF7D8B99),
                    fontSize = 16.sp
                )
            }
        }

        /* "No internet connection" overlay - shown when network is down */
        if (showOfflineOverlay) {
            Box(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(32.dp),
                contentAlignment = Alignment.Center
            ) {
                /* Centered column with icon, title, and subtitle */
                Column(
                    horizontalAlignment = Alignment.CenterHorizontally
                ) {
                    /* Satellite dish emoji as visual indicator */
                    Text(
                        "📡",
                        fontSize = 48.sp
                    )
                    /* Spacing between icon and title */
                    Spacer(modifier = Modifier.height(16.dp))
                    /* Main error message */
                    Text(
                        "No internet connection",
                        color = Color(0xFFE6EDF3),
                        fontSize = 18.sp
                    )
                    /* Spacing between title and subtitle */
                    Spacer(modifier = Modifier.height(8.dp))
                    /* Subtitle indicating automatic retry */
                    Text(
                        "Retrying automatically...",
                        color = Color(0xFF7D8B99),
                        fontSize = 14.sp
                    )
                }
            }
        }

        /* "Add sensor" button - only shown when internet is available */
        if (!showOfflineOverlay) {
            ExtendedFloatingActionButton(
                text = { Text("Add sensor") },
                icon = { },
                /* Triggers permission check then BLE scanning */
                onClick = onAddSensor,
                modifier = Modifier
                    /* Position at bottom center */
                    .align(Alignment.BottomCenter)
                    /* Pad above system navigation bar */
                    .navigationBarsPadding()
                    /* Additional padding from edges */
                    .padding(16.dp)
            )
        }
    }
}

/*
 * BLE scan screen composable.
 * Shows discovered "Weather station" BLE devices as tappable cards.
 * Each card shows the device name, MAC address, RSSI, and a hint to tap.
 */
@Composable
fun ScanScreen(
    devices: List<BleDeviceInfo>,
    isScanning: Boolean,
    onRescan: () -> Unit,
    onBack: () -> Unit,
    onDeviceSelected: (BleDeviceInfo) -> Unit
) {
    /* Column layout with system bar padding */
    Column(
        modifier = Modifier
            .fillMaxSize()
            .statusBarsPadding()
            .navigationBarsPadding()
            .padding(16.dp)
    ) {
        /* Header row with back button and title */
        Row(
            verticalAlignment = Alignment.CenterVertically,
            modifier = Modifier.fillMaxWidth()
        ) {
            /* Back button returns to dashboard */
            TextButton(onClick = onBack) {
                Text("< Back", color = Color(0xFF4CC4D6))
            }
            Spacer(modifier = Modifier.width(8.dp))
            /* Screen title */
            Text("Searching for sensors", style = MaterialTheme.typography.titleMedium)
        }

        Spacer(modifier = Modifier.height(16.dp))

        /* Progress indicator shown during active scan */
        if (isScanning) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier.padding(bottom = 16.dp)
            ) {
                /* Teal circular spinner */
                CircularProgressIndicator(
                    color = Color(0xFF4CC4D6),
                    modifier = Modifier.padding(end = 12.dp)
                )
                /* Scanning status text */
                Text(
                    "Scanning for \"Weather station\" devices...",
                    color = Color(0xFF7D8B99),
                    fontSize = 14.sp
                )
            }
        }

        /* Empty state when scan finished with no results */
        if (devices.isEmpty() && !isScanning) {
            Text(
                "No sensors found. Make sure the device is powered on and in setup mode.",
                color = Color(0xFF7D8B99),
                fontSize = 14.sp,
                modifier = Modifier.padding(vertical = 24.dp)
            )
            /* Retry button */
            TextButton(onClick = onRescan) {
                Text("Scan again", color = Color(0xFF4CC4D6))
            }
        }

        /* Scrollable list of discovered devices */
        LazyColumn {
            items(devices) { device ->
                /* Device card - tapping initiates GATT connection */
                Card(
                    colors = CardDefaults.cardColors(containerColor = Color(0xFF161E26)),
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(vertical = 4.dp)
                        /* Tap handler for this device */
                        .clickable { onDeviceSelected(device) }
                ) {
                    Column(modifier = Modifier.padding(16.dp)) {
                        /* Device name */
                        Text(device.name, color = Color(0xFFE6EDF3), fontSize = 16.sp)
                        Spacer(modifier = Modifier.height(4.dp))
                        /* MAC address in teal monospace */
                        Text(
                            device.address,
                            color = Color(0xFF4CC4D6),
                            fontSize = 14.sp,
                            fontFamily = FontFamily.Monospace
                        )
                        Spacer(modifier = Modifier.height(2.dp))
                        /* Signal strength */
                        Text(
                            "RSSI: ${device.rssi} dBm",
                            color = Color(0xFF7D8B99),
                            fontSize = 12.sp
                        )
                        Spacer(modifier = Modifier.height(4.dp))
                        /* Hint that the card is tappable */
                        Text(
                            "Tap to configure",
                            color = Color(0xFF4CC4D6),
                            fontSize = 12.sp
                        )
                    }
                }
            }
        }

        /* "Scan again" button after scan completes with results */
        if (devices.isNotEmpty() && !isScanning) {
            Spacer(modifier = Modifier.height(12.dp))
            TextButton(onClick = onRescan) {
                Text("Scan again", color = Color(0xFF4CC4D6))
            }
        }
    }
}

/*
 * WiFi network selection screen composable.
 * Shows WiFi networks discovered by the ESP32, lets the user
 * select one, enter a password, and send credentials to the ESP32.
 */
@Composable
fun WifiSelectScreen(
    networks: List<WifiNetworkInfo>,
    isScanning: Boolean,
    isConnecting: Boolean,
    status: String,
    showPasswordDialog: Boolean,
    selectedNetwork: WifiNetworkInfo?,
    passwordText: String,
    onPasswordChanged: (String) -> Unit,
    onNetworkSelected: (WifiNetworkInfo) -> Unit,
    onPasswordSubmit: () -> Unit,
    onPasswordDismiss: () -> Unit,
    onBack: () -> Unit
) {
    /* Column layout with system bar padding */
    Column(
        modifier = Modifier
            .fillMaxSize()
            .statusBarsPadding()
            .navigationBarsPadding()
            .padding(16.dp)
    ) {
        /* Header row with back button and title */
        Row(
            verticalAlignment = Alignment.CenterVertically,
            modifier = Modifier.fillMaxWidth()
        ) {
            /* Back button returns to BLE scan screen */
            TextButton(onClick = onBack) {
                Text("< Back", color = Color(0xFF4CC4D6))
            }
            Spacer(modifier = Modifier.width(8.dp))
            /* Screen title */
            Text("Select WiFi Network", style = MaterialTheme.typography.titleMedium)
        }

        Spacer(modifier = Modifier.height(8.dp))

        /* Status message - teal for success, dim grey for others */
        if (status.isNotEmpty()) {
            Text(
                status,
                color = if (status.contains("OK") || status.contains("success"))
                    Color(0xFF4CC4D6) else Color(0xFF7D8B99),
                fontSize = 14.sp,
                modifier = Modifier.padding(bottom = 8.dp)
            )
        }

        /* Progress indicator while ESP32 is scanning WiFi */
        if (isScanning) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier.padding(bottom = 16.dp)
            ) {
                /* Teal circular spinner */
                CircularProgressIndicator(
                    color = Color(0xFF4CC4D6),
                    modifier = Modifier.padding(end = 12.dp)
                )
                /* Scanning status text */
                Text(
                    "Scanning WiFi networks from device...",
                    color = Color(0xFF7D8B99),
                    fontSize = 14.sp
                )
            }
        }

        /* Empty state when no networks found */
        if (networks.isEmpty() && !isScanning && !isConnecting) {
            Text(
                "No WiFi networks found by the device.",
                color = Color(0xFF7D8B99),
                fontSize = 14.sp,
                modifier = Modifier.padding(vertical = 24.dp)
            )
        }

        /* Scrollable list of WiFi networks */
        LazyColumn {
            items(networks) { network ->
                /* Network card - tapping selects this network */
                Card(
                    colors = CardDefaults.cardColors(containerColor = Color(0xFF161E26)),
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(vertical = 4.dp)
                        /* Tap handler for this network */
                        .clickable { onNetworkSelected(network) }
                ) {
                    /* Row for network info and lock icon */
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        modifier = Modifier
                            .padding(16.dp)
                            .fillMaxWidth()
                    ) {
                        /* Network details take remaining space */
                        Column(modifier = Modifier.weight(1f)) {
                            /* SSID name */
                            Text(
                                network.ssid,
                                color = Color(0xFFE6EDF3),
                                fontSize = 16.sp
                            )
                            Spacer(modifier = Modifier.height(2.dp))
                            /* Signal strength and security status */
                            Text(
                                "RSSI: ${network.rssi} dBm" +
                                        if (network.encrypted) "  •  Secured" else "  •  Open",
                                color = Color(0xFF7D8B99),
                                fontSize = 12.sp
                            )
                        }
                        /* Lock icon for encrypted networks */
                        if (network.encrypted) {
                            Text("🔒", fontSize = 16.sp)
                        }
                    }
                }
            }
        }
    }

    /* Password entry dialog - shown for encrypted networks */
    if (showPasswordDialog && selectedNetwork != null) {
        AlertDialog(
            /* Tapping outside dismisses the dialog */
            onDismissRequest = onPasswordDismiss,
            /* Dark background matching app theme */
            containerColor = Color(0xFF161E26),
            /* Title shows which network the password is for */
            title = {
                Text(
                    "Enter password for\n${selectedNetwork.ssid}",
                    color = Color(0xFFE6EDF3)
                )
            },
            /* Password text field with visibility toggle */
            text = {
                /* Track password visibility state */
                /* remember preserves state across recompositions */
                var passwordVisible by remember { mutableStateOf(false) }
                OutlinedTextField(
                    /* Bind to the password text state */
                    value = passwordText,
                    /* Update text when user types */
                    onValueChange = onPasswordChanged,
                    /* Label inside the text field */
                    label = { Text("WiFi Password") },
                    /* Single line only */
                    singleLine = true,
                    /* Toggle between masked dots and plain text */
                    visualTransformation = if (passwordVisible)
                        VisualTransformation.None
                    else
                        PasswordVisualTransformation(),
                    /* Password keyboard layout */
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
                    /* Eye icon to toggle visibility */
                    trailingIcon = {
                        IconButton(onClick = { passwordVisible = !passwordVisible }) {
                            /* Toggle between see and hide emoji */
                            Text(
                                if (passwordVisible) "🙈" else "👁",
                                fontSize = 18.sp
                            )
                        }
                    },
                    /* Custom colors matching the app theme */
                    colors = OutlinedTextFieldDefaults.colors(
                        focusedTextColor = Color(0xFFE6EDF3),
                        unfocusedTextColor = Color(0xFFE6EDF3),
                        focusedBorderColor = Color(0xFF4CC4D6),
                        unfocusedBorderColor = Color(0xFF7D8B99),
                        focusedLabelColor = Color(0xFF4CC4D6),
                        unfocusedLabelColor = Color(0xFF7D8B99),
                        cursorColor = Color(0xFF4CC4D6)
                    ),
                    /* Fill the dialog width */
                    modifier = Modifier.fillMaxWidth()
                )
            },
            /* Connect button sends credentials to ESP32 */
            confirmButton = {
                TextButton(onClick = onPasswordSubmit) {
                    Text("Connect", color = Color(0xFF4CC4D6))
                }
            },
            /* Cancel button closes without sending */
            dismissButton = {
                TextButton(onClick = onPasswordDismiss) {
                    Text("Cancel", color = Color(0xFF7D8B99))
                }
            }
        )
    }
}
