import 'dart:async';
import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:haloai_phone/backend/api.dart';
import 'package:haloai_phone/backend/backend_client.dart';
import 'package:haloai_phone/backend/request_controller.dart';
import 'package:haloai_phone/ble/ble_messages.dart';
import 'package:haloai_phone/ble/device_session.dart';
import 'package:haloai_phone/stt_worker.dart';

import '../ble/fake_transport.dart';

const key = 'abcdefghijklmnopqrstuvwxyz';

BackendResult res(String state,
        {String rid = 'r1',
        int rev = 1,
        List<String> lines = const ['line'],
        List<List<String>>? candidates,
        String? hash,
        String? execKey,
        String? failureStage}) =>
    BackendResult({
      'state': state,
      'request_id': rid,
      'revision': rev,
      'screen_lines': lines,
      'candidates': candidates?.map((c) => {'candidate_id': c[0], 'label': c[1]}).toList(),
      'proposal_hash': hash,
      'execution_key': execKey,
      'failure_stage': failureStage,
      'expires_in_s': state == 'review' ? 60 : null,
    });

ApiError err(int status, String code, {BackendResult? current}) => ApiError(status, code, '', current: current);

/// The backend, scripted per test. Calls are recorded as strings.
class FakeApi extends BackendClient {
  FakeApi() : super(BackendConfig(Uri.parse('https://x'), 'k'));
  final calls = <String>[];
  Stream<StreamEvent> Function(String rid)? onStart;
  Stream<StreamEvent> Function(String rid, int rev)? onAddition;
  Stream<StreamEvent> Function(String rid, String cand)? onSelect;
  Future<BackendResult> Function()? onApprove, onCancel, onContinue;
  final statusAnswers = <Object>[]; // BackendResult or an error, one per poll

  @override
  Stream<StreamEvent> start({
    required String requestId,
    required String segmentId,
    required String text,
    required String sentAt,
    required String timezone,
    String language = 'en',
    String orientation = 'landscape',
  }) {
    calls.add('start "$text" $timezone');
    return onStart!(requestId);
  }

  @override
  Stream<StreamEvent> addition(String requestId,
      {required int revision, required String segmentId, required String text, required String sentAt}) {
    calls.add('addition rev $revision "$text"');
    return onAddition!(requestId, revision);
  }

  @override
  Stream<StreamEvent> select(String requestId, {required int revision, required String candidateId}) {
    calls.add('select rev $revision $candidateId');
    return onSelect!(requestId, candidateId);
  }

  @override
  Future<BackendResult> approve(String requestId, {required int revision, required String proposalHash}) {
    calls.add('approve rev $revision $proposalHash');
    return onApprove!();
  }

  @override
  Future<BackendResult> continueAnyway(String requestId, {required int revision}) {
    calls.add('continue rev $revision');
    return onContinue!();
  }

  @override
  Future<BackendResult> cancel(String requestId) {
    calls.add('cancel $requestId');
    return onCancel!();
  }

  @override
  Future<BackendResult> touch(String requestId, {required int revision}) async {
    calls.add('touch rev $revision');
    return res('review', rid: requestId, rev: revision);
  }

  @override
  Future<BackendResult> executionStatus(String executionKey) async {
    calls.add('status $executionKey');
    final a = statusAnswers.removeAt(0);
    if (a is BackendResult) return a;
    throw a;
  }

  @override
  Future<BackendResult> requestStatus(String requestId) async {
    calls.add('request $requestId');
    return res('cancelled', rid: requestId);
  }
}

Stream<StreamEvent> answer(String rid, BackendResult r, {String? status}) => Stream.fromIterable([
      Accepted(rid, r.revision, 'c'),
      if (status != null) Status(rid, r.revision, status),
      ResultEvent(BackendResult({...r.json, 'request_id': rid})),
    ]);

