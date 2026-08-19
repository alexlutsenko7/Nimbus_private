/* Main package declaration for the simple_dev companion app */
package ca.alxlabs.simpledev

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
 * MainActivity - single activity for the simple_dev companion app.
 * Manages three screens, same structure as android/'s MainActivity.kt:
 *   1. Dashboard - WebView showing the reading from simple_dev/index.php
 *   2. BLE Scan - scans for "SimpleDev Station" devices
 *   3. WiFi Select - shows WiFi networks found by the ESP32 and provisions it
 *
 * Unlike android/'s MainActivity.kt, there is no account/claim step: no
 * user-credential characteristic, no dashboard login cookie to forward, no
 * CLAIM_OK/CLAIM_FAILED status - simple_dev/index.php has no accounts, so
 * once CONNECT_OK arrives the setup is already done.
 */
class MainActivity : ComponentActivity() {

    /* URL of the simple_dev dashboard - the WebView loads this to display */
    /* the latest reading posted by simple_dev.ino */
    private val dashboardUrl = "https://alxlabs.ca/books/env_sensor/web/simple_dev/index.php"

    /* Reference to the WebView instance so we can reload it from onResume */
    private var dashboardWebView: WebView? = null

    /* The BLE device name simple_dev.ino advertises - distinct from */
    /* wifi_bt.ino's "Weather station" so the two setups can't find each */
    /* other's device */
    private val targetDeviceName = "SimpleDev Station"

    /* How long to scan for BLE devices before stopping (milliseconds) */
    private val scanDurationMs = 10000L

    /* BLE GATT UUIDs - must match simple_dev.ino's SERVICE_UUID/CHAR_*_UUID */
    /* (leading digit 2, vs. wifi_bt.ino's 1, so the two never cross-match) */
    private val serviceUuid = UUID.fromString("22345678-1234-1234-1234-123456789ABC")
    private val commandUuid = UUID.fromString("22345678-1234-1234-1234-123456789ABD")
    private val wifiListUuid = UUID.fromString("22345678-1234-1234-1234-123456789ABE")
    private val credentialUuid = UUID.fromString("22345678-1234-1234-1234-123456789ABF")
    private val statusUuid = UUID.fromString("22345678-1234-1234-1234-123456789AC0")
    /* Client Characteristic Configuration Descriptor UUID - standard BLE descriptor for enabling notifications */
    private val cccdUuid = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

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
    /* terminal status (CONNECT_OK/CONNECT_FAIL/ERROR) after credentials */
    /* are sent - e.g. a BLE notification gets dropped - the UI would */
    /* otherwise sit on "Connecting to WiFi..." indefinitely. */
    private val wifiConnectHandler = android.os.Handler(android.os.Looper.getMainLooper())
    private var wifiConnectTimeoutRunnable: Runnable? = null
    private val wifiConnectTimeoutMs = 90000L

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
     */
    private val permissionLauncher =
        registerForActivityResult(
            ActivityResultContracts.RequestMultiplePermissions()
        ) { result ->
            val allGranted = result.values.all { it }
            if (allGranted) {
                currentScreen.value = "scan"
                startBleScan()
            } else {
                Toast.makeText(
                    this,
                    "Wi-Fi and Bluetooth permissions are needed to add a sensor",
                    Toast.LENGTH_LONG
                ).show()
            }
        }

