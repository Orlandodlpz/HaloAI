package com.haloai.haloai_phone

import android.os.Bundle
import io.flutter.embedding.android.FlutterActivity
import java.lang.ref.WeakReference

/** Attaches to the engine MainApplication keeps for the whole process (P3). */
class MainActivity : FlutterActivity() {
    override fun getCachedEngineId(): String = MainApplication.ENGINE_ID

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        HaloService.activity = WeakReference(this)
    }
}
