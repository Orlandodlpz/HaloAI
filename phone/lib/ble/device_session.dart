import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import '../stt_worker.dart';
import 'ble_messages.dart';
import 'device_transport.dart';
import 'id_map.dart';
import 'segment_assembler.dart';

/// Transcribes finished audio. In the app this is `SttWorker.transcribeSamples`.
typedef Transcribe = Future<SttResult> Function(Float32List samples, int sampleRate, {Object? tag});

/// One transcribed voice segment from the device.
class DeviceTranscript {
  DeviceTranscript({
    required this.req,
    required this.seg,
    required this.rev,
    required this.requestId,
    required this.segmentId,
    required this.result,
    required this.stats,
    this.releaseToCompleteMs,
    this.releaseToTextMs,
  });
  final int req, seg, rev;
  final String requestId, segmentId;
  final SttResult result;
  final SegmentStats stats;

  /// BLE part of the latency budget: release (`record_end`) to every chunk
  /// and the final marker stored. Null if `record_end` never arrived.
  final int? releaseToCompleteMs;

  /// Release to text on the phone (BLE plus Parakeet).
  final int? releaseToTextMs;

  String get logLine => 'BLERESULT req=$req seg=$seg rev=$rev audio=${result.audioSec.toStringAsFixed(2)}s '
      'release_to_complete=${releaseToCompleteMs}ms decode=${result.decodeMs}ms '
      'release_to_text=${releaseToTextMs}ms $stats text="${result.text}"';
}

/// P1: the phone side of the device link. Receives audio and events,
/// acknowledges audio, transcribes finished segments, keeps the ID maps,
/// and tells the device whether the phone's capture service is ready.
///
/// Forwarding events to the backend is P2; here they are mapped (sid to
/// revision and proposal hash, index to candidate ID) and reported.
class DeviceSession {
  DeviceSession({
    required this.transport,
    required this.transcribe,
    required this.sttReady,
    int Function()? nowMs,
    DateTime Function()? clock,
  })  : _nowMs = nowMs ?? _monotonicMs,
        _clock = clock ?? DateTime.now;

  final DeviceTransport transport;
  final Transcribe transcribe;
  final bool Function() sttReady;
  final int Function() _nowMs;
  final DateTime Function() _clock;

  final ids = IdMap();
  late AudioReceiver _receiver = _newReceiver();
  final _defrag = Defragmenter();
  final _cancelled = <int>{};
  final _releaseMs = <(int, int), int>{};
  final _subs = <StreamSubscription>[];
  Timer? _ticker;
  int _messageId = 0;

  final _transcripts = StreamController<DeviceTranscript>.broadcast();
  final _events = StreamController<DeviceEvent>.broadcast();
  final _log = StreamController<String>.broadcast();

  Stream<DeviceTranscript> get transcripts => _transcripts.stream;
  Stream<DeviceEvent> get events => _events.stream;

  /// Human readable log lines for the test screen.
  Stream<String> get log => _log.stream;

  LinkStatus status = const LinkStatus(LinkState.off);
  DeviceEvent? lastHello;
  int get rejectedChunks => _receiver.rejectedChunks;

  static final _clockStart = Stopwatch()..start();
  static int _monotonicMs() => _clockStart.elapsedMilliseconds;

  Future<void> start() async {
    _subs
      ..add(transport.status.listen(_onStatus))
      ..add(transport.audio.listen((b) => _receiver.onNotification(b, _nowMs())))
      ..add(transport.events.listen(_onEventFragment));
    _ticker = Timer.periodic(const Duration(milliseconds: 50), (_) => _receiver.tick(_nowMs()));
    await transport.start();
  }

  Future<void> stop() async {
    _ticker?.cancel();
    for (final s in _subs) {
      await s.cancel();
    }
    _subs.clear();
    await transport.stop();
  }

  /// Call when the speech model's readiness changes (loaded, crashed).
  Future<void> phoneReadyChanged() => _sendSettings();

  /// Writes a Screen and returns its context (sid binds later events to it).
  Future<ScreenContext> sendScreen(String screen,
      {String? requestId,
      int? revision,
      String? proposalHash,
      List<String> screenLines = const [],
      String? statusLine,
      List<String> candidateLabels = const [],
      List<String> candidateIds = const [],
      int? expiresInS}) async {
    final ctx = ids.newScreen(
        requestId: requestId, revision: revision, proposalHash: proposalHash, candidateIds: candidateIds);
    final msg = ScreenMessage(
        sid: ctx.sid,
        screen: screen,
        rev: revision,
        screenLines: screenLines,
        statusLine: statusLine,
        candidates: candidateLabels,
        expiresInS: expiresInS);
    for (final f in fragment(msg.toJson(), _messageId++ & 0xFF, transport.maxWrite)) {
      await transport.writeScreen(f);
    }
    _say('screen sid=${ctx.sid} $screen');
    return ctx;
  }

  AudioReceiver _newReceiver() => AudioReceiver(
        onAck: (ack) => _write(() => transport.writeAudioAck(Uint8List.fromList(utf8.encode(jsonEncode(ack.toJson()))))),
        onSegment: _onSegment,
        onError: (e) => _say('audio: $e'),
      );

