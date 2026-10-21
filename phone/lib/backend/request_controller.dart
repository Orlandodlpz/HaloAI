import 'dart:async';

import '../ble/ble_messages.dart';
import '../ble/device_session.dart';
import '../ble/device_transport.dart';
import '../ble/id_map.dart';
import 'api.dart';
import 'backend_client.dart';

/// What the device shows (or would show) for one result: exactly the Screen
/// write, so the app can display it with or without a device connected.
class ScreenView {
  ScreenView({
    required this.screen,
    required this.requestId,
    required this.revision,
    this.lines = const [],
    this.statusLine,
    this.candidates = const [],
    this.expiresInS,
    this.pendingKey,
  });
  final String screen;
  final String requestId;
  final int revision;
  final List<String> lines;
  final String? statusLine;
  final List<String> candidates;
  final int? expiresInS;
  final String? pendingKey;
}

/// The phone between the device and the backend (P2). One current request.
///
/// Inputs: device transcripts and events, plus typed or phone mic text
/// ([say], [add]) and phone side buttons ([approve], [select],
/// [continueAnyway], [cancel]) that take the same paths as device presses.
/// Every backend result becomes one Screen write, mapped as in
/// pipeline/scripts/phone_sim.py `show()`; the phone never writes its own
/// text for a backend screen. Rules from BRIEFING section 2: results for a
/// request that is no longer current, or older than the latest `accepted`
/// revision, are dropped; approve is polled while `executing`; a saved key
/// from the device is reconciled, and an unknown key is EXECUTION_UNCERTAIN,
/// never CANCELLED.
class RequestController {
  RequestController({
    required this.api,
    required this.timezone,
    this.orientation = 'landscape',
    DateTime Function()? clock,
    Future<void> Function(Duration)? sleep,
    this.pollInterval = const Duration(seconds: 1),
    this.pollFor = const Duration(seconds: 60),
    this.touchEvery = const Duration(seconds: 10),
  })  : _clock = clock ?? DateTime.now,
        _sleep = sleep ?? Future<void>.delayed;

  final BackendClient api;
  String timezone;
  String orientation;
  final DateTime Function() _clock;
  final Future<void> Function(Duration) _sleep;
  final Duration pollInterval, pollFor, touchEvery;

  static const screens = {
    'review': 'REVIEW',
    'conflict_ambiguous': 'CONFLICT_AMBIGUOUS',
    'conflict_schedule': 'CONFLICT_SCHEDULE',
    'read_only_result': 'READ_ONLY_RESULT',
    'executing': 'EXECUTING',
    'success': 'SUCCESS',
    'failure': 'FAILURE',
    'execution_uncertain': 'EXECUTION_UNCERTAIN',
    'cancelled': 'CANCELLED',
    'processing': 'PROCESSING',
  };

  // The only text the phone writes itself, for the cases the contract gives
  // the phone (no backend answer exists). 28 characters at most: the
  // portrait budget.
  static const emptyTranscriptLines = ["Didn't catch that", 'Hold and try again'];
  static const unreachableLines = ["Can't reach HaloAI", 'Check your connection'];
  // The backend's own words for an unknown key (pipeline/scripts/phone_sim.py).
  static const uncertainLines = ['Not sure it went through', 'Check Sent or your calendar'];

  DeviceSession? _device;
  final _subs = <StreamSubscription>[];
  final _views = StreamController<ScreenView>.broadcast();
  final _log = StreamController<String>.broadcast();

  String? _requestId;
  int _acceptedRev = 0;
  BackendResult? _latest;
  String? _polling; // execution key being polled
  final _reconciling = <String>{};
  DateTime? _lastTouch;

  Stream<ScreenView> get views => _views.stream;
  Stream<String> get log => _log.stream;
  BackendResult? get current => _latest;
  String? get currentRequestId => _requestId;

  // ------------------------------------------------------------ the device

  void attachDevice(DeviceSession? device) {
    for (final s in _subs) {
      s.cancel();
    }
    _subs.clear();
    _device = device;
    if (device == null) return;
    _subs
      ..add(device.transcripts.listen(_onTranscript))
      ..add(device.events.listen(_onDeviceEvent));
  }

  Future<void> _onTranscript(DeviceTranscript t) async {
    final addition = t.requestId == _requestId && _latest != null;
    await _input(t.result.text, requestId: t.requestId, segmentId: t.segmentId, addition: addition);
  }

