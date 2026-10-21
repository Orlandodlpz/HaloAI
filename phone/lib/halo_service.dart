import 'package:flutter/services.dart';

import 'work_lock.dart';

/// The Android foreground service (HaloService.kt) over the `haloai/service`
/// channel (P3). It keeps the process at foreground priority while the badge
/// is connected and holds the partial wake lock [WorkLock] asks for. It holds
/// no logic: everything still runs in this isolate.
class HaloService implements WakeLockChannel {
  static const _ch = MethodChannel('haloai/service');

  /// Starts the service. Fails (PlatformException) until Bluetooth connect
  /// permission is granted, which the connectedDevice service type requires.
  Future<void> start() => _ch.invokeMethod('start');
  Future<void> stop() => _ch.invokeMethod('stop');

  @override
  Future<void> acquire(String reason, Duration timeout) =>
      _ch.invokeMethod('acquire', {'reason': reason, 'timeoutMs': timeout.inMilliseconds});

  @override
  Future<void> release() => _ch.invokeMethod('release');

  Future<ServiceStatus> status() async {
    final m = (await _ch.invokeMapMethod<String, Object?>('status')) ?? const {};
    return ServiceStatus(
      running: m['running'] == true,
      notifications: m['notifications'] == true,
      unrestricted: m['unrestricted'] == true,
    );
  }
}

class ServiceStatus {
  const ServiceStatus({this.running = false, this.notifications = false, this.unrestricted = false});
  final bool running, notifications, unrestricted;

  @override
  String toString() => 'Service ${running ? 'running' : 'stopped'} | '
      'notifications ${notifications ? 'allowed' : 'denied'} | '
      'battery ${unrestricted ? 'unrestricted' : 'optimized'}';
}