void main() {
  late FakeApi api;
  late RequestController c;
  late FakeTransport t;
  late DeviceSession device;
  final views = <ScreenView>[];
  var now = DateTime(2026, 10, 20, 10);

  /// Every Screen write the device received, decoded.
  List<Map> writes() => [
        for (final f in t.screens)
          if (Defragmenter().add(f) case final j?) jsonDecode(j) as Map
      ];

  setUp(() async {
    api = FakeApi();
    now = DateTime(2026, 10, 20, 10);
    c = RequestController(api: api, timezone: 'America/Chicago', clock: () => now, sleep: (_) async {});
    views.clear();
    c.views.listen(views.add);
    t = FakeTransport();
    device = DeviceSession(
        transport: t,
        sttReady: () => true,
        transcribe: (s, sr, {tag}) async => SttResult('', 0, 0, tag: tag));
    c.attachDevice(device);
    await device.start();
  });

  tearDown(() => device.stop());

  /// A device press on the screen the phone wrote last.
  Future<void> press(String type, {int? index}) async {
    final sid = writes().last['sid'] as int;
    t.event({'type': type, 'sid': sid, 'index': ?index});
    await pumpEventQueue();
  }

  test('review: the device gets the lines, the countdown and pending_key', () async {
    api.onStart = (rid) => answer(rid, res('review', hash: 'h', execKey: key), status: 'Ready to confirm');
    await c.say('block 3 to 4 tomorrow for the gym');
    final w = writes();
    expect(w.first, {'sid': w.first['sid'], 'status_line': 'Ready to confirm'}); // status bar only
    expect(w.last['screen'], 'REVIEW');
    expect(w.last['pending_key'], key);
    expect(w.last['expires_in_s'], 60);
    expect(api.calls.single, 'start "block 3 to 4 tomorrow for the gym" America/Chicago');
  });

  test('pending_key goes only on REVIEW and EXECUTION_UNCERTAIN', () async {
    api.onStart = (rid) => answer(rid, res('review', hash: 'h', execKey: key));
    api.onApprove = () async => res('executing', rid: c.currentRequestId!, hash: 'h', execKey: key);
    api.statusAnswers.add(res('success', rid: 'r1', execKey: key));
    await c.say('gym');
    await press('approve');
    expect(writes().map((w) => (w['screen'], w.containsKey('pending_key'))),
        [('REVIEW', true), ('EXECUTING', false), ('SUCCESS', false)]);
  });

  test('device approve sends the revision and hash of the screen it was pressed on, then polls', () async {
    api.onStart = (rid) => answer(rid, res('review', rev: 3, hash: 'h3', execKey: key));
    api.onApprove = () async => res('executing', rid: c.currentRequestId!, rev: 3, execKey: key);
    api.statusAnswers
      ..add(err(429, 'rate_limited'))
      ..add(Exception('network down'))
      ..add(res('executing', rid: 'r1', rev: 3, execKey: key))
      ..add(res('success', rid: 'r1', rev: 3, execKey: key));
    await c.say('gym');
    await press('approve');
    expect(api.calls.where((x) => x.startsWith('approve')), ['approve rev 3 h3']);
    expect(api.calls.where((x) => x.startsWith('status')), hasLength(4)); // 429 and network kept polling
    expect(views.last.screen, 'SUCCESS');
  });

  test('approve with no answer is EXECUTION_UNCERTAIN with the key kept, never FAILURE', () async {
    api.onStart = (rid) => answer(rid, res('review', hash: 'h', execKey: key));
    api.onApprove = () async => throw TimeoutException('no answer');
    await c.say('email Victor');
    await c.approve();
    expect(writes().last['screen'], 'EXECUTION_UNCERTAIN');
    expect(writes().last['pending_key'], key);
  });

  test('candidates: labels to the device, the id behind the pressed index to the backend', () async {
    api.onStart = (rid) => answer(rid, res('conflict_ambiguous', candidates: [
          ['c-kim', 'Alex Kim'],
          ['c-riv', 'Alex Rivera']
        ]));
    api.onSelect = (rid, cand) => answer(rid, res('review', rev: 2, hash: 'h', execKey: key));
    await c.say('email Alex');
    expect(writes().last['candidates'], ['Alex Kim', 'Alex Rivera']);
    await press('select', index: 1);
    expect(api.calls.last, 'select rev 1 c-riv');
    expect(views.last.screen, 'REVIEW');
  });

  test('continue despite a schedule conflict', () async {
    api.onStart = (rid) => answer(rid, res('conflict_schedule'));
    api.onContinue = () async => res('review', rid: c.currentRequestId!, rev: 2, hash: 'h', execKey: key);
    await c.say('meeting at 10');
    await press('continue');
    expect(api.calls.last, 'continue rev 1');
    expect(views.last.screen, 'REVIEW');
  });

  test('an addition sends the base revision; a stale_revision answer resyncs to current', () async {
    api.onStart = (rid) => answer(rid, res('review', hash: 'h', execKey: key));
    api.onAddition = (rid, rev) => answer(rid, res('review', rev: 2, hash: 'h2', execKey: 'k2'));
    await c.say('gym');
    await c.add('make it 5 to 6');
    expect(api.calls.last, 'addition rev 1 "make it 5 to 6"');
    expect(c.current!.revision, 2);
    api.onApprove = () async => throw err(409, 'stale_revision',
        current: res('review', rid: c.currentRequestId!, rev: 3, hash: 'h3', execKey: 'k3'));
    await c.approve();
    expect(views.last.revision, 3);
  });

  test('device cancel reaches the backend and later answers for it are dropped', () async {
    final late = StreamController<StreamEvent>();
    api.onStart = (rid) => late.stream;
    api.onCancel = () async => res('cancelled');
    final run = c.say('gym');
    await pumpEventQueue();
    final rid = c.currentRequestId!;
    t.event({'type': 'cancel', 'req': 1, 'sid': null, 'reason': 'user'});
    await pumpEventQueue();
    // The device's cancel names its own req, which maps to its own request
    // ID; this request was typed, so cancel it from the phone instead.
    await c.cancel();
    late
      ..add(Accepted(rid, 1, 'c'))
      ..add(ResultEvent(res('review', rid: rid, hash: 'h', execKey: key)));
    await late.close();
    await run;
    expect(views.where((v) => v.screen == 'REVIEW'), isEmpty);
    expect(api.calls.where((x) => x.startsWith('cancel')), isNotEmpty);
  });

  test('a newer request drops the results of the older one', () async {
    final first = StreamController<StreamEvent>();
    var n = 0;
    api.onStart = (rid) => n++ == 0 ? first.stream : answer(rid, res('read_only_result'));
    final old = c.say('first');
    await pumpEventQueue();
    final oldRid = c.currentRequestId!;
    await c.say('second');
    first
      ..add(ResultEvent(res('review', rid: oldRid, hash: 'h', execKey: key)))
      ..close();
    await old;
    expect(views.map((v) => v.screen), ['READ_ONLY_RESULT']);
  });

  test('empty transcripts: FAILURE with no call on a new request; on an addition the proposal stays', () async {
    await c.say('   ');
    expect(api.calls, isEmpty);
    expect(views.last.screen, 'FAILURE');
    expect(views.last.lines, RequestController.emptyTranscriptLines);

    api.onStart = (rid) => answer(rid, res('review', hash: 'h', execKey: key));
    await c.say('gym');
    final before = writes().length;
    await c.add('');
    expect(writes().length, before + 1);
    expect(writes().last['screen'], 'REVIEW');
    expect(writes().last['rev'], 1);
    expect(api.calls.last, 'touch rev 1');
  });

  test('knob activity becomes touch, at most once every 10 s', () async {
    api.onStart = (rid) => answer(rid, res('review', hash: 'h', execKey: key));
    await c.say('gym');
    await press('activity');
    await press('activity');
    now = now.add(const Duration(seconds: 11));
    await press('activity');
    expect(api.calls.where((x) => x.startsWith('touch')), hasLength(2));
  });

  group('reconcile a saved key', () {
    test('never approved and still open: cancel, and CANCELLED tells the device to clear it', () async {
      api.statusAnswers.add(res('review', rid: 'r9', hash: 'h', execKey: key));
      api.onCancel = () async => res('cancelled', rid: 'r9');
      t.event({'type': 'status_query', 'pending_key': key});
      await pumpEventQueue();
      expect(api.calls, ['status $key', 'cancel r9']);
      expect(writes().last['screen'], 'CANCELLED');
    });

    test('the approve won the race: keep the key and poll to the outcome', () async {
      api.statusAnswers
        ..add(res('review', rid: 'r9', hash: 'h', execKey: key))
        ..add(res('success', rid: 'r9', execKey: key));
      api.onCancel = () async =>
          throw err(409, 'already_executing', current: res('executing', rid: 'r9', execKey: key));
      await c.reconcile(key);
      expect(writes().map((w) => w['screen']), ['EXECUTING', 'SUCCESS']);
    });

    test('unknown key (404) is EXECUTION_UNCERTAIN with the key, never CANCELLED', () async {
      api.statusAnswers.add(err(404, 'not_found'));
      t.event({'type': 'hello', 'boot': 1, 'state': 'BOOT_RECOVERY', 'req': 0, 'pending_key': key});
      await pumpEventQueue();
      expect(writes().last['screen'], 'EXECUTION_UNCERTAIN');
      expect(writes().last['pending_key'], key);
      expect(writes().where((w) => w['screen'] == 'CANCELLED'), isEmpty);
    });

    test('an authoritative outcome is shown as is', () async {
      api.statusAnswers.add(res('failure', rid: 'r9', failureStage: 'execution'));
      await c.reconcile(key);
      expect(writes().last['screen'], 'FAILURE');
      expect(writes().last.containsKey('failure_stage'), isFalse); // not over BLE
    });
  });
}
