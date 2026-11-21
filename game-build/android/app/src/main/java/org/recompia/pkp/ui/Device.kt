// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.ui

import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.input.InputManager
import android.os.BatteryManager
import android.os.PowerManager
import android.text.format.DateUtils
import android.text.format.Formatter
import android.view.InputDevice
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.LocalContext
import kotlinx.coroutines.delay

/** Names of the connected physical gamepads (USB and Bluetooth), updated as they come and go. */
@Composable
fun rememberControllers(): List<String> {
    val ctx = LocalContext.current
    val im = remember { ctx.getSystemService(InputManager::class.java) }
    fun scan() = InputDevice.getDeviceIds().toList().mapNotNull { InputDevice.getDevice(it) }
        .filter { !it.isVirtual && (it.sources and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD || it.sources and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK) }
        .map { it.name }.distinct()
    var pads by remember { mutableStateOf(scan()) }
    DisposableEffect(im) {
        val l = object : InputManager.InputDeviceListener {
            override fun onInputDeviceAdded(id: Int) { pads = scan() }
            override fun onInputDeviceRemoved(id: Int) { pads = scan() }
            override fun onInputDeviceChanged(id: Int) { pads = scan() }
        }
        im.registerInputDeviceListener(l, null)
        onDispose { im.unregisterInputDeviceListener(l) }
    }
    return pads
}

/** Battery and heat through public APIs. The heat state is Android's own thermal status (what throttles the CPU), not a temperature. */
data class Health(val batteryPct: Int, val charging: Boolean, val batteryTempC: Float?, val thermal: Int)

@Composable
fun rememberHealth(): Health? {
    val ctx = LocalContext.current
    val pm = remember { ctx.getSystemService(PowerManager::class.java) }
    var thermal by remember { mutableStateOf(pm.currentThermalStatus) }
    DisposableEffect(pm) {
        val l = PowerManager.OnThermalStatusChangedListener { thermal = it }
        pm.addThermalStatusListener(ctx.mainExecutor, l)
        onDispose { pm.removeThermalStatusListener(l) }
    }
    var battery by remember { mutableStateOf<Triple<Int, Boolean, Float?>?>(null) }
    LaunchedEffect(Unit) {
        while (true) {
            val b = ctx.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
            val level = b?.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) ?: -1
            val scale = b?.getIntExtra(BatteryManager.EXTRA_SCALE, 100) ?: 100
            val plugged = (b?.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) ?: 0) != 0
            val temp = b?.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, Int.MIN_VALUE)?.takeIf { it != Int.MIN_VALUE }?.let { it / 10f }
            battery = Triple(if (level >= 0) level * 100 / scale else -1, plugged, temp)
            delay(5000)
        }
    }
    val bt = battery ?: return null
    return Health(bt.first, bt.second, bt.third, thermal)
}

fun Context.size(bytes: Long): String = if (bytes < 0) "-" else Formatter.formatFileSize(this, bytes)

fun ago(time: Long): String = DateUtils.getRelativeTimeSpanString(time, System.currentTimeMillis(), DateUtils.MINUTE_IN_MILLIS).toString()

/** "About 25 min" from ms, the launcher's roughly() rounding. */
fun Context.roughly(ms: Long): String {
    val min = ms / 60_000.0
    return when {
        min < 1 -> getString(org.recompia.pkp.R.string.ui_eta_lt_min)
        min < 10 -> getString(org.recompia.pkp.R.string.ui_eta_min, Math.ceil(min).toInt())
        min < 55 -> getString(org.recompia.pkp.R.string.ui_eta_min, (Math.round(min / 5) * 5).toInt())
        else -> {
            val total = (Math.round(min / 10) * 10).toInt()
            if (total % 60 != 0) getString(org.recompia.pkp.R.string.ui_eta_h_min, total / 60, total % 60)
            else getString(org.recompia.pkp.R.string.ui_eta_h, total / 60)
        }
    }
}

/** Paths as players know them: /sdcard instead of /storage/emulated/0. */
fun shown(path: String) = path.replace("/storage/emulated/0", "/sdcard")

/** "1 h 12 min" / "25 min". */
fun Context.duration(ms: Long): String {
    val min = (ms / 60_000).toInt()
    return if (min >= 60) getString(org.recompia.pkp.R.string.ui_duration_h_min, min / 60, min % 60) else getString(org.recompia.pkp.R.string.ui_duration_min, min)
}
