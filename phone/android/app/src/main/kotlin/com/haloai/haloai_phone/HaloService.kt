package com.haloai.haloai_phone

import android.Manifest
import android.app.Activity
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel
import java.lang.ref.WeakReference

/**
 * P3: keeps the process at foreground priority while the badge is connected,
 * so Android does not kill it or cut its network with the phone locked. It
 * holds no logic: the BLE session, Parakeet and the backend client keep
 * running in the Dart isolate of the cached engine (MainApplication).
 *
 * Also holds the partial wake lock that Dart's WorkLock asks for while a
 * command is in flight. Every lock has a timeout, so a missed release costs
 * at most that long.
 *
 * Channel `haloai/service`: start, stop, acquire {reason, timeoutMs},
 * release, status.
 */
class HaloService : Service() {
    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        try {
            val n = notification(this)
            if (Build.VERSION.SDK_INT >= 29) {
                startForeground(NOTIFICATION_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
            } else {
                startForeground(NOTIFICATION_ID, n)
            }
            running = true
        } catch (e: Exception) {
            // Missing permission or not allowed from the background: give up
            // quietly rather than crash the app; status() reports it stopped.
            stopSelf()
        }
        // Not sticky: after a kill, a restarted service would have no session
        // behind it, only a notification claiming one.
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        running = false
        releaseLock()
        super.onDestroy()
    }

    companion object {
        private const val CHANNEL_ID = "halo_link"
        private const val NOTIFICATION_ID = 1
        private const val NOTIFICATIONS = "android.permission.POST_NOTIFICATIONS"

        @Volatile private var running = false
        private var wakeLock: PowerManager.WakeLock? = null
        var activity: WeakReference<Activity>? = null

        fun register(app: Context, engine: FlutterEngine) {
            MethodChannel(engine.dartExecutor.binaryMessenger, "haloai/service").setMethodCallHandler { call, result ->
                try {
                    when (call.method) {
                        "start" -> {
                            if (Build.VERSION.SDK_INT >= 31 &&
                                app.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED
                            ) {
                                result.error("no_permission", "Bluetooth connect permission not granted", null)
                                return@setMethodCallHandler
                            }
                            askNotifications(app)
                            val i = Intent(app, HaloService::class.java)
                            if (Build.VERSION.SDK_INT >= 26) app.startForegroundService(i) else app.startService(i)
                            result.success(null)
                        }
                        "stop" -> {
                            app.stopService(Intent(app, HaloService::class.java))
                            running = false
                            releaseLock()
                            result.success(null)
                        }
                        "acquire" -> {
                            val ms = (call.argument<Number>("timeoutMs") ?: 120000).toLong()
                            acquireLock(app, ms)
                            result.success(null)
                        }
                        "release" -> {
                            releaseLock()
                            result.success(null)
                        }
                        "status" -> {
                            val pm = app.getSystemService(Context.POWER_SERVICE) as PowerManager
                            val nm = app.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
                            result.success(
                                mapOf(
                                    "running" to running,
                                    "notifications" to nm.areNotificationsEnabled(),
                                    "unrestricted" to pm.isIgnoringBatteryOptimizations(app.packageName),
                                    "lockHeld" to (wakeLock?.isHeld == true),
                                )
                            )
                        }
                        else -> result.notImplemented()
                    }
                } catch (e: Exception) {
                    result.error("failed", e.toString(), null)
                }
            }
        }

        /** Replaces any lock already held, so the newest timeout wins. */
        @Synchronized
        private fun acquireLock(app: Context, timeoutMs: Long) {
            releaseLock()
            val pm = app.getSystemService(Context.POWER_SERVICE) as PowerManager
            wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "haloai:work").apply {
                setReferenceCounted(false)
                acquire(timeoutMs)
            }
        }

        @Synchronized
        private fun releaseLock() {
            wakeLock?.let { if (it.isHeld) it.release() }
            wakeLock = null
        }

        /** Android 13+: without this the service still runs, but its notification is hidden. */
        private fun askNotifications(app: Context) {
            if (Build.VERSION.SDK_INT < 33) return
            if (app.checkSelfPermission(NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED) return
            activity?.get()?.requestPermissions(arrayOf(NOTIFICATIONS), 1)
        }

        private fun notification(ctx: Context): Notification {
            val open = PendingIntent.getActivity(
                ctx, 0, Intent(ctx, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE
            )
            val b = if (Build.VERSION.SDK_INT >= 26) {
                val nm = ctx.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
                nm.createNotificationChannel(
                    NotificationChannel(CHANNEL_ID, "Badge connection", NotificationManager.IMPORTANCE_LOW)
                )
                Notification.Builder(ctx, CHANNEL_ID)
            } else {
                @Suppress("DEPRECATION") Notification.Builder(ctx)
            }
            return b.setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
                .setContentTitle("HaloAI")
                .setContentText("Connected to your badge")
                .setContentIntent(open)
                .setOngoing(true)
                .build()
        }
    }
}
