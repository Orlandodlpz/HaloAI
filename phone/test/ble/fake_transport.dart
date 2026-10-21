import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:haloai_phone/ble/ble_messages.dart';
import 'package:haloai_phone/ble/device_transport.dart';

/// BLE link without a radio: records every write and lets a test inject
/// device events.
class FakeTransport implements DeviceTransport {
  final statusC = StreamController<LinkStatus>.broadcast(sync: true);
  final audioC = StreamController<Uint8List>.broadcast(sync: true);
  final eventsC = StreamController<Uint8List>.broadcast(sync: true);
  final acks = <Map>[];
  final screens = <Uint8List>[];
  final settings = <Map>[];
  int msgId = 0;

  @override
  Stream<LinkStatus> get status => statusC.stream;
  @override
  Stream<Uint8List> get audio => audioC.stream;
  @override
  Stream<Uint8List> get events => eventsC.stream;
  @override
  int maxWrite = 244;
  @override
  Future<void> writeAudioAck(Uint8List json) async => acks.add(jsonDecode(utf8.decode(json)) as Map);
  @override
  Future<void> writeScreen(Uint8List f) async => screens.add(f);
  final wallpaper = <Uint8List>[];
  @override
  Future<void> writeWallpaper(Uint8List f) async => wallpaper.add(f);
  @override
  Future<void> writeSettings(Uint8List f) async =>
      settings.add(jsonDecode(Defragmenter().add(f)!) as Map);
  @override
  Future<void> start() async => statusC.add(const LinkStatus(LinkState.connected, mtu: 247));
  @override
  Future<void> stop() async {}

  void event(Map<String, Object?> e) {
    for (final f in fragment(e, msgId++, maxWrite)) {
      eventsC.add(f);
    }
  }
}
