package com.example.smartwatch

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.*
import android.bluetooth.le.*
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.drawable.GradientDrawable
import android.location.Location
import android.media.RingtoneManager
import android.os.*
import android.view.View
import android.widget.*
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import com.google.android.gms.location.*
import org.json.JSONObject
import java.text.SimpleDateFormat
import java.util.*

class MainActivity : AppCompatActivity() {

    // ── BLE UUIDs ─────────────────────────────────────────────────────────────
    private val SERVICE_UUID       = UUID.fromString("12345678-1234-1234-1234-123456789abc")
    private val CHAR_SENSOR_UUID   = UUID.fromString("12345678-1234-1234-1234-123456789abd")
    private val CHAR_LOCATION_UUID = UUID.fromString("12345678-1234-1234-1234-123456789abe")
    private val CCCD_UUID          = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

    // ── BLE state ─────────────────────────────────────────────────────────────
    private var bluetoothGatt: BluetoothGatt? = null
    private var locationChar: BluetoothGattCharacteristic? = null
    private var isScanning = false
    private val handler = Handler(Looper.getMainLooper())

    // ── Views ─────────────────────────────────────────────────────────────────
    private lateinit var tvBPM: TextView
    private lateinit var tvHRV: TextView
    private lateinit var tvHRVLabel: TextView
    private lateinit var tvFinger: TextView
    private lateinit var tvDrowsyPct: TextView
    private lateinit var tvDrowsyState: TextView
    private lateinit var tvTilt: TextView
    private lateinit var drowsyBar: ProgressBar
    private lateinit var tvLat: TextView
    private lateinit var tvLng: TextView
    private lateinit var tvSpeed: TextView
    private lateinit var tvGpsStatus: TextView
    private lateinit var tvCalibStatus: TextView
    private lateinit var tvCalibNote: TextView
    private lateinit var calibBar: ProgressBar
    private lateinit var tvLastAlert: TextView
    private lateinit var tvAlertCount: TextView
    private lateinit var tvBleStatus: TextView
    private lateinit var bleDot: View
    private lateinit var btnScan: Button

    // ── GPS ───────────────────────────────────────────────────────────────────
    private lateinit var fusedLocationClient: FusedLocationProviderClient
    private var lastLat   = 0.0
    private var lastLng   = 0.0
    private var lastSpeed = 0f

    // ── Alert tracking ────────────────────────────────────────────────────────
    private var alertCount    = 0
    private var alertCooldown = 0L
    private val timeFormat    = SimpleDateFormat("hh:mm:ss a", Locale.getDefault())

    // ── Permission request code ───────────────────────────────────────────────
    private val PERM_REQUEST = 100

    // ─────────────────────────────────────────────────────────────────────────
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        bindViews()
        requestAllPermissions()

        btnScan.setOnClickListener { startBleScan() }