  Future<void> _onDeviceEvent(DeviceEvent e) async {
    final ctx = e.sid == null ? null : _device?.ids.screen(e.sid!);
    switch (e.type) {
      case 'approve':
        if (ctx?.requestId == null || ctx?.revision == null || ctx?.proposalHash == null) {
          _say('approve sid=${e.sid}: no proposal behind that screen, ignored');
          return;
        }
        await _approve(ctx!.requestId!, ctx.revision!, ctx.proposalHash!);
      case 'select':
        final cand = e.sid == null || e.index == null ? null : _device?.ids.candidateId(e.sid!, e.index!);
        if (ctx?.requestId == null || cand == null) return _say('select sid=${e.sid}: unknown candidate');
        await _select(ctx!.requestId!, ctx.revision!, cand);
      case 'continue':
        if (ctx?.requestId == null) return;
        await _continue(ctx!.requestId!, ctx.revision!);
      case 'cancel':
        final rid = ctx?.requestId ?? (e.req == null ? null : _device?.ids.requestId(e.req!));
        if (rid != null) await _cancel(rid, reason: e.reason, show: false);
      case 'activity':
        if (ctx?.requestId != null && ctx?.revision != null) await _touch(ctx!.requestId!, ctx.revision!);
      case 'status_query':
        final key = e.fields['pending_key'] as String?;
        if (key != null) await reconcile(key);
      case 'hello':
        final key = e.pendingKey;
        if (key != null) await reconcile(key);
      default:
        break; // wake, record_*, dismiss, ack, battery: nothing for the backend
    }
  }

  // ------------------------------------------------- phone side inputs

  /// A new request from typed or phone mic text.
  Future<void> say(String text) => _input(text, requestId: uuidV4(), segmentId: uuidV4(), addition: false);

  /// An addition to the current request.
  Future<void> add(String text) async {
    final rid = _requestId;
    if (rid == null) return say(text);
    await _input(text, requestId: rid, segmentId: uuidV4(), addition: true);
  }

  Future<void> approve() async {
    final r = _latest;
    if (r == null || r.state != 'review' || r.proposalHash == null) return _say('approve: nothing to approve');
    await _approve(r.requestId, r.revision, r.proposalHash!);
  }

  Future<void> select(int index) async {
    final r = _latest;
    final c = r?.candidates;
    if (r == null || c == null || index < 0 || index >= c.length) return _say('select: no candidate $index');
    await _select(r.requestId, r.revision, c[index].candidateId);
  }

  Future<void> continueAnyway() async {
    final r = _latest;
    if (r == null || r.state != 'conflict_schedule') return _say('continue: not in a schedule conflict');
    await _continue(r.requestId, r.revision);
  }

  Future<void> cancel() async {
    final rid = _requestId;
    if (rid != null) await _cancel(rid, reason: 'user', show: true);
  }

  // ------------------------------------------------------------- flows

  Future<void> _input(String text, {required String requestId, required String segmentId, required bool addition}) async {
    final sentAt = isoWithOffset(_clock());
    if (text.trim().isEmpty) {
      // BRIEFING section 2, Empty transcripts: no backend call.
      if (addition && _latest != null) {
        _say('empty addition: proposal kept');
        await _show(_latest!);
        await _touch(_latest!.requestId, _latest!.revision, force: true);
      } else {
        _requestId = requestId;
        _latest = null;
        await _local('FAILURE', requestId, 0, emptyTranscriptLines);
      }
      return;
    }
    if (addition) {
      final base = _latest?.revision ?? _acceptedRev;
      _say('addition rev $base: "$text"');
      await _run(api.addition(requestId, revision: base, segmentId: segmentId, text: text, sentAt: sentAt),
          requestId);
    } else {
      _requestId = requestId;
      _acceptedRev = 0;
      _latest = null;
      _say('start: "$text"');
      await _run(
          api.start(
              requestId: requestId,
              segmentId: segmentId,
              text: text,
              sentAt: sentAt,
              timezone: timezone,
              orientation: orientation),
          requestId);
    }
  }

  Future<void> _select(String rid, int revision, String candidateId) =>
      _run(api.select(rid, revision: revision, candidateId: candidateId), rid);

