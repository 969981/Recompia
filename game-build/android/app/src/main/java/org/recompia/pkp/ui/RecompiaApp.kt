// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.ui

import android.app.Activity
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.Surface
import androidx.compose.runtime.Composable
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.compose.LifecycleEventEffect
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.navigation.NavHostController
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import org.recompia.pkp.GameLauncher

object Routes {
    const val SETUP = "setup"
    const val HOME = "home"
    const val BUILD = "build"
    const val SETTINGS = "settings"
    const val STORAGE = "storage"
    const val SAVES = "saves"
    const val LOGS = "logs"
    const val ABOUT = "about"
    const val HELP = "help"
    const val COOP = "coop"
}

@Composable
fun RecompiaApp(keyNav: MutableState<Boolean>) {
    val vm: AppViewModel = viewModel()
    val ui by vm.ui.collectAsStateWithLifecycle()
    val nav = rememberNavController()
    val start = androidx.compose.runtime.remember { if (ui.setupDone) Routes.HOME else Routes.SETUP }
    LifecycleEventEffect(Lifecycle.Event.ON_RESUME) { vm.refresh() }
    RecompiaTheme {
        ProvideKeyNav(keyNav) {
            Surface(Modifier.fillMaxSize(), color = Rc.bg) {
                NavHost(
                    nav,
                    startDestination = start,
                    enterTransition = { slideInHorizontally { it / 6 } + fadeIn() },
                    exitTransition = { fadeOut() },
                    popEnterTransition = { fadeIn() },
                    popExitTransition = { slideOutHorizontally { it / 6 } + fadeOut() },
                ) {
                    composable(Routes.SETUP) {
                        SetupScreen(vm, onHelp = { nav.navigate(Routes.HELP) }, onDone = {
                            vm.finishSetup()
                            if (nav.previousBackStackEntry != null) nav.popBackStack()
                            else nav.navigate(Routes.HOME) { popUpTo(Routes.SETUP) { inclusive = true } }
                        })
                    }
                    composable(Routes.HOME) { HomeScreen(vm, nav) }
                    composable(Routes.BUILD) { BuildPage(vm, onBack = { nav.popBackStack() }, onChangeFiles = { openSetup(vm, nav, SetupStep.DUMP) }) }
                    composable(Routes.SETTINGS) { SettingsScreen(onBack = { nav.popBackStack() }) }
                    composable(Routes.STORAGE) { StorageScreen(vm, onBack = { nav.popBackStack() }) }
                    composable(Routes.SAVES) { SavesScreen(vm, onBack = { nav.popBackStack() }) }
                    composable(Routes.LOGS) { LogsScreen(onBack = { nav.popBackStack() }) }
                    composable(Routes.ABOUT) { AboutScreen(onBack = { nav.popBackStack() }) }
                    composable(Routes.HELP) { HelpScreen(onBack = { nav.popBackStack() }) }
                    composable(Routes.COOP) {
                        CoopScreen(ui.status == GameStatus.READY || ui.status == GameStatus.SHADERS_MISSING, onBack = { nav.popBackStack() })
                    }
                }
            }
        }
    }
}

/** Back into the wizard at a step (Home's "Choose game files", "Allow storage access"). */
fun openSetup(vm: AppViewModel, nav: NavHostController, step: SetupStep) {
    vm.goTo(step)
    nav.navigate(Routes.SETUP)
}

/** Starts the game (GameActivity copies a PC-built library from <dataRoot>/build first, the dev shortcut). */
fun play(activity: Activity, vm: AppViewModel) {
    vm.played()
    GameLauncher.launch(activity)
}

@Composable
fun activity(): Activity = LocalContext.current as Activity
