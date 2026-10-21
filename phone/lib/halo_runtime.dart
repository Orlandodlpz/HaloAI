import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter_timezone/flutter_timezone.dart';
import 'package:path_provider/path_provider.dart';

import 'backend/backend_client.dart';
import 'backend/request_controller.dart';
import 'ble/device_session.dart';
import 'ble/device_transport.dart';
import 'ble/fbp_transport.dart';
import 'halo_service.dart';
import 'stt_worker.dart';
import 'wallpaper/wallpaper_sender.dart';
import 'work_lock.dart';

class Entry {
  Entry(this.label, this.r, {this.releaseToTextMs});
  final String label;
  final SttResult r;
  final int? releaseToTextMs;
}

/// Everything that must keep running with the screen off (P3): the speech
/// model, the BLE session, the request controller, the wake lock and the log
/// file. It lives for the whole process, not for a page, so closing or
/// rebuilding the UI never stops the session; only [disconnect] does.
///
/// While the badge is connected the foreground service (HaloService.kt)
/// keeps the process alive, and [lock] keeps the CPU awake while a command
/// is in flight.
class HaloRuntime {
  HaloRuntime({HaloService? service, DeviceTransport Function()? transport})
      : service = service ?? HaloService(),
        _transport = transport ?? FbpTransport.new {
    lock = WorkLock(this.service, log: (l) => record('P3LOG $l'));
  }

  static final instance = HaloRuntime();

  final HaloService service;
  late final WorkLock lock;
  final DeviceTransport Function() _transport;

  SttWorker? stt;
  String? baseDir;
  String status = 'Loading model...';
  final entries = <Entry>[];

  DeviceSession? device;
  LinkStatus link = const LinkStatus(LinkState.off);
  final deviceLog = <String>[];
  final _deviceSubs = <StreamSubscription>[];

  RequestController? ctl;
  ScreenView? view;
  final apiLog = <String>[];
  String? timezone;

  ServiceStatus serviceStatus = const ServiceStatus();

  /// `landscape` or `portrait` (P4): sent to the device in Settings and to the
  /// backend with every request. Saved on the phone.
  String orientation = 'landscape';

  /// The Google account screen's own client, on the separate test key when
  /// the build has one (BackendConfig.googleFromEnvironment), else the main one.
  BackendClient? google;

  BackendClient? get api => ctl?.api;

  /// The device says it can take a wallpaper (proposed `hello` field, P4).
  bool get deviceTakesWallpaper =>
      link.state == LinkState.connected && device?.lastHello?.fields['wallpaper'] == true;

  final _changes = StreamController<void>.broadcast();

  /// Fires whenever anything shown on the page changed.
  Stream<void> get changes => _changes.stream;
  void changed() => _changes.add(null);

  bool _started = false;
  IOSink? _file;
  String? _fileDay;

  /// Loads the model and the backend client once per process.
  Future<void> init() async {
    if (_started) return;
    _started = true;
    try {
      final saved = await _settingsFile();
      if (saved.existsSync()) {
        orientation = (jsonDecode(saved.readAsStringSync()) as Map)['orientation'] as String? ?? orientation;
      }
    } catch (e) {
      record('P4LOG settings not loaded: $e');
    }
    await _initBackend();
    await _initModel();
  }

  // ------------------------------------------------------------------ log

  /// Prints [line] for logcat and appends it with a timestamp to
  /// `<external files>/logs/halo_YYYYMMDD.log`. The P3 gate runs unplugged
  /// (USB charging keeps the phone out of Doze), so logcat cannot be the
  /// record; pull the file afterwards.
  void record(String line, {bool echo = true}) {
    // ignore: avoid_print
    if (echo) print(line);
    final dir = baseDir;
    if (dir == null) return;
    final now = DateTime.now();
    final day = '${now.year}${_two(now.month)}${_two(now.day)}';
    try {
      if (_fileDay != day) {
        _file?.close();
        Directory('$dir/logs').createSync(recursive: true);
        _file = File('$dir/logs/halo_$day.log').openWrite(mode: FileMode.append);
        _fileDay = day;
      }
      _file!.writeln('${isoWithOffset(now)} $line');
    } catch (e) {
      // ignore: avoid_print
      print('P3LOG log file failed: $e');
    }
  }

  static String _two(int v) => v.toString().padLeft(2, '0');

  // -------------------------------------------------------------- startup

  Future<void> _initBackend() async {
    final config = BackendConfig.fromEnvironment();
    if (config == null) {
      apiLog.insert(0, 'Backend off: build with --dart-define-from-file=config/backend.json');
      changed();
      return;
    }
    var tz = 'America/Chicago';
    try {
      tz = (await FlutterTimezone.getLocalTimezone()).identifier;
    } catch (e) {
      record('APILOG timezone lookup failed, using $tz: $e');
    }
    timezone = tz;
    final c = RequestController(api: BackendClient(config), timezone: tz, orientation: orientation);
    final g = BackendConfig.googleFromEnvironment();
    google = g == null ? c.api : BackendClient(g);
    c.views.listen((v) {
      view = v;
      unawaited(lock.onScreen(v.screen));
      changed();
    });
    c.log.listen((l) {
      record('APILOG $l');
      apiLog.insert(0, l);
      if (apiLog.length > 4) apiLog.removeLast();
      changed();
    });
    device?.timezone = tz;
    c.attachDevice(device);
    ctl = c;
    changed();
  }