  Future<void> _run(Stream<StreamEvent> stream, String rid) async {
    final sw = Stopwatch()..start();
    int? acceptedMs;
    try {
      await for (final e in stream) {
        if (rid != _requestId) return _say('dropped ${e.runtimeType} for an old request');
        switch (e) {
          case Accepted():
            if (e.revision > _acceptedRev) _acceptedRev = e.revision;
            acceptedMs ??= sw.elapsedMilliseconds;
          case Status():
            if (e.revision < _acceptedRev) continue;
            await _status(e.statusLine, rid, e.revision);
          case Slow():
            if (e.revision < _acceptedRev) continue;
            await _local('PROCESSING_SLOW', rid, e.revision, const []);
          case ResultEvent(:final result):
            if (result.revision < _acceptedRev) return _say('dropped result rev ${result.revision}');
            _logResult(result, acceptedMs, sw.elapsedMilliseconds);
            await _show(result);
        }
      }
    } on ApiError catch (err) {
      await _onError(err, rid);
    } catch (err) {
      _say('stream failed: $err');
      if (rid == _requestId && _latest == null) await _local('FAILURE', rid, _acceptedRev, unreachableLines);
    }
  }

  Future<void> _approve(String rid, int revision, String hash) async {
    final key = _latest?.requestId == rid ? _latest?.executionKey : null;
    final sw = Stopwatch()..start();
    try {
      final r = await api.approve(rid, revision: revision, proposalHash: hash);
      _logResult(r, null, sw.elapsedMilliseconds, call: 'approve');
      await _show(r);
      if (r.state == 'executing' && r.executionKey != null) await _poll(r.executionKey!);
    } on ApiError catch (err) {
      await _onError(err, rid);
    } catch (err) {
      // No answer: the action may have happened. Uncertain, with the key kept.
      _say('approve got no answer: $err');
      await _local('EXECUTION_UNCERTAIN', rid, revision, uncertainLines, pendingKey: key);
    }
  }

  Future<void> _continue(String rid, int revision) async {
    try {
      await _show(await api.continueAnyway(rid, revision: revision));
    } on ApiError catch (err) {
      await _onError(err, rid);
    } catch (err) {
      _say('continue failed: $err');
    }
  }

  Future<void> _cancel(String rid, {String? reason, required bool show}) async {
    if (rid == _requestId) _requestId = null; // late answers for it are ignored from now on
    try {
      final r = await api.cancel(rid);
      _say('cancel ($reason): ${r.state}');
      if (show) await _show(r, force: true);
    } on ApiError catch (err) {
      _say('cancel: ${err.code}');
      if (err.code == 'already_executing' && err.current != null) {
        _requestId = rid;
        await _show(err.current!);
        final key = err.current!.executionKey;
        if (key != null) await _poll(key);
      }
    } catch (err) {
      _say('cancel failed: $err');
    }
  }

  Future<void> _touch(String rid, int revision, {bool force = false}) async {
    final now = _clock();
    if (!force && _lastTouch != null && now.difference(_lastTouch!) < touchEvery) return;
    _lastTouch = now;
    try {
      final r = await api.touch(rid, revision: revision);
      _say('touch: ${r.expiresInS} s left');
    } catch (err) {
      _say('touch failed: $err'); // loses nothing: approval needs the network too
    }
  }

  /// Polls an approved execution until it is final. 401, 429 and network
  /// errors are not answers: keep polling. A 404 is EXECUTION_UNCERTAIN.
  Future<void> _poll(String key) async {
    if (_polling == key) return;
    _polling = key;
    final deadline = _clock().add(pollFor);
    try {
      while (_clock().isBefore(deadline)) {
        await _sleep(pollInterval);
        try {
          final r = await api.executionStatus(key);
          if (r.state == 'executing') continue;
          await _show(r, force: true);
          return;
        } on ApiError catch (err) {
          if (err.httpStatus == 404) {
            await _local('EXECUTION_UNCERTAIN', _requestId ?? '', 0, uncertainLines, pendingKey: key);
            return;
          }
          _say('poll: ${err.code}, still polling');
        } catch (err) {
          _say('poll: $err, still polling');
        }
      }
      _say('poll gave up after ${pollFor.inSeconds} s; the device reconciles from its key');
    } finally {
      if (_polling == key) _polling = null;
    }
  }

