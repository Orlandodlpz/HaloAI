import 'dart:async';

import 'ble/ble_messages.dart';
import 'ble/device_transport.dart';

/// What [WorkLock] drives: the native partial wake lock (HaloService.kt).
/// [acquire] replaces any lock already held, with a fresh [timeout].
abstract class WakeLockChannel {
  Future<void> acquire(String reason, Duration timeout);
  Future<void> release();
}

/// Keeps the CPU awake while a command is in flight with the phone locked
/// (P3), and lets it sleep otherwise.
///
/// Held from the device's `record_start` (or a transcript, approve, select,
/// continue, or a reconcile) until the controller shows a screen that waits
/// on the user or ends the request, or the link drops. That covers ADPCM
/// decode, Parakeet, HTTPS and status polling, and lets the phone sleep while
/// REVIEW waits for a press. `cancel` and `activity` need one short call each
/// and no screen follows them, so they take a brief lock that simply runs out.
/// The native side also ends every lock at its timeout, so a missed release
/// costs at most [work].
class WorkLock {
  WorkLock(this._channel, {void Function(String)? log, DateTime Function()? clock})
      : _log = log ?? ((_) {}),
        _clock = clock ?? DateTime.now;

  static const work = Duration(seconds: 120);
  static const brief = Duration(seconds: 15);

  /// Screens during which the phone is still working; any other screen
  /// releases the lock.
  static const busyScreens = {'PROCESSING', 'PROCESSING_SLOW', 'EXECUTING'};

  final WakeLockChannel _channel;
  final void Function(String) _log;
  final DateTime Function() _clock;

  String? _reason;
  DateTime? _since, _until;

  bool get held => _until != null && _clock().isBefore(_until!);
  String? get reason => held ? _reason : null;

  /// Last finished lock, for the status line: "held 1840 ms (record_start)".
  String? lastSummary;

  Future<void> onDeviceEvent(DeviceEvent e) {
    switch (e.type) {
      case 'record_start' || 'approve' || 'select' || 'continue':
        return _acquire(e.type, work);
      case 'cancel' || 'activity':
        return _acquire(e.type, brief);
      case 'status_query' when e.fields['pending_key'] != null:
        return _acquire('reconcile', work);
      case 'hello' when e.pendingKey != null:
        return _acquire('reconcile', work);
    }
    return Future.value();
  }

  Future<void> onTranscript() => _acquire('transcript', work);

  Future<void> onScreen(String screen) =>
      busyScreens.contains(screen) ? Future.value() : _release('screen $screen');

  Future<void> onLink(LinkState s) => s == LinkState.connected ? Future.value() : _release('link ${s.name}');

  Future<void> _acquire(String reason, Duration timeout) async {
    final now = _clock();
    // Extend, never shorten: a brief lock during a command keeps the longer one.
    final until = held && _until!.isAfter(now.add(timeout)) ? _until! : now.add(timeout);
    if (!held) _since = now;
    _reason = held ? _reason : reason;
    _until = until;
    _log('wake lock acquire ($reason) for ${until.difference(now).inSeconds} s');
    try {
      await _channel.acquire(reason, until.difference(now));
    } catch (e) {
      _log('wake lock acquire failed: $e');
    }
  }

  Future<void> _release(String why) async {
    if (!held) return;
    final ms = _clock().difference(_since!).inMilliseconds;
    lastSummary = 'held $ms ms ($_reason)';
    _log('wake lock release ($why) after $ms ms, started by $_reason');
    _until = null;
    _reason = null;
    try {
      await _channel.release();
    } catch (e) {
      _log('wake lock release failed: $e');
    }
  }
}