  Future<void> _initModel() async {
    try {
      final ext = await getExternalStorageDirectory();
      baseDir = ext!.path;
      final modelDir = '$baseDir/models/parakeet-v3';
      // The app must create this folder itself: on Android 11+ a folder made by
      // `adb shell` is owned by the shell user and its files stay invisible here.
      Directory(modelDir).createSync(recursive: true);
      if (!File('$modelDir/encoder.int8.onnx').existsSync()) {
        final seen = Directory(modelDir).listSync().map((e) => e.path.split('/').last);
        status = 'Model missing. Push it to:\n$modelDir\nSeen: ${seen.join(', ')}';
        changed();
        return;
      }
      final s = await SttWorker.start(modelDir);
      record('P0RESULT load=${s.loadMs}ms threads=${SttWorker.numThreads}');
      stt = s;
      status = 'Ready. Model load ${s.loadMs} ms, ${SttWorker.numThreads} threads.';
      changed();
      await device?.phoneReadyChanged();
    } catch (e, st) {
      debugPrint('P0ERROR $e\n$st');
      status = 'Error: $e';
      changed();
    }
  }

  // ---------------------------------------------------------------- device

  Future<void> connect() async {
    if (device != null) return;
    final session = DeviceSession(
      transport: _transport(),
      sttReady: () => stt?.isAlive ?? false,
      transcribe: (samples, sr, {tag}) {
        final s = stt;
        if (s == null) throw StateError('speech model not loaded');
        return s.transcribeSamples(samples, sr, tag: tag);
      },
    );
    _deviceSubs
      ..add(session.log.listen((l) {
        record('BLELOG $l');
        final was = link.state;
        link = session.status;
        if (link.state != was) unawaited(_onLink(link.state));
        deviceLog.insert(0, l);
        if (deviceLog.length > 6) deviceLog.removeLast();
        changed();
      }))
      ..add(session.events.listen((e) => unawaited(lock.onDeviceEvent(e))))
      ..add(session.transcripts.listen((t) {
        unawaited(lock.onTranscript());
        record(t.logLine, echo: false); // DeviceSession already prints it
        entries.insert(0, Entry('device | req ${t.req} seg ${t.seg} rev ${t.rev}', t.result,
            releaseToTextMs: t.releaseToTextMs));
        status = 'Device: release to text ${t.releaseToTextMs} ms '
            '(BLE ${t.releaseToCompleteMs} ms, ${t.stats.resendRequests} resend requests)';
        changed();
      }));
    session.timezone = timezone;
    session.orientation = orientation;
    ctl?.attachDevice(session);
    device = session;
    changed();
    await session.start();
  }

  Future<void> disconnect() async {
    final session = device;
    if (session == null) return;
    for (final s in _deviceSubs) {
      await s.cancel();
    }
    _deviceSubs.clear();
    ctl?.attachDevice(null);
    await session.stop();
    device = null;
    link = const LinkStatus(LinkState.off);
    await lock.onLink(LinkState.off);
    await _stopService();
    changed();
  }

  /// The service starts on the first connect rather than on the Connect tap:
  /// Android 14+ refuses a connectedDevice service until Bluetooth connect
  /// permission is granted, and that is asked for during the first scan. A
  /// connect only happens after the user tapped Connect with the app open, so
  /// the service is still started from the foreground as Android 12+ requires.
  Future<void> _onLink(LinkState s) async {
    await lock.onLink(s);
    if (s == LinkState.connected && !serviceStatus.running) {
      try {
        await service.start();
        record('P3LOG service started');
      } catch (e) {
        record('P3LOG service start failed: $e');
      }
      await refreshServiceStatus();
    }
  }

  Future<void> _stopService() async {
    try {
      await service.stop();
      record('P3LOG service stopped');
    } catch (e) {
      record('P3LOG service stop failed: $e');
    }
    await refreshServiceStatus();
  }

  Future<void> refreshServiceStatus() async {
    try {
      serviceStatus = await service.status();
    } catch (_) {
      serviceStatus = const ServiceStatus();
    }
    changed();
  }

  // -------------------------------------------------------------- settings

  /// The phone's own settings (P4), in app private storage. A file rather
  /// than shared_preferences, whose Android plugin failed in release builds
  /// (R8 removed a method it calls; NoSuchMethodError on the Z Flip7).
  Future<File> Function() settingsFile = () async => File('${(await getApplicationSupportDirectory()).path}/settings.json');
  Future<File> _settingsFile() => settingsFile();

  Future<void> setOrientation(String o) async {
    orientation = o;
    ctl?.orientation = o;
    device?.orientation = o;
    changed();
    try {
      await (await _settingsFile()).writeAsString(jsonEncode({'orientation': o}), flush: true);
    } catch (e) {
      record('P4LOG orientation not saved: $e');
    }
    await device?.phoneReadyChanged(); // resends Settings with the new orientation
    record('P4LOG orientation $o');
  }

  /// Sends a packed 1 bit image (wallpaper/dither.dart) to the device.
  Future<void> sendWallpaper(Uint8List bits, int w, int h) async {
    final d = device;
    if (d == null || link.state != LinkState.connected) throw StateError('device not connected');
    final sw = Stopwatch()..start();
    await WallpaperSender(d.transport, d.events).send(bits, w, h);
    record('P4LOG wallpaper ${w}x$h sent and confirmed in ${sw.elapsedMilliseconds} ms');
  }

  // ----------------------------------------------------------- phone input

  /// A hold while a proposal or question is on screen adds to it, as on the
  /// device; otherwise it starts a new request.
  Future<void> sendSpoken(String text) async {
    final c = ctl;
    if (c == null) return;
    const open = {'review', 'conflict_ambiguous', 'conflict_schedule', 'processing'};
    if (open.contains(c.current?.state)) {
      await c.add(text);
    } else {
      await c.say(text);
    }
  }
}
