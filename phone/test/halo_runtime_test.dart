import 'package:flutter/widgets.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:haloai_phone/ble/device_transport.dart';
import 'package:haloai_phone/halo_runtime.dart';
import 'package:haloai_phone/halo_service.dart';
import 'package:haloai_phone/main.dart';
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