        fusedLocationClient = LocationServices.getFusedLocationProviderClient(this)
        startLocationUpdates()
    }

    // ── Bind all views ────────────────────────────────────────────────────────
    private fun bindViews() {
        tvBPM         = findViewById(R.id.tvBPM)
        tvHRV         = findViewById(R.id.tvHRV)
        tvHRVLabel    = findViewById(R.id.tvHRVLabel)
        tvFinger      = findViewById(R.id.tvFinger)
        tvDrowsyPct   = findViewById(R.id.tvDrowsyPct)
        tvDrowsyState = findViewById(R.id.tvDrowsyState)
        tvTilt        = findViewById(R.id.tvTilt)
        drowsyBar     = findViewById(R.id.drowsyBar)
        tvLat         = findViewById(R.id.tvLat)
        tvLng         = findViewById(R.id.tvLng)
        tvSpeed       = findViewById(R.id.tvSpeed)
        tvGpsStatus   = findViewById(R.id.tvGpsStatus)
        tvCalibStatus = findViewById(R.id.tvCalibStatus)
        tvCalibNote   = findViewById(R.id.tvCalibNote)
        calibBar      = findViewById(R.id.calibBar)
        tvLastAlert   = findViewById(R.id.tvLastAlert)
        tvAlertCount  = findViewById(R.id.tvAlertCount)
        tvBleStatus   = findViewById(R.id.tvBleStatus)
        bleDot        = findViewById(R.id.bleDot)
        btnScan       = findViewById(R.id.btnScan)
    }

    // ── Permissions ───────────────────────────────────────────────────────────
    private fun requestAllPermissions() {
        val perms = mutableListOf(
            Manifest.permission.ACCESS_FINE_LOCATION,
            Manifest.permission.ACCESS_COARSE_LOCATION
        )
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            perms += Manifest.permission.BLUETOOTH_SCAN
            perms += Manifest.permission.BLUETOOTH_CONNECT
        }
        val missing = perms.filter {
            ActivityCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isNotEmpty()) {
            ActivityCompat.requestPermissions(this, missing.toTypedArray(), PERM_REQUEST)
        }
    }

    // ── GPS ───────────────────────────────────────────────────────────────────
    @SuppressLint("MissingPermission")
    private fun startLocationUpdates() {
        val req = LocationRequest.Builder(Priority.PRIORITY_HIGH_ACCURACY, 1000)
            .setMinUpdateIntervalMillis(500)
            .build()

        fusedLocationClient.requestLocationUpdates(req, object : LocationCallback() {
            override fun onLocationResult(result: LocationResult) {
                val loc: Location = result.lastLocation ?: return
                lastLat   = loc.latitude
                lastLng   = loc.longitude
                lastSpeed = loc.speed * 3.6f  // m/s → km/h

                runOnUiThread {
                    tvLat.text      = "LAT  %.5f".format(lastLat)
                    tvLng.text      = "LNG  %.5f".format(lastLng)
                    tvSpeed.text    = "%.0f".format(lastSpeed)
                    tvGpsStatus.text = "GPS ±${loc.accuracy.toInt()}m"
                    tvGpsStatus.setTextColor(getColor(R.color.green))
                }

                sendLocationToWatch(lastLat, lastLng, lastSpeed.toDouble())
            }
        }, Looper.getMainLooper())
    }

    // ── BLE Scan ──────────────────────────────────────────────────────────────
    @SuppressLint("MissingPermission")
    private fun startBleScan() {
        if (isScanning) return
        val bm = getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
        val scanner = bm.adapter?.bluetoothLeScanner ?: run {
            Toast.makeText(this, "Bluetooth not available", Toast.LENGTH_SHORT).show()
            return
        }

        bluetoothGatt?.disconnect()
        bluetoothGatt?.close()
        bluetoothGatt = null

        isScanning = true
        btnScan.text = "SCANNING..."
        tvBleStatus.text = "Scanning for SmartWatch..."
        setDotColor(getColor(R.color.amber))

        val filter = ScanFilter.Builder()
            .setDeviceName("SmartWatch")
            .build()
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()

        scanner.startScan(listOf(filter), settings, scanCallback)

        handler.postDelayed({
            if (isScanning) {
                scanner.stopScan(scanCallback)
                isScanning = false
                if (bluetoothGatt == null) {
                    runOnUiThread {
                        btnScan.text = "SCAN"
                        tvBleStatus.text = "Not found — tap SCAN to retry"
                        setDotColor(getColor(R.color.darkgray))
                    }
                }
            }
        }, 15000)
    }

    @SuppressLint("MissingPermission")
    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val bm = getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
            bm.adapter.bluetoothLeScanner.stopScan(this)
            isScanning = false
            runOnUiThread { tvBleStatus.text = "Connecting..." }
            bluetoothGatt = result.device.connectGatt(
                this@MainActivity, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
        }

        override fun onScanFailed(errorCode: Int) {
            isScanning = false
            runOnUiThread {
                btnScan.text = "SCAN"
                tvBleStatus.text = "Scan failed (code $errorCode)"
                setDotColor(getColor(R.color.darkgray))
            }
        }
    }

    // ── GATT Callbacks ────────────────────────────────────────────────────────
    @SuppressLint("MissingPermission")
    private val gattCallback = object : BluetoothGattCallback() {

        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> {
                    runOnUiThread {
                        tvBleStatus.text = "Connected — SmartWatch"
                        setDotColor(getColor(R.color.green))
                        btnScan.text = "CONNECTED"
                    }
                    handler.postDelayed({ gatt.discoverServices() }, 600)
                }
                BluetoothProfile.STATE_DISCONNECTED -> {
                    bluetoothGatt?.close()
                    bluetoothGatt = null
                    locationChar  = null
                    runOnUiThread {
                        tvBleStatus.text = "Disconnected — tap SCAN to reconnect"
                        setDotColor(getColor(R.color.darkgray))
                        btnScan.text = "SCAN"
                        tvBPM.text = "--"
                        tvHRV.text = "--"
                        tvFinger.text = "No finger detected"
                    }
                }
            }
        }

        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) return
            val service = gatt.getService(SERVICE_UUID) ?: run {
                runOnUiThread { tvBleStatus.text = "Service not found on device" }
                return
            }

            val sensorChar = service.getCharacteristic(CHAR_SENSOR_UUID) ?: return
            gatt.setCharacteristicNotification(sensorChar, true)
            val desc = sensorChar.getDescriptor(CCCD_UUID)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                gatt.writeDescriptor(desc, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
            } else {
                @Suppress("DEPRECATION")
                desc.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                @Suppress("DEPRECATION")
                gatt.writeDescriptor(desc)
            }

            locationChar = service.getCharacteristic(CHAR_LOCATION_UUID)
            runOnUiThread { tvBleStatus.text = "Live — SmartWatch" }
        }

        @Suppress("DEPRECATION")
        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic
        ) {
            if (characteristic.uuid == CHAR_SENSOR_UUID) {
                parseSensorData(characteristic.getStringValue(0) ?: return)
            }
        }

        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray
        ) {
            if (characteristic.uuid == CHAR_SENSOR_UUID) {
                parseSensorData(String(value))
            }
        }
    }

    // ── Parse JSON from watch ─────────────────────────────────────────────────
    private fun parseSensorData(json: String) {
        try {
            val obj       = JSONObject(json)
            val bpm       = obj.getInt("b")
            val hrv       = obj.getDouble("h")
            val score     = obj.getDouble("d").toFloat()
            val tilt      = obj.getDouble("t").toFloat()
            val calibDone = obj.getInt("c") == 1
            val finger    = obj.getInt("f") == 1

            runOnUiThread {
                updateBPM(bpm, finger)
                updateHRV(hrv)
                updateDrowsiness(score, calibDone)
                updateTilt(tilt)
                updateCalibration(calibDone)
                if (score >= 70f && finger) triggerDrowsyAlert(score)
            }
        } catch (e: Exception) {
            e.printStackTrace()
        }
    }

    // ── UI update functions ───────────────────────────────────────────────────
    private fun updateBPM(bpm: Int, finger: Boolean) {
        tvBPM.text = if (bpm > 0 && finger) bpm.toString() else "--"
        tvBPM.setTextColor(
            if (bpm in 40..100) getColor(R.color.white)
            else if (bpm > 100)  getColor(R.color.crimson)
            else                 getColor(R.color.gray)
        )
        tvFinger.text = if (finger) "● Finger detected" else "○ No finger detected"
        tvFinger.setTextColor(
            if (finger) getColor(R.color.green) else getColor(R.color.gray))
    }

    private fun updateHRV(hrv: Double) {
        tvHRV.text = if (hrv > 0) "%.1f".format(hrv) else "--"
        val (label, color) = when {
            hrv <= 0   -> "No data"      to R.color.gray
            hrv < 20   -> "Very Low"     to R.color.crimson
            hrv < 30   -> "Low"          to R.color.orange
            hrv < 50   -> "Moderate"     to R.color.amber
            hrv < 100  -> "Normal"       to R.color.green
            else       -> "Excellent"    to R.color.cyan
        }
        tvHRVLabel.text = label
        tvHRVLabel.setTextColor(getColor(color))
    }

    private fun updateDrowsiness(score: Float, calibDone: Boolean) {
        val pct = score.toInt()
        tvDrowsyPct.text = "$pct%"
        drowsyBar.progress = pct

        if (!calibDone) {
            tvDrowsyState.text = "Calibrating..."
            tvDrowsyState.setTextColor(getColor(R.color.amber))
            tvDrowsyPct.setTextColor(getColor(R.color.amber))
            setProgressColor(drowsyBar, getColor(R.color.amber))
            return
        }

        val (state, color) = when {
            score >= 70 -> "⚠ DROWSY!"  to R.color.crimson
            score >= 40 -> "! ALERT"    to R.color.amber
            else        -> "✓ AWAKE"    to R.color.green
        }
        tvDrowsyState.text = state
        tvDrowsyState.setTextColor(getColor(color))
        tvDrowsyPct.setTextColor(getColor(color))
        setProgressColor(drowsyBar, getColor(color))
    }

    private fun updateTilt(tilt: Float) {
        tvTilt.text = "%.1f°".format(tilt)
        tvTilt.setTextColor(
            when {
                tilt > 35 -> getColor(R.color.crimson)
                tilt > 20 -> getColor(R.color.amber)
                else      -> getColor(R.color.cyan)
            }
        )
    }

    private fun updateCalibration(done: Boolean) {
        if (done) {
            calibBar.progress = 100
            setProgressColor(calibBar, getColor(R.color.green))
            tvCalibStatus.text = "DONE"
            tvCalibStatus.setTextColor(getColor(R.color.green))
            tvCalibNote.text = "Baseline established — drowsiness detection active"
        }
    }

    // ── Drowsy alert ─────────────────────────────────────────────────────────
    private fun triggerDrowsyAlert(score: Float) {
        val now = System.currentTimeMillis()
        if (now - alertCooldown < 15000) return
        alertCooldown = now
        alertCount++

        val time = timeFormat.format(Date(now))
        tvLastAlert.text  = "$time — Score: ${score.toInt()}%"
        tvAlertCount.text = "$alertCount alert${if (alertCount > 1) "s" else ""}"
        tvAlertCount.setTextColor(getColor(R.color.crimson))

        val vibrator = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            val vm = getSystemService(Context.VIBRATOR_MANAGER_SERVICE) as VibratorManager
            vm.defaultVibrator
        } else {
            @Suppress("DEPRECATION")
            getSystemService(Context.VIBRATOR_SERVICE) as Vibrator
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            vibrator.vibrate(VibrationEffect.createWaveform(
                longArrayOf(0, 400, 150, 400, 150, 400), -1))
        } else {
            @Suppress("DEPRECATION")
            vibrator.vibrate(longArrayOf(0, 400, 150, 400, 150, 400), -1)
        }

        try {
            val uri = RingtoneManager.getDefaultUri(RingtoneManager.TYPE_ALARM)
                ?: RingtoneManager.getDefaultUri(RingtoneManager.TYPE_NOTIFICATION)
            val ringtone = RingtoneManager.getRingtone(applicationContext, uri)
            ringtone?.play()
            handler.postDelayed({ ringtone?.stop() }, 4000)
        } catch (e: Exception) {
            e.printStackTrace()
        }

        Toast.makeText(this, "⚠ DROWSINESS DETECTED!", Toast.LENGTH_LONG).show()
    }

    // ── Send GPS to watch ─────────────────────────────────────────────────────
    @SuppressLint("MissingPermission")
    private fun sendLocationToWatch(lat: Double, lng: Double, speed: Double) {
        val gatt = bluetoothGatt ?: return
        val char = locationChar   ?: return
        val data = "%.6f,%.6f,%.1f".format(lat, lng, speed).toByteArray()

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            gatt.writeCharacteristic(char, data, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT)
        } else {
            @Suppress("DEPRECATION")
            char.value = data
            @Suppress("DEPRECATION")
            gatt.writeCharacteristic(char)
        }
    }

    // ── Helpers ───────────────────────────────────────────────────────────────
    private fun setDotColor(color: Int) {
        val drawable = bleDot.background.mutate() as? GradientDrawable
        drawable?.setColor(color)
    }

    private fun setProgressColor(bar: ProgressBar, color: Int) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            bar.progressTintList = android.content.res.ColorStateList.valueOf(color)
        }
    }

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    override fun onDestroy() {
        super.onDestroy()
        bluetoothGatt?.disconnect()
        bluetoothGatt?.close()
        bluetoothGatt = null
    }
}
