package com.haloai.haloai_phone

import io.flutter.app.FlutterApplication
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.embedding.engine.FlutterEngineCache
import io.flutter.embedding.engine.dart.DartExecutor

/**
 * P3: one Flutter engine for the whole process, created here instead of by
 * the activity. The activity only attaches to it, so closing the screen (or
 * swiping the app away) does not destroy the Dart isolate that holds the BLE
 * session while the foreground service keeps the process alive.
 */
class MainApplication : FlutterApplication() {
    override fun onCreate() {
        super.onCreate()
        val engine = FlutterEngine(this)
        engine.dartExecutor.executeDartEntrypoint(DartExecutor.DartEntrypoint.createDefault())
        FlutterEngineCache.getInstance().put(ENGINE_ID, engine)
        HaloService.register(this, engine)
    }

    companion object {
        const val ENGINE_ID = "halo"
    }
}
