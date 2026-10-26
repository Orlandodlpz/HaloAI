import 'package:flutter_test/flutter_test.dart';
import 'package:haloai_phone/ble/ble_messages.dart';
import 'package:haloai_phone/ble/device_transport.dart';
import 'package:haloai_phone/work_lock.dart';

class FakeChannel implements WakeLockChannel {
  final calls = <String>[];
  @override
  Future<void> acquire(String reason, Duration timeout) async => calls.add('acquire $reason ${timeout.inSeconds}');
  @override
  Future<void> release() async => calls.add('release');
}

DeviceEvent ev(String type, [Map<String, Object?> f = const {}]) => DeviceEvent(type, {'type': type, ...f});

void main() {
  late FakeChannel ch;
  late DateTime now;
  late WorkLock lock;

  setUp(() {
    ch = FakeChannel();
    now = DateTime(2026, 10, 21, 12);
    lock = WorkLock(ch, clock: () => now);
  });

  test('a command holds the lock from record_start through PROCESSING and lets go on REVIEW', () async {
    await lock.onDeviceEvent(ev('record_start', {'req': 1, 'seg': 1}));
    expect(lock.held, isTrue);
    await lock.onTranscript();
    await lock.onScreen('PROCESSING');
    expect(lock.held, isTrue);
    now = now.add(const Duration(milliseconds: 1500));
    await lock.onScreen('REVIEW');
    expect(lock.held, isFalse);
    expect(ch.calls.last, 'release');
    expect(lock.lastSummary, 'held 1500 ms (record_start)');
  });

  test('approve is held through EXECUTING and released on the outcome', () async {
    await lock.onDeviceEvent(ev('approve', {'sid': 3}));
    await lock.onScreen('EXECUTING');
    expect(lock.held, isTrue);
    await lock.onScreen('SUCCESS');
    expect(lock.held, isFalse);
  });

  for (final screen in [
    'REVIEW', 'CONFLICT_AMBIGUOUS', 'CONFLICT_SCHEDULE', 'READ_ONLY_RESULT', //
    'SUCCESS', 'FAILURE', 'EXECUTION_UNCERTAIN', 'CANCELLED',
  ]) {
    test('never left held after $screen', () async {
      await lock.onDeviceEvent(ev('select', {'sid': 1, 'index': 0}));
      await lock.onScreen(screen);
      expect(lock.held, isFalse);
    });
  }

  test('released when the link drops', () async {
    await lock.onDeviceEvent(ev('record_start'));
    await lock.onLink(LinkState.lost);
    expect(lock.held, isFalse);
    expect(ch.calls.last, 'release');
  });

  test('a brief lock during a command extends nothing and shortens nothing', () async {
    await lock.onDeviceEvent(ev('approve'));
    now = now.add(const Duration(seconds: 10));
    await lock.onDeviceEvent(ev('activity', {'sid': 1}));
    // Still the approve's lock: 110 s left, not 15.
    expect(ch.calls.last, 'acquire activity 110');
    expect(lock.reason, 'approve');
    now = now.add(const Duration(seconds: 100));
    expect(lock.held, isTrue);
  });

  test('a brief lock on its own runs out without a release', () async {
    await lock.onDeviceEvent(ev('cancel', {'req': 1, 'reason': 'user'}));
    expect(ch.calls.single, 'acquire cancel 15');
    now = now.add(const Duration(seconds: 16));
    expect(lock.held, isFalse);
  });

  test('reconcile only when the device holds a key', () async {
    await lock.onDeviceEvent(ev('hello', {'boot': 1}));
    await lock.onDeviceEvent(ev('battery', {'battery': 50}));
    expect(ch.calls, isEmpty);
    await lock.onDeviceEvent(ev('hello', {'boot': 1, 'pending_key': 'abc'}));
    expect(ch.calls.single, 'acquire reconcile 120');
  });

  test('a release with nothing held does not call the channel', () async {
    await lock.onScreen('REVIEW');
    await lock.onLink(LinkState.off);
    expect(ch.calls, isEmpty);
  });
}
