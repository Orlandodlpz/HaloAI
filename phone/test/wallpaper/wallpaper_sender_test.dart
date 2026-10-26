import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:haloai_phone/ble/ble_messages.dart';
import 'package:haloai_phone/ble/crc16.dart';
import 'package:haloai_phone/wallpaper/wallpaper_sender.dart';

import '../ble/fake_transport.dart';

void main() {
  late FakeTransport t;
  late StreamController<DeviceEvent> events;
  final bits = Uint8List.fromList(List.generate(48000, (i) => (i * 7) & 0xFF));

  setUp(() {
    t = FakeTransport()..maxWrite = 253; // MTU 256
    events = StreamController<DeviceEvent>.broadcast();
  });

  List<Map<String, Object?>> messages() =>
      [for (final f in t.wallpaper) (jsonDecode(Defragmenter().add(f)!) as Map).cast<String, Object?>()];

  /// Answers like the proposed device: [answers] in order, one per pass.
  void device(List<Map<String, Object?>> answers) {
    var seen = 0, pass = 0;
    Timer.periodic(const Duration(milliseconds: 1), (timer) {
      final msgs = messages();
      if (msgs.length == seen || msgs.last['crc'] == null) return;
      seen = msgs.length;
      events.add(DeviceEvent('wallpaper', {'type': 'wallpaper', 'wp': msgs.last['wp'], ...answers[pass++]}));
      if (pass == answers.length) timer.cancel();
    });
  }

  test('every byte goes once, in order, each write fits the MTU, and the last carries the CRC', () async {
    device([{'ok': true}]);
    await WallpaperSender(t, events.stream).send(bits, 800, 480);
    expect(t.wallpaper.every((f) => f.length <= t.maxWrite), isTrue);
    final msgs = messages();
    expect(t.wallpaper.length, msgs.length, reason: 'one write per chunk');
    final joined = BytesBuilder();
    var expectOff = 0;
    for (final m in msgs) {
      expect(m['off'], expectOff);
      final part = base64Decode(m['data'] as String);
      joined.add(part);
      expectOff += part.length;
      expect((m['w'], m['h']), (800, 480));
    }
    expect(joined.takeBytes(), bits);
    expect(msgs.last['crc'], crc16CcittFalse([bits]));
    expect(msgs.where((m) => m['crc'] != null), hasLength(1));
  });

  test('resend_from resends from that chunk to the end', () async {
    device([{'ok': false, 'resend_from': 30000}, {'ok': true}]);
    await WallpaperSender(t, events.stream).send(bits, 800, 480);
    final msgs = messages();
    final per = WallpaperSender.bytesPerWrite(t.maxWrite);
    final firstPass = (48000 + per - 1) ~/ per;
    final resent = msgs.sublist(firstPass);
    expect(resent.first['off'], 30000 - 30000 % per);
    expect(resent.last['crc'], isNotNull);
  });

  test('the answer timeout starts after the last chunk, not before the writes', () async {
    final slow = _SlowTransport()..maxWrite = 253;
    var seen = 0;
    Timer.periodic(const Duration(milliseconds: 1), (timer) {
      if (slow.wallpaper.length == seen) return;
      seen = slow.wallpaper.length;
      final m = jsonDecode(Defragmenter().add(slow.wallpaper.last)!) as Map;
      if (m['crc'] != null) {
        timer.cancel();
        events.add(DeviceEvent('wallpaper', {'type': 'wallpaper', 'wp': m['wp'], 'ok': true}));
      }
    });
    // 341 writes of 1 ms each take far longer than the 50 ms answer timeout.
    await WallpaperSender(slow, events.stream, answerTimeout: const Duration(milliseconds: 50)).send(bits, 800, 480);
  });

  test('no answer is an error, not a silent success', () async {
    await expectLater(
        WallpaperSender(t, events.stream, answerTimeout: const Duration(milliseconds: 50)).send(bits, 800, 480),
        throwsA(isA<TimeoutException>()));
  });

  test('a wrong size is refused before anything is sent', () async {
    await expectLater(WallpaperSender(t, events.stream).send(Uint8List(100), 800, 480), throwsArgumentError);
    expect(t.wallpaper, isEmpty);
  });
}

class _SlowTransport extends FakeTransport {
  @override
  Future<void> writeWallpaper(Uint8List f) async {
    await Future<void>.delayed(const Duration(milliseconds: 1));
    wallpaper.add(f);
  }
}