  void _onStatus(LinkStatus s) {
    status = s;
    _say('link: $s');
  }

  void _onEventFragment(Uint8List bytes) {
    final String? json;
    try {
      json = _defrag.add(bytes);
    } on FormatException catch (e) {
      _say('event: $e');
      return;
    }
    if (json == null) return;
    final DeviceEvent e;
    try {
      e = DeviceEvent.parse(json);
    } on FormatException catch (err) {
      _say('event: $err');
      return;
    }
    _onEvent(e);
  }

  void _onEvent(DeviceEvent e) {
    switch (e.type) {
      case 'hello':
        lastHello = e;
        final boot = e.boot;
        if (boot != null && ids.onHello(boot)) {
          // Device restarted: its counters start over, old segments are meaningless.
          _receiver = _newReceiver();
          _cancelled.clear();
          _releaseMs.clear();
          _say('hello: new boot $boot (state ${e.fields['state']}, fw ${e.fields['fw']})');
        } else {
          _say('hello: reconnect, same boot $boot (state ${e.fields['state']})');
        }
        if (e.pendingKey != null) _say('hello: pending execution ${e.pendingKey} (reconcile in P2)');
        _sendSettings();
      case 'record_start':
        _cancelled.remove(e.req);
        _say('record_start req=${e.req} seg=${e.seg} addition=${e.fields['addition']}');
      case 'record_end':
        if (e.req != null && e.seg != null) _releaseMs[(e.req!, e.seg!)] = _nowMs();
        _say('record_end req=${e.req} seg=${e.seg} chunks=${e.fields['chunks']}');
      case 'cancel':
        if (e.req != null) {
          _cancelled.add(e.req!);
          _receiver.dropRequest(e.req!);
        }
        _say('cancel req=${e.req}');
      case 'select':
        final s = e.sid == null ? null : ids.screen(e.sid!);
        final cand = e.sid == null || e.index == null ? null : ids.candidateId(e.sid!, e.index!);
        _say('select sid=${e.sid} index=${e.index} -> candidate $cand, rev ${s?.revision} (backend in P2)');
      case 'approve' || 'continue' || 'activity' || 'ack' || 'dismiss':
        final s = e.sid == null ? null : ids.screen(e.sid!);
        _say('${e.type} sid=${e.sid} -> request ${s?.requestId} rev ${s?.revision} '
            'hash ${s?.proposalHash}${s == null && e.sid != null ? ' (unknown sid)' : ''} (backend in P2)');
      default:
        _say('${e.type} ${e.fields}');
    }
    _events.add(e);
  }

  Future<void> _onSegment(SegmentAudio a) async {
    if (_cancelled.contains(a.req)) {
      _say('segment req=${a.req} seg=${a.seg} dropped: cancelled');
      return;
    }
    final release = _releaseMs.remove((a.req, a.seg));
    final completeMs = a.stats.completeMs!;
    final SttResult r;
    try {
      r = await transcribe(a.toFloat(), 16000, tag: (a.req, a.seg));
    } catch (e) {
      _say('transcribe req=${a.req} seg=${a.seg} failed: $e');
      return;
    }
    if (_cancelled.contains(a.req)) {
      _say('transcript req=${a.req} seg=${a.seg} dropped: cancelled while decoding');
      return;
    }
    final t = DeviceTranscript(
      req: a.req,
      seg: a.seg,
      rev: a.rev,
      requestId: ids.requestId(a.req),
      segmentId: ids.segmentId(a.req, a.seg),
      result: r,
      stats: a.stats,
      releaseToCompleteMs: release == null ? null : completeMs - release,
      releaseToTextMs: release == null ? null : _nowMs() - release,
    );
    // ignore: avoid_print
    print(t.logLine);
    _transcripts.add(t);
  }

  Future<void> _sendSettings() async {
    if (status.state != LinkState.connected) return;
    // DateTime.toIso8601String has no UTC offset for local times, so the
    // time is formatted here instead of by settingsJson.
    final s = settingsJson(phoneReady: sttReady())..['time'] = isoWithOffset(_clock());
    for (final f in fragment(s, _messageId++ & 0xFF, transport.maxWrite)) {
      await _write(() => transport.writeSettings(f));
    }
    _say('settings phone_ready=${s['phone_ready']}');
  }

  Future<void> _write(Future<void> Function() w) async {
    try {
      await w();
    } catch (e) {
      _say('write failed: $e');
    }
  }

  void _say(String m) => _log.add(m);
}

/// RFC 3339 local time with its UTC offset, e.g. 2026-10-13T14:05:00-05:00.
String isoWithOffset(DateTime t) {
  final o = t.timeZoneOffset;
  final sign = o.isNegative ? '-' : '+';
  final m = o.inMinutes.abs();
  String two(int v) => v.toString().padLeft(2, '0');
  final base = t.toIso8601String().split('.').first;
  return '$base$sign${two(m ~/ 60)}:${two(m % 60)}';
}
