package org.capyps5.emulator

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.Surface
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import org.capyps5.emulator.model.GameItem
import org.capyps5.emulator.ui.screens.EmulationScreen
import org.capyps5.emulator.ui.screens.HomeScreen
import org.capyps5.emulator.ui.theme.CapyPS5Theme

class MainActivity : ComponentActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        // Configure immersive fullscreen landscape mode
        WindowCompat.setDecorFitsSystemWindows(window, false)
        val insetsController = WindowCompat.getInsetsController(window, window.decorView)
        insetsController.systemBarsBehavior =
            WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        insetsController.hide(WindowInsetsCompat.Type.systemBars())

        // Initialize CapyPS5 native engine
        NativeBridge.nativeInit(
            filesDir = filesDir.absolutePath,
            cacheDir = cacheDir.absolutePath
        )

        // Ensure default directories exist
        val demoDir = java.io.File(filesDir, "demos/homebrew")
        if (!demoDir.exists()) {
            demoDir.mkdirs()
        }
        val sdcardDir = java.io.File(android.os.Environment.getExternalStorageDirectory(), "CapyPS5")
        if (!sdcardDir.exists()) {
            try { sdcardDir.mkdirs() } catch (_: Exception) {}
        }

        val autoLaunchElf = intent?.getStringExtra("launch_elf")
        val autoLaunchDir = intent?.getStringExtra("launch_dir") ?: autoLaunchElf?.let { java.io.File(it).parent } ?: ""
        val initialGame = if (!autoLaunchElf.isNullOrEmpty() && java.io.File(autoLaunchElf).exists()) {
            val gameDir = java.io.File(autoLaunchDir)
            var titleId = intent?.getStringExtra("title_id") ?: gameDir.name
            var titleName = intent?.getStringExtra("title_name") ?: gameDir.name
            val paramJson = java.io.File(gameDir, "sce_sys/param.json")
            if (paramJson.exists()) {
                try {
                    val json = org.json.JSONObject(paramJson.readText())
                    titleId = json.optString("titleId", titleId)
                    val loc = json.optJSONObject("localizedParameters")
                    val defLang = loc?.optJSONObject("en-US") ?: loc?.optJSONObject("default")
                    titleName = defLang?.optString("titleName", titleName) ?: titleName
                } catch (_: Exception) {}
            }
            GameItem(
                titleId = titleId,
                titleName = titleName,
                directoryPath = autoLaunchDir,
                elfPath = autoLaunchElf
            )
        } else null

        setContent {
            CapyPS5Theme {
                Surface(modifier = Modifier.fillMaxSize()) {
                    var currentGame by remember { mutableStateOf<GameItem?>(initialGame) }
                    val games = remember {
                        val initialList = mutableStateListOf(
                            GameItem(
                                titleId = "CUSA00001",
                                titleName = "PlayStation 5 Homebrew Demo",
                                directoryPath = demoDir.absolutePath,
                                elfPath = java.io.File(demoDir, "eboot.bin").absolutePath
                            )
                        )
                        // Scan /sdcard/CapyPS5 for ELFs and game directories
                        if (sdcardDir.exists()) {
                            sdcardDir.listFiles()?.forEach { f ->
                                if (f.isFile && f.extension.lowercase() in listOf("elf", "bin", "self")) {
                                    initialList.add(
                                        GameItem(
                                            titleId = "SDCARD",
                                            titleName = f.name,
                                            directoryPath = f.parent ?: sdcardDir.absolutePath,
                                            elfPath = f.absolutePath
                                        )
                                    )
                                } else if (f.isDirectory) {
                                    val eboot = java.io.File(f, "eboot.bin")
                                    if (eboot.exists()) {
                                        var titleId = f.name
                                        var titleName = f.name
                                        val paramJson = java.io.File(f, "sce_sys/param.json")
                                        if (paramJson.exists()) {
                                            try {
                                                val json = org.json.JSONObject(paramJson.readText())
                                                titleId = json.optString("titleId", titleId)
                                                val loc = json.optJSONObject("localizedParameters")
                                                val defLang = loc?.optJSONObject("en-US") ?: loc?.optJSONObject("default")
                                                titleName = defLang?.optString("titleName", titleName) ?: titleName
                                            } catch (_: Exception) {}
                                        }
                                        initialList.add(
                                            GameItem(
                                                titleId = titleId,
                                                titleName = titleName,
                                                directoryPath = f.absolutePath,
                                                elfPath = eboot.absolutePath
                                            )
                                        )
                                    }
                                }
                            }
                        }
                        initialList
                    }

                    if (currentGame != null) {
                        EmulationScreen(
                            game = currentGame!!,
                            onExit = {
                                currentGame = null
                            }
                        )
                    } else {
                        HomeScreen(
                            games = games,
                            onGameSelected = { game ->
                                currentGame = game
                            },
                            onAddGameFile = { uriString ->
                                val localFile = if (uriString.startsWith("content://")) {
                                    copyUriToLocal(android.net.Uri.parse(uriString))
                                } else {
                                    java.io.File(uriString)
                                }
                                if (localFile != null && localFile.exists()) {
                                    val newGame = GameItem(
                                        titleId = "CUSTOM",
                                        titleName = localFile.name,
                                        directoryPath = localFile.parent ?: filesDir.absolutePath,
                                        elfPath = localFile.absolutePath
                                    )
                                    games.add(newGame)
                                    currentGame = newGame
                                }
                            }
                        )
                    }
                }
            }
        }
    }

    private fun copyUriToLocal(uri: android.net.Uri): java.io.File? {
        return try {
            var name = "eboot.bin"
            contentResolver.query(uri, null, null, null, null)?.use { cursor ->
                if (cursor.moveToFirst()) {
                    val idx = cursor.getColumnIndex(android.provider.OpenableColumns.DISPLAY_NAME)
                    if (idx != -1) {
                        cursor.getString(idx)?.let { name = it }
                    }
                }
            }
            val targetDir = java.io.File(filesDir, "imported")
            if (!targetDir.exists()) targetDir.mkdirs()
            val targetFile = java.io.File(targetDir, name)
            contentResolver.openInputStream(uri)?.use { input ->
                targetFile.outputStream().use { output ->
                    input.copyTo(output)
                }
            }
            targetFile
        } catch (e: Exception) {
            null
        }
    }

    private var physicalButtons = 0
    private var physicalLx = 0f
    private var physicalLy = 0f
    private var physicalRx = 0f
    private var physicalRy = 0f
    private var physicalL2 = 0f
    private var physicalR2 = 0f

    override fun dispatchKeyEvent(event: android.view.KeyEvent): Boolean {
        val pressed = event.action == android.view.KeyEvent.ACTION_DOWN
        val mask = when (event.keyCode) {
            android.view.KeyEvent.KEYCODE_BUTTON_A -> NativeBridge.BTN_CROSS
            android.view.KeyEvent.KEYCODE_BUTTON_B, android.view.KeyEvent.KEYCODE_BACK -> NativeBridge.BTN_CIRCLE
            android.view.KeyEvent.KEYCODE_BUTTON_X -> NativeBridge.BTN_SQUARE
            android.view.KeyEvent.KEYCODE_BUTTON_Y -> NativeBridge.BTN_TRIANGLE
            android.view.KeyEvent.KEYCODE_BUTTON_L1 -> NativeBridge.BTN_L1
            android.view.KeyEvent.KEYCODE_BUTTON_R1 -> NativeBridge.BTN_R1
            android.view.KeyEvent.KEYCODE_BUTTON_L2 -> NativeBridge.BTN_L2
            android.view.KeyEvent.KEYCODE_BUTTON_R2 -> NativeBridge.BTN_R2
            android.view.KeyEvent.KEYCODE_BUTTON_THUMBL -> NativeBridge.BTN_L3
            android.view.KeyEvent.KEYCODE_BUTTON_THUMBR -> NativeBridge.BTN_R3
            android.view.KeyEvent.KEYCODE_BUTTON_START -> NativeBridge.BTN_START
            android.view.KeyEvent.KEYCODE_BUTTON_SELECT -> NativeBridge.BTN_SELECT
            android.view.KeyEvent.KEYCODE_DPAD_UP -> NativeBridge.BTN_UP
            android.view.KeyEvent.KEYCODE_DPAD_DOWN -> NativeBridge.BTN_DOWN
            android.view.KeyEvent.KEYCODE_DPAD_LEFT -> NativeBridge.BTN_LEFT
            android.view.KeyEvent.KEYCODE_DPAD_RIGHT -> NativeBridge.BTN_RIGHT
            else -> 0
        }
        if (mask != 0) {
            physicalButtons = if (pressed) physicalButtons or mask else physicalButtons and mask.inv()
            NativeBridge.nativeSendControllerInput(
                physicalButtons, physicalLx, physicalLy, physicalRx, physicalRy, physicalL2, physicalR2
            )
            return true
        }
        return super.dispatchKeyEvent(event)
    }

    override fun dispatchGenericMotionEvent(event: android.view.MotionEvent): Boolean {
        if (event.isFromSource(android.view.InputDevice.SOURCE_CLASS_JOYSTICK)) {
            physicalLx = event.getAxisValue(android.view.MotionEvent.AXIS_X)
            physicalLy = event.getAxisValue(android.view.MotionEvent.AXIS_Y)
            physicalRx = event.getAxisValue(android.view.MotionEvent.AXIS_Z)
            physicalRy = event.getAxisValue(android.view.MotionEvent.AXIS_RZ)
            val l2Axis = maxOf(event.getAxisValue(android.view.MotionEvent.AXIS_LTRIGGER), event.getAxisValue(android.view.MotionEvent.AXIS_BRAKE))
            val r2Axis = maxOf(event.getAxisValue(android.view.MotionEvent.AXIS_RTRIGGER), event.getAxisValue(android.view.MotionEvent.AXIS_GAS))
            if (l2Axis > 0.1f) physicalButtons = physicalButtons or NativeBridge.BTN_L2
            else physicalButtons = physicalButtons and NativeBridge.BTN_L2.inv()
            if (r2Axis > 0.1f) physicalButtons = physicalButtons or NativeBridge.BTN_R2
            else physicalButtons = physicalButtons and NativeBridge.BTN_R2.inv()

            val hatX = event.getAxisValue(android.view.MotionEvent.AXIS_HAT_X)
            val hatY = event.getAxisValue(android.view.MotionEvent.AXIS_HAT_Y)
            if (hatX < -0.5f) physicalButtons = physicalButtons or NativeBridge.BTN_LEFT
            else physicalButtons = physicalButtons and NativeBridge.BTN_LEFT.inv()
            if (hatX > 0.5f) physicalButtons = physicalButtons or NativeBridge.BTN_RIGHT
            else physicalButtons = physicalButtons and NativeBridge.BTN_RIGHT.inv()
            if (hatY < -0.5f) physicalButtons = physicalButtons or NativeBridge.BTN_UP
            else physicalButtons = physicalButtons and NativeBridge.BTN_UP.inv()
            if (hatY > 0.5f) physicalButtons = physicalButtons or NativeBridge.BTN_DOWN
            else physicalButtons = physicalButtons and NativeBridge.BTN_DOWN.inv()

            physicalL2 = l2Axis
            physicalR2 = r2Axis
            NativeBridge.nativeSendControllerInput(
                physicalButtons, physicalLx, physicalLy, physicalRx, physicalRy, physicalL2, physicalR2
            )
            return true
        }
        return super.dispatchGenericMotionEvent(event)
    }
}
