import 'package:flutter/widgets.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:haloai_phone/ble/ble_messages.dart';
import 'package:haloai_phone/ble/device_transport.dart';
import 'package:haloai_phone/halo_runtime.dart';
import 'package:haloai_phone/halo_service.dart';
import 'package:haloai_phone/main.dart';
import 'package:image/image.dart' as img;
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'ble/fake_transport.dart';

class FakeService extends HaloService {
  final calls = <String>[];
  bool running = false;
  @override
  Future<void> start() async {
    calls.add('start');
    running = true;
  }

  @override
  Future<void> stop() async {
    calls.add('stop');
    running = false;
  }

  @override
  Future<void> acquire(String reason, Duration timeout) async => calls.add('acquire $reason');
  @override
  Future<void> release() async => calls.add('release');
  @override
  Future<ServiceStatus> status() async => ServiceStatus(running: running);
}

class StopCountingTransport extends FakeTransport {
  int stops = 0;
  @override
  Future<void> stop() async => stops++;
}

void main() {
  test('the service starts when the link connects and stops on disconnect', () async {
    final service = FakeService();
    final transport = StopCountingTransport();
    final rt = HaloRuntime(service: service, transport: () => transport);
    await rt.connect();
    await pumpEventQueue();
    expect(rt.link.state, LinkState.connected);
    expect(service.calls, ['start']);
    expect(rt.serviceStatus.running, isTrue);

    await rt.disconnect();
    expect(service.calls.last, 'stop');
    expect(transport.stops, 1);
    expect(rt.serviceStatus.running, isFalse);
  });

  test('a reconnect does not start the service twice', () async {
    final service = FakeService();
    final transport = FakeTransport();
    final rt = HaloRuntime(service: service, transport: () => transport);
    await rt.connect();
    await pumpEventQueue();
    transport.statusC.add(const LinkStatus(LinkState.lost));
    transport.statusC.add(const LinkStatus(LinkState.connected));
    await pumpEventQueue();
    expect(service.calls.where((c) => c == 'start'), hasLength(1));
  });

  test('orientation resends Settings to the device and is saved (P4)', () async {
    final file = File('${Directory.systemTemp.createTempSync('halo').path}/settings.json');
    final transport = FakeTransport();
    final rt = HaloRuntime(service: FakeService(), transport: () => transport)..settingsFile = () async => file;
    await rt.connect();
    await pumpEventQueue();
    await rt.setOrientation('portrait');
    expect(transport.settings.last['orientation'], 'portrait');
    expect(file.readAsStringSync(), contains('"orientation":"portrait"'));
  });

  test('changing orientation resends the wallpaper for the new one (Victor, 2026-10-25)', () async {
    final dir = Directory.systemTemp.createTempSync('halo');
    final transport = FakeTransport();
    final rt = HaloRuntime(service: FakeService(), transport: () => transport)
      ..settingsFile = (() async => File('${dir.path}/settings.json'))
      ..wallpaperPace = Duration.zero
      ..wallpaperPhoto = img.encodePng(img.Image(width: 60, height: 40))
      ..wallpaperOnBadgeFor = 'landscape';
    await rt.connect();
    transport.event({'type': 'hello', 'boot': 1, 'wallpaper': true});
    await pumpEventQueue();
    // The device: confirm each finished transfer.
    final answered = <int>{};
    final timer = Timer.periodic(const Duration(milliseconds: 2), (_) {
      if (transport.wallpaper.isEmpty) return;
      final m = jsonDecode(Defragmenter().add(transport.wallpaper.last)!) as Map;
      if (m['crc'] != null && answered.add(m['wp'] as int)) {
        transport.event({'type': 'wallpaper', 'wp': m['wp'], 'ok': true});
      }
    });
    await rt.setOrientation('portrait');
    timer.cancel();
    final first = jsonDecode(Defragmenter().add(transport.wallpaper.first)!) as Map;
    expect((first['w'], first['h']), (480, 800));
    expect(rt.wallpaperOnBadgeFor, 'portrait');
    expect(rt.wallpaperStale, isFalse);
    expect(File('${dir.path}/settings.json').readAsStringSync(), contains('"wallpaper_on_badge_for":"portrait"'));
  });

  testWidgets('closing the page does not stop the session', (tester) async {
    final transport = StopCountingTransport();
    final rt = HaloRuntime(service: FakeService(), transport: () => transport);
    await tester.runAsync(() async {
      await rt.connect();
      await pumpEventQueue();
    });
    await tester.pumpWidget(HaloApp(runtime: rt));
    expect(find.text('Disconnect'), findsOneWidget);
    await tester.pumpWidget(const SizedBox()); // app disposed
    expect(transport.stops, 0);
    expect(rt.device, isNotNull);
    expect(rt.link.state, LinkState.connected);
  });
}
