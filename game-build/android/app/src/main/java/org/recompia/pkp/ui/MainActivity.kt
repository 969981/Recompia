// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.ui

import android.os.Bundle
import android.view.KeyEvent
import android.view.MotionEvent
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.runtime.mutableStateOf

/**
 * The app UI (setup wizard, Home, settings, storage, saves, logs, about). The manifest reaches it through the activity alias
 * org.recompia.pkp.MainActivity. A gamepad works everywhere: A presses the focused control and B goes back, mapped here for
 * every pad (the system's own fallback only covers pads with a generic key layout).
 */
class MainActivity : ComponentActivity() {
    private val keyNav = mutableStateOf(false)

    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge(statusBarStyle = SystemBarStyle.dark(0), navigationBarStyle = SystemBarStyle.dark(0))
        super.onCreate(savedInstanceState)
        setContent { RecompiaApp(keyNav) }
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        val mapped = when (event.keyCode) {
            KeyEvent.KEYCODE_BUTTON_A -> KeyEvent.KEYCODE_DPAD_CENTER
            KeyEvent.KEYCODE_BUTTON_B -> KeyEvent.KEYCODE_BACK
            else -> null
        }
        if (event.keyCode != KeyEvent.KEYCODE_BACK) keyNav.value = true
        if (mapped != null) {
            return super.dispatchKeyEvent(KeyEvent(event.downTime, event.eventTime, event.action, mapped, event.repeatCount,
                event.metaState, event.deviceId, event.scanCode, event.flags, event.source))
        }
        return super.dispatchKeyEvent(event)
    }

    override fun dispatchTouchEvent(ev: MotionEvent): Boolean {
        if (ev.actionMasked == MotionEvent.ACTION_DOWN) keyNav.value = false
        return super.dispatchTouchEvent(ev)
    }
}