  /// A saved key from the device (`hello`, `status_query`): what happened?
  Future<void> reconcile(String key) async {
    if (_polling == key || !_reconciling.add(key)) return;
    try {
      final r = await api.executionStatus(key);
      _say('reconcile $key: ${r.state}');
      _requestId = r.requestId;
      if (r.state == 'review') {
        // Never approved and still open: cancel it, and the reply is final.
        await _cancel(r.requestId, reason: 'reconcile', show: true);
      } else if (r.state == 'executing') {
        await _show(r, force: true);
        await _poll(key);
      } else {
        await _show(r, force: true, pendingKey: r.state == 'execution_uncertain' ? key : null);
      }
    } on ApiError catch (err) {
      if (err.httpStatus == 404) {
        // Only after a database restore: the action may exist. Never CANCELLED.
        await _local('EXECUTION_UNCERTAIN', _requestId ?? '', 0, uncertainLines, pendingKey: key);
      } else {
        _say('reconcile $key: ${err.code}, the device asks again');
      }
    } catch (err) {
      _say('reconcile $key: $err, the device asks again');
    } finally {
      _reconciling.remove(key);
    }
  }

  Future<void> _onError(ApiError err, String rid) async {
    _say('${err.httpStatus} ${err.code}');
    final cur = err.current;
    if (cur != null) {
      // stale_revision, already_executing, expired...: resync to what is true now.
      if (cur.requestId == _requestId || _requestId == null) _requestId = cur.requestId;
      await _show(cur, force: true);
      if (cur.state == 'executing' && cur.executionKey != null) await _poll(cur.executionKey!);
    } else if (err.code == 'expired') {
      try {
        await _show(await api.requestStatus(rid), force: true);
      } catch (_) {}
    } else if (rid == _requestId && _latest == null) {
      await _local('FAILURE', rid, _acceptedRev, unreachableLines);
    }
  }

  // ------------------------------------------------------------- output

  Future<void> _show(BackendResult r, {bool force = false, String? pendingKey}) async {
    if (!force && r.requestId != _requestId) return _say('dropped result for an old request');
    _latest = r;
    if (r.revision > _acceptedRev) _acceptedRev = r.revision;
    final screen = screens[r.state] ?? r.state.toUpperCase();
    final cands = r.candidates;
    final key = pendingKey ??
        ((screen == 'REVIEW' || screen == 'EXECUTION_UNCERTAIN') ? r.executionKey : null);
    final view = ScreenView(
      screen: screen,
      requestId: r.requestId,
      revision: r.revision,
      lines: r.screenLines ?? const [],
      candidates: cands?.map((c) => c.label).toList() ?? const [],
      expiresInS: r.expiresInS,
      pendingKey: key,
    );
    _views.add(view);
    await _write(() => _device!.sendScreen(screen,
        requestId: r.requestId,
        revision: r.revision,
        proposalHash: r.proposalHash,
        screenLines: view.lines,
        candidateLabels: view.candidates,
        candidateIds: cands?.map((c) => c.candidateId).toList() ?? const [],
        expiresInS: r.expiresInS,
        pendingKey: key));
  }

  Future<void> _status(String line, String rid, int revision) async {
    _views.add(ScreenView(screen: 'PROCESSING', requestId: rid, revision: revision, statusLine: line));
    await _write(() => _device!.sendStatus(line, requestId: rid, revision: revision));
  }

  /// A screen the phone writes without a backend result (the cases above).
  Future<void> _local(String screen, String rid, int revision, List<String> lines, {String? pendingKey}) async {
    _views.add(ScreenView(screen: screen, requestId: rid, revision: revision, lines: lines, pendingKey: pendingKey));
    await _write(() => _device!.sendScreen(screen,
        requestId: rid, revision: revision, screenLines: lines, pendingKey: pendingKey));
  }

  Future<void> _write(Future<Object?> Function() w) async {
    final d = _device;
    if (d == null || d.status.state != LinkState.connected) return;
    try {
      await w();
    } catch (err) {
      _say('screen write failed: $err');
    }
  }

  void _logResult(BackendResult r, int? acceptedMs, int totalMs, {String call = 'stream'}) {
    final t = r.timings;
    final line = 'APIRESULT $call req=${r.requestId.substring(0, 8)} rev=${r.revision} state=${r.state} '
        'accepted=${acceptedMs}ms total=${totalMs}ms backend=${t['backend_total_ms']}ms '
        'model=${t['model_ms']}ms tools=${t['tools_ms']}ms lines=${r.screenLines?.length}';
    // ignore: avoid_print
    print(line);
    _say(line);
  }

  void _say(String m) => _log.add(m);

  Future<void> dispose() async {
    attachDevice(null);
    await _views.close();
    await _log.close();
  }
}
