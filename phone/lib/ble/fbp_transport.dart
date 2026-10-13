import 'dart:async';
import 'dart:typed_data';

import 'package:flutter_blue_plus/flutter_blue_plus.dart';

import 'ble_messages.dart';
import 'device_transport.dart';

/// [DeviceTransport] over flutter_blue_plus (BLE central). Never Bluetooth
/// Classic.
///
/// Connect sequence (BRIEFING section 2): scan for the HaloAI service UUID,
/// connect, request MTU 517 (the device prefers 256, the smaller wins), fail
/// loudly below 179 (a 176 byte chunk plus 3 bytes ATT header), request the
/// 2M PHY and high connection priority, subscribe to Audio, report connected,
/// then subscribe to Events last: the device sends `hello` once Events
/// notifications are on, so everything is ready to answer it.
///
/// flutter_blue_plus is free for personal, nonprofit and educational use
/// (License.nonprofit); for-profit use needs its commercial license.
class FbpTransport implements DeviceTransport {
  static const wantMtu = 517;
  static const minMtu = 179;

  final _status = StreamController<LinkStatus>.broadcast();
  final _audio = StreamController<Uint8List>.broadcast();
  final _events = StreamController<Uint8List>.broadcast();

  BluetoothDevice? _device;
  BluetoothCharacteristic? _ack, _screen, _settings;
  int _mtu = 23;
  bool _running = false;

  @override
  Stream<LinkStatus> get status => _status.stream;
  @override
  Stream<Uint8List> get audio => _audio.stream;
  @override
  Stream<Uint8List> get events => _events.stream;
  @override
  int get maxWrite => _mtu - 3;

  @override
  Future<void> start() async {
    if (_running) return;
    _running = true;
    unawaited(_loop());
  }

  @override
  Future<void> stop() async {
    _running = false;
    await FlutterBluePlus.stopScan();
    await _device?.disconnect();
    _emit(const LinkStatus(LinkState.off));
  }

  @override
  Future<void> writeAudioAck(Uint8List json) => _need(_ack).write(json, withoutResponse: true);
  @override
  Future<void> writeScreen(Uint8List f) => _need(_screen).write(f);
  @override
  Future<void> writeSettings(Uint8List f) => _need(_settings).write(f);

  BluetoothCharacteristic _need(BluetoothCharacteristic? c) =>
      c ?? (throw StateError('device not connected'));

  Future<void> _loop() async {
    var backoff = 1;
    var directFailures = 0;
    while (_running) {
      try {
        if (_device == null || directFailures >= 2) {
          _device = await _scan();
          directFailures = 0;
        }
        await _connect(_device!);
        backoff = 1;
        directFailures = 0;
        await _device!.connectionState.where((s) => s == BluetoothConnectionState.disconnected).first;
        _clearChars();
        if (_running) _emit(const LinkStatus(LinkState.lost, detail: 'reconnecting'));
      } on _Fatal catch (e) {
        _clearChars();
        _running = false;
        _emit(LinkStatus(LinkState.failed, detail: e.message));
        return;
      } catch (e) {
        _clearChars();
        directFailures++;
        if (!_running) return;
        _emit(LinkStatus(LinkState.lost, detail: '$e, retry in $backoff s'));
        await Future<void>.delayed(Duration(seconds: backoff));
        backoff = (backoff * 2).clamp(1, 10);
      }
    }
  }

  Future<BluetoothDevice> _scan() async {
    if (!await FlutterBluePlus.isSupported) throw _Fatal('Bluetooth LE not supported on this phone');
    _emit(const LinkStatus(LinkState.scanning, detail: 'waiting for Bluetooth'));
    await FlutterBluePlus.adapterState.where((s) => s == BluetoothAdapterState.on).first;
    _emit(const LinkStatus(LinkState.scanning));
    final found = Completer<BluetoothDevice>();
    final sub = FlutterBluePlus.onScanResults.listen((results) {
      if (results.isNotEmpty && !found.isCompleted) found.complete(results.last.device);
    });
    try {
      await FlutterBluePlus.startScan(withServices: [Guid(HaloUuids.service)], timeout: const Duration(seconds: 15));
      return await found.future.timeout(const Duration(seconds: 16),
          onTimeout: () => throw TimeoutException('no HaloAI device found'));
    } finally {
      await sub.cancel();
      await FlutterBluePlus.stopScan();
    }
  }

  Future<void> _connect(BluetoothDevice d) async {
    _emit(LinkStatus(LinkState.connecting, detail: d.remoteId.str));
    await d.connect(license: License.nonprofit, mtu: null, timeout: const Duration(seconds: 15));
    _mtu = await d.requestMtu(wantMtu);
    if (_mtu < minMtu) {
      await d.disconnect();
      throw _Fatal('negotiated MTU $_mtu is below $minMtu: a 176 byte audio chunk does not fit. '
          'The device must allow a larger MTU (section 2).');
    }
    var phy = '2M PHY requested';
    try {
      await d.setPreferredPhy(txPhy: Phy.le2m.mask, rxPhy: Phy.le2m.mask, option: PhyCoding.noPreferred);
    } catch (e) {
      phy = '2M PHY refused ($e)';
    }
    try {
      await d.requestConnectionPriority(connectionPriorityRequest: ConnectionPriority.high);
    } catch (_) {
      // Not fatal: only affects latency.
    }

    final services = await d.discoverServices();
    final svc = services.where((s) => s.uuid == Guid(HaloUuids.service)).firstOrNull ??
        (throw _Fatal('connected device has no HaloAI service'));
    BluetoothCharacteristic char(String uuid) =>
        svc.characteristics.where((c) => c.uuid == Guid(uuid)).firstOrNull ??
        (throw _Fatal('HaloAI service is missing characteristic $uuid'));
    final audio = char(HaloUuids.audio);
    final events = char(HaloUuids.events);
    _ack = char(HaloUuids.audioAck);
    _screen = char(HaloUuids.screen);
    _settings = char(HaloUuids.settings);

    final audioSub = audio.onValueReceived.listen((v) => _audio.add(Uint8List.fromList(v)));
    final eventSub = events.onValueReceived.listen((v) => _events.add(Uint8List.fromList(v)));
    d.cancelWhenDisconnected(audioSub);
    d.cancelWhenDisconnected(eventSub);
    await audio.setNotifyValue(true);
    _emit(LinkStatus(LinkState.connected, mtu: _mtu, phy: phy, detail: d.remoteId.str));
    await events.setNotifyValue(true); // device answers with hello
  }

  void _clearChars() => _ack = _screen = _settings = null;

  void _emit(LinkStatus s) => _status.add(s);
}

class _Fatal implements Exception {
  _Fatal(this.message);
  final String message;
}