    /*
     * Called when the activity resumes from background - see android/'s
     * MainActivity.kt for the full reasoning.
     */
    override fun onResume() {
        super.onResume()
        resumeSignal.value++
        if (currentScreen.value == "dashboard" && showNoInternet.value) {
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

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            /* Same dark color scheme as android/'s MainActivity.kt, matching */
            /* the shared dashboard CSS palette */
            val simpleDevColors = darkColorScheme(
                primaryContainer = Color(0xFF1A3040),
                onPrimaryContainer = Color(0xFF4CC4D6),
                surface = Color(0xFF0E1419),
                onSurface = Color(0xFFE6EDF3)
            )
            MaterialTheme(colorScheme = simpleDevColors) {
                Surface(modifier = Modifier.fillMaxSize()) {
                    when (currentScreen.value) {
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
                        "scan" -> ScanScreen(
                            devices = foundDevices,
                            isScanning = isScanning.value,
                            onRescan = { startBleScan() },
                            onBack = {
                                stopBleScan()
                                currentScreen.value = "dashboard"
                            },
                            onDeviceSelected = { device -> connectToDevice(device) }
                        )
                        "wifi_select" -> WifiSelectScreen(
                            networks = wifiNetworks,
                            isScanning = isWifiScanning.value,
                            isConnecting = isConnecting.value,
                            status = provisioningStatus.value,
                            showPasswordDialog = showPasswordDialog.value,
                            selectedNetwork = selectedNetwork.value,
                            passwordText = passwordText,
                            onPasswordChanged = { passwordText = it },
                            onNetworkSelected = { network ->
                                selectedNetwork.value = network
                                passwordText = ""
                                if (network.encrypted) {
                                    showPasswordDialog.value = true
                                } else {
                                    sendCredentials(network.ssid, "")
                                }
                            },
                            onPasswordSubmit = {
                                showPasswordDialog.value = false
                                sendCredentials(
                                    selectedNetwork.value?.ssid ?: "",
                                    passwordText
                                )
                            },
                            onPasswordDismiss = {
                                showPasswordDialog.value = false
                            },
                            onBack = {
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
     */
    private fun requiredPermissions(): Array<String> {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            arrayOf(
                Manifest.permission.BLUETOOTH_SCAN,
                Manifest.permission.BLUETOOTH_CONNECT
            )
        } else {
            arrayOf(
                Manifest.permission.ACCESS_FINE_LOCATION
            )
        }
    }

    private fun requestSensorPermissions() {
        val needed = requiredPermissions().filter { permission ->
            ContextCompat.checkSelfPermission(this, permission) !=
                    PackageManager.PERMISSION_GRANTED
        }

        if (needed.isEmpty()) {
            currentScreen.value = "scan"
            startBleScan()
        } else {
            permissionLauncher.launch(needed.toTypedArray())
        }
    }

    @SuppressLint("MissingPermission")
    private fun startBleScan() {
        foundDevices.clear()
        isScanning.value = true

        Log.d("BLE_SCAN", "Starting scan...")

        val bluetoothManager =
            getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
        val adapter = bluetoothManager.adapter

        if (adapter == null || !adapter.isEnabled) {
            Log.e("BLE_SCAN", "Bluetooth adapter null or not enabled")
            Toast.makeText(this, "Please enable Bluetooth", Toast.LENGTH_LONG).show()
            isScanning.value = false
            return
        }

        bleScanner = adapter.bluetoothLeScanner

        if (bleScanner == null) {
            Log.e("BLE_SCAN", "BLE scanner is null")
            isScanning.value = false
            return
        }

        Log.d("BLE_SCAN", "Scanner obtained, starting scan now")
        bleScanner?.startScan(scanCallback)

        android.os.Handler(mainLooper).postDelayed({
            stopBleScan()
            Log.d("BLE_SCAN", "Scan stopped after timeout, found ${foundDevices.size} devices")
        }, scanDurationMs)
    }

    @SuppressLint("MissingPermission")
    private fun stopBleScan() {
        if (isScanning.value) {
            bleScanner?.stopScan(scanCallback)
            isScanning.value = false
        }
    }

    private val scanCallback = object : ScanCallback() {
        @SuppressLint("MissingPermission")
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val deviceName = result.device.name
            val deviceAddress = result.device.address

            Log.d("BLE_SCAN", "Found: name=$deviceName addr=$deviceAddress rssi=${result.rssi}")

            if (deviceName == null || deviceAddress == null) {
                return
            }

            if (deviceName != targetDeviceName) {
                return
            }

            val alreadyFound = foundDevices.any { it.address == deviceAddress }
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

        override fun onScanFailed(errorCode: Int) {
            Log.e("BLE_SCAN", "Scan failed with error code $errorCode")
            isScanning.value = false
        }
    }

    @SuppressLint("MissingPermission")
    private fun connectToDevice(device: BleDeviceInfo) {
        stopBleScan()
        isConnecting.value = true
        provisioningStatus.value = "Connecting..."
        currentScreen.value = "wifi_select"
        wifiNetworks.clear()

        val bluetoothManager =
            getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
        val adapter = bluetoothManager.adapter
        val bleDevice: BluetoothDevice = adapter.getRemoteDevice(device.address)

        bluetoothGatt = bleDevice.connectGatt(this, false, gattCallback)

        /* Timeout: if services not discovered in 10 seconds, force refresh */
        android.os.Handler(mainLooper).postDelayed({
            if (isConnecting.value || !isConnected.value) {
                Log.d("BLE_GATT", "Connection timeout - forcing cache refresh")
                provisioningStatus.value = "Retrying connection..."
                bluetoothGatt?.disconnect()
                bluetoothGatt?.close()
                bluetoothGatt = bleDevice.connectGatt(
                    this,
                    false,
                    gattCallback,
                    BluetoothDevice.TRANSPORT_LE
                )
            }
        }, 10000)
    }

    @SuppressLint("MissingPermission")
    private fun disconnectGatt() {
        bluetoothGatt?.disconnect()
        bluetoothGatt?.close()
        bluetoothGatt = null
        isConnected.value = false
        isConnecting.value = false
        cancelWifiConnectTimeout()
    }

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

    private fun cancelWifiConnectTimeout() {
        wifiConnectTimeoutRunnable?.let { wifiConnectHandler.removeCallbacks(it) }
        wifiConnectTimeoutRunnable = null
    }

    /*
     * Parses a single WiFi network entry from the ESP32.
     * Format: "SSID,RSSI,encrypted" - same as wifi_bt.ino's/android/'s.
     */
    private fun parseAndAddWifiNetwork(data: String) {
        val parts = data.split(",")
        if (parts.size >= 3) {
            val ssid = parts[0]
            val alreadyFound = wifiNetworks.any { it.ssid == ssid }
            if (!alreadyFound) {
                wifiNetworks.add(
                    WifiNetworkInfo(
                        ssid = ssid,
                        rssi = parts[1].toIntOrNull() ?: -99,
                        encrypted = parts[2] == "1"
                    )
                )
                wifiNetworks.sortByDescending { it.rssi }
            }
        }
    }

    /*
     * GATT callback - handles all BLE connection events and data exchange.
     * Same structure as android/'s MainActivity.kt, minus the
     * user-credential characteristic write (no accounts here).
     */
    private val gattCallback = object : BluetoothGattCallback() {

        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                Log.d("BLE_GATT", "Connected, refreshing GATT cache...")
                refreshGattCache(gatt)
                runOnUiThread {
                    isConnected.value = true
                    isConnecting.value = false
                    provisioningStatus.value = "Connected, discovering services..."
                }
                android.os.Handler(mainLooper).postDelayed({
                    gatt.discoverServices()
                }, 500)
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                Log.d("BLE_GATT", "Disconnected")
                cancelWifiConnectTimeout()
                runOnUiThread {
                    isConnected.value = false
                    isConnecting.value = false
                    provisioningStatus.value = "Disconnected"
                }
            }
        }

        @SuppressLint("MissingPermission")
        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                Log.e("BLE_GATT", "Service discovery failed with status $status")
                return
            }

            Log.d("BLE_GATT", "Services discovered")

            val service = gatt.getService(serviceUuid)
            if (service == null) {
                Log.e("BLE_GATT", "simple_dev service not found")
                runOnUiThread {
                    provisioningStatus.value = "Service not found on device"
                }
                return
            }

            val wifiListChar = service.getCharacteristic(wifiListUuid)
            if (wifiListChar != null) {
                gatt.setCharacteristicNotification(wifiListChar, true)
                val descriptor = wifiListChar.getDescriptor(cccdUuid)
                if (descriptor != null) {
                    descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                    gatt.writeDescriptor(descriptor)
                }
            }

            val statusChar = service.getCharacteristic(statusUuid)
            if (statusChar != null) {
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

            /* Send SCAN command after both subscriptions are set up */
            android.os.Handler(mainLooper).postDelayed({
                sendScanCommand()
            }, 1500)
        }

        @Deprecated("Deprecated in API 33")
        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic
        ) {
            val value = String(characteristic.value)
            Log.d("BLE_GATT", "Notification from ${characteristic.uuid}: $value")

            when (characteristic.uuid) {
                wifiListUuid -> {
                    runOnUiThread {
                        parseAndAddWifiNetwork(value)
                    }
                }
                statusUuid -> {
                    runOnUiThread {
                        handleStatusUpdate(value)
                    }
                }
            }
        }
    }

    @SuppressLint("MissingPermission")
    private fun sendScanCommand() {
        val service = bluetoothGatt?.getService(serviceUuid) ?: return
        val commandChar = service.getCharacteristic(commandUuid) ?: return

        isWifiScanning.value = true
        provisioningStatus.value = "Scanning WiFi networks..."

        commandChar.value = "SCAN".toByteArray()
        bluetoothGatt?.writeCharacteristic(commandChar)

        Log.d("BLE_GATT", "SCAN command sent")
    }

    /*
     * Sends WiFi credentials to the ESP32 via the credential characteristic.
     * Payload format: "SSID|password". Unlike android/'s MainActivity.kt,
     * there is no user-credential write ahead of this - simple_dev has no
     * accounts to claim the device into, so this goes straight out.
     */
    @SuppressLint("MissingPermission")
    private fun sendCredentials(ssid: String, password: String) {
        val service = bluetoothGatt?.getService(serviceUuid) ?: return
        val credentialChar = service.getCharacteristic(credentialUuid) ?: return

        provisioningStatus.value = "Sending credentials..."

        val payload = "$ssid|$password"
        credentialChar.value = payload.toByteArray()
        bluetoothGatt?.writeCharacteristic(credentialChar)
        startWifiConnectTimeout()

        Log.d("BLE_GATT", "Credentials sent for SSID: $ssid")
    }

    /*
     * Handles status notifications from the ESP32. Simpler than android/'s
     * MainActivity.kt: CONNECT_OK is always the final success outcome here
     * (no CLAIM_OK/CLAIM_FAILED step follows it - no accounts).
     */
    private fun handleStatusUpdate(status: String) {
        Log.d("BLE_GATT", "Status update: $status")

        when {
            status == "SCANNING" -> {
                provisioningStatus.value = "Device is scanning WiFi..."
            }
            status.startsWith("SCAN_DONE") -> {
                provisioningStatus.value = "WiFi scan complete"
                isWifiScanning.value = false
            }
            status == "CONNECTING" -> {
                provisioningStatus.value = "Connecting to WiFi..."
            }
            status == "CONNECT_OK" -> {
                cancelWifiConnectTimeout()
                provisioningStatus.value = "WiFi connected! Credentials saved."
                Toast.makeText(this, "Sensor configured successfully!", Toast.LENGTH_LONG).show()
            }
            status == "CONNECT_FAIL" -> {
                cancelWifiConnectTimeout()
                provisioningStatus.value = "WiFi connection failed. Check password."
                Toast.makeText(this, "WiFi connection failed", Toast.LENGTH_LONG).show()
            }
            status.startsWith("ERROR") -> {
                cancelWifiConnectTimeout()
                provisioningStatus.value = status
            }
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        stopBleScan()
        disconnectGatt()
    }
}

/*
 * Data class representing a discovered BLE device.
 */
data class BleDeviceInfo(
    val name: String,
    val address: String,
    val rssi: Int
)

/*
 * Data class representing a WiFi network found by the ESP32.
 */
data class WifiNetworkInfo(
    val ssid: String,
    val rssi: Int,
    val encrypted: Boolean
)

/*
 * Dashboard screen composable - displays the web dashboard in a WebView
 * with an "Add sensor" button. Shows a "No internet connection" overlay
 * when the network is unavailable. Identical to android/'s AppScreen.
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
    Box(modifier = Modifier.fillMaxSize()) {
        AndroidView(
            modifier = Modifier.fillMaxSize(),
            factory = { context ->
                WebView(context).apply {
                    setBackgroundColor(android.graphics.Color.parseColor("#0e1419"))
                    webViewClient = object : WebViewClient() {
                        private var lastLoadFailed = false

                        override fun onPageStarted(view: WebView?, startedUrl: String?, favicon: android.graphics.Bitmap?) {
                            if (startedUrl != null && startedUrl.startsWith("http")) {
                                lastLoadFailed = false
                                onLoadStarted()
                            }
                        }

                        @Deprecated("Deprecated in API 23")
                        override fun onReceivedError(
                            view: WebView?,
                            errorCode: Int,
                            description: String?,
                            failingUrl: String?
                        ) {
                            lastLoadFailed = true
                            view?.loadData(
                                "<html><body style='background:#0e1419'></body></html>",
                                "text/html",
                                "UTF-8"
                            )
                            onNetworkError()
                            view?.postDelayed({
                                view.loadUrl(url)
                            }, 5000)
                        }

                        override fun onReceivedError(
                            view: WebView?,
                            request: android.webkit.WebResourceRequest?,
                            error: android.webkit.WebResourceError?
                        ) {
                            if (request?.isForMainFrame == true) {
                                lastLoadFailed = true
                                view?.loadData(
                                    "<html><body style='background:#0e1419'></body></html>",
                                    "text/html",
                                    "UTF-8"
                                )
                                onNetworkError()
                                view?.postDelayed({
                                    view.loadUrl(url)
                                }, 5000)
                            }
                        }

                        override fun onReceivedHttpError(
                            view: WebView?,
                            request: android.webkit.WebResourceRequest?,
                            errorResponse: android.webkit.WebResourceResponse?
                        ) {
                            if (request?.isForMainFrame == true) {
                                lastLoadFailed = true
                                onNetworkError()
                                view?.postDelayed({
                                    view?.loadUrl(url)
                                }, 5000)
                            }
                        }

                        override fun onPageFinished(view: WebView?, finishedUrl: String?) {
                            if (finishedUrl != null && finishedUrl.startsWith("http") && !lastLoadFailed) {
                                onPageLoaded()
                            }
                        }
                    }
                    settings.javaScriptEnabled = true
                    settings.domStorageEnabled = true
                    settings.cacheMode = android.webkit.WebSettings.LOAD_NO_CACHE
                    loadUrl(url)
                    onWebViewCreated(this)
                }
            }
        )

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

        if (showOfflineOverlay) {
            Box(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(32.dp),
                contentAlignment = Alignment.Center
            ) {
                Column(
                    horizontalAlignment = Alignment.CenterHorizontally
                ) {
                    Text(
                        "📡",
                        fontSize = 48.sp
                    )
                    Spacer(modifier = Modifier.height(16.dp))
                    Text(
                        "No internet connection",
                        color = Color(0xFFE6EDF3),
                        fontSize = 18.sp
                    )
                    Spacer(modifier = Modifier.height(8.dp))
                    Text(
                        "Retrying automatically...",
                        color = Color(0xFF7D8B99),
                        fontSize = 14.sp
                    )
                }
            }
        }

        if (!showOfflineOverlay) {
            ExtendedFloatingActionButton(
                text = { Text("Add sensor") },
                icon = { },
                onClick = onAddSensor,
                modifier = Modifier
                    .align(Alignment.BottomCenter)
                    .navigationBarsPadding()
                    .padding(16.dp)
            )
        }
    }
}

/*
 * BLE scan screen composable - shows discovered "SimpleDev Station" BLE
 * devices as tappable cards. Identical to android/'s ScanScreen.
 */
@Composable
fun ScanScreen(
    devices: List<BleDeviceInfo>,
    isScanning: Boolean,
    onRescan: () -> Unit,
    onBack: () -> Unit,
    onDeviceSelected: (BleDeviceInfo) -> Unit
) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .statusBarsPadding()
            .navigationBarsPadding()
            .padding(16.dp)
    ) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            modifier = Modifier.fillMaxWidth()
        ) {
            TextButton(onClick = onBack) {
                Text("< Back", color = Color(0xFF4CC4D6))
            }
            Spacer(modifier = Modifier.width(8.dp))
            Text("Searching for sensors", style = MaterialTheme.typography.titleMedium)
        }

        Spacer(modifier = Modifier.height(16.dp))

        if (isScanning) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier.padding(bottom = 16.dp)
            ) {
                CircularProgressIndicator(
                    color = Color(0xFF4CC4D6),
                    modifier = Modifier.padding(end = 12.dp)
                )
                Text(
                    "Scanning for \"SimpleDev Station\" devices...",
                    color = Color(0xFF7D8B99),
                    fontSize = 14.sp
                )
            }
        }

        if (devices.isEmpty() && !isScanning) {
            Text(
                "No sensors found. Make sure the device is powered on and in setup mode.",
                color = Color(0xFF7D8B99),
                fontSize = 14.sp,
                modifier = Modifier.padding(vertical = 24.dp)
            )
            TextButton(onClick = onRescan) {
                Text("Scan again", color = Color(0xFF4CC4D6))
            }
        }

        LazyColumn {
            items(devices) { device ->
                Card(
                    colors = CardDefaults.cardColors(containerColor = Color(0xFF161E26)),
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(vertical = 4.dp)
                        .clickable { onDeviceSelected(device) }
                ) {
                    Column(modifier = Modifier.padding(16.dp)) {
                        Text(device.name, color = Color(0xFFE6EDF3), fontSize = 16.sp)
                        Spacer(modifier = Modifier.height(4.dp))
                        Text(
                            device.address,
                            color = Color(0xFF4CC4D6),
                            fontSize = 14.sp,
                            fontFamily = FontFamily.Monospace
                        )
                        Spacer(modifier = Modifier.height(2.dp))
                        Text(
                            "RSSI: ${device.rssi} dBm",
                            color = Color(0xFF7D8B99),
                            fontSize = 12.sp
                        )
                        Spacer(modifier = Modifier.height(4.dp))
                        Text(
                            "Tap to configure",
                            color = Color(0xFF4CC4D6),
                            fontSize = 12.sp
                        )
                    }
                }
            }
        }

        if (devices.isNotEmpty() && !isScanning) {
            Spacer(modifier = Modifier.height(12.dp))
            TextButton(onClick = onRescan) {
                Text("Scan again", color = Color(0xFF4CC4D6))
            }
        }
    }
}

/*
 * WiFi network selection screen composable - shows WiFi networks
 * discovered by the ESP32, lets the user select one, enter a password,
 * and send credentials to the ESP32. Identical to android/'s
 * WifiSelectScreen.
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
    Column(
        modifier = Modifier
            .fillMaxSize()
            .statusBarsPadding()
            .navigationBarsPadding()
            .padding(16.dp)
    ) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            modifier = Modifier.fillMaxWidth()
        ) {
            TextButton(onClick = onBack) {
                Text("< Back", color = Color(0xFF4CC4D6))
            }
            Spacer(modifier = Modifier.width(8.dp))
            Text("Select WiFi Network", style = MaterialTheme.typography.titleMedium)
        }

        Spacer(modifier = Modifier.height(8.dp))

        if (status.isNotEmpty()) {
            Text(
                status,
                color = if (status.contains("OK") || status.contains("success"))
                    Color(0xFF4CC4D6) else Color(0xFF7D8B99),
                fontSize = 14.sp,
                modifier = Modifier.padding(bottom = 8.dp)
            )
        }

        if (isScanning) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier.padding(bottom = 16.dp)
            ) {
                CircularProgressIndicator(
                    color = Color(0xFF4CC4D6),
                    modifier = Modifier.padding(end = 12.dp)
                )
                Text(
                    "Scanning WiFi networks from device...",
                    color = Color(0xFF7D8B99),
                    fontSize = 14.sp
                )
            }
        }

        if (networks.isEmpty() && !isScanning && !isConnecting) {
            Text(
                "No WiFi networks found by the device.",
                color = Color(0xFF7D8B99),
                fontSize = 14.sp,
                modifier = Modifier.padding(vertical = 24.dp)
            )
        }

        LazyColumn {
            items(networks) { network ->
                Card(
                    colors = CardDefaults.cardColors(containerColor = Color(0xFF161E26)),
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(vertical = 4.dp)
                        .clickable { onNetworkSelected(network) }
                ) {
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        modifier = Modifier
                            .padding(16.dp)
                            .fillMaxWidth()
                    ) {
                        Column(modifier = Modifier.weight(1f)) {
                            Text(
                                network.ssid,
                                color = Color(0xFFE6EDF3),
                                fontSize = 16.sp
                            )
                            Spacer(modifier = Modifier.height(2.dp))
                            Text(
                                "RSSI: ${network.rssi} dBm" +
                                        if (network.encrypted) "  •  Secured" else "  •  Open",
                                color = Color(0xFF7D8B99),
                                fontSize = 12.sp
                            )
                        }
                        if (network.encrypted) {
                            Text("🔒", fontSize = 16.sp)
                        }
                    }
                }
            }
        }
    }

    if (showPasswordDialog && selectedNetwork != null) {
        AlertDialog(
            onDismissRequest = onPasswordDismiss,
            containerColor = Color(0xFF161E26),
            title = {
                Text(
                    "Enter password for\n${selectedNetwork.ssid}",
                    color = Color(0xFFE6EDF3)
                )
            },
            text = {
                var passwordVisible by remember { mutableStateOf(false) }
                OutlinedTextField(
                    value = passwordText,
                    onValueChange = onPasswordChanged,
                    label = { Text("WiFi Password") },
                    singleLine = true,
                    visualTransformation = if (passwordVisible)
                        VisualTransformation.None
                    else
                        PasswordVisualTransformation(),
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
                    trailingIcon = {
                        IconButton(onClick = { passwordVisible = !passwordVisible }) {
                            Text(
                                if (passwordVisible) "🙈" else "👁",
                                fontSize = 18.sp
                            )
                        }
                    },
                    colors = OutlinedTextFieldDefaults.colors(
                        focusedTextColor = Color(0xFFE6EDF3),
                        unfocusedTextColor = Color(0xFFE6EDF3),
                        focusedBorderColor = Color(0xFF4CC4D6),
                        unfocusedBorderColor = Color(0xFF7D8B99),
                        focusedLabelColor = Color(0xFF4CC4D6),
                        unfocusedLabelColor = Color(0xFF7D8B99),
                        cursorColor = Color(0xFF4CC4D6)
                    ),
                    modifier = Modifier.fillMaxWidth()
                )
            },
            confirmButton = {
                TextButton(onClick = onPasswordSubmit) {
                    Text("Connect", color = Color(0xFF4CC4D6))
                }
            },
            dismissButton = {
                TextButton(onClick = onPasswordDismiss) {
                    Text("Cancel", color = Color(0xFF7D8B99))
                }
            }
        )
    }
}
