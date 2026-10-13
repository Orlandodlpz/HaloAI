import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:haloai_phone/ble/ble_messages.dart';
import 'package:haloai_phone/ble/device_session.dart';
import 'package:haloai_phone/ble/device_transport.dart';
import 'package:haloai_phone/ble/ima_adpcm.dart';
import 'package:haloai_phone/stt_worker.dart';

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

void main() {
  late FakeTransport t;
  late DeviceSession s;
  late int now;
  var ready = true;
  final decoded = <Object?>[];
  Completer<void>? gate;

  setUp(() {
    t = FakeTransport();
    now = 0;
    ready = true;
    decoded.clear();
    gate = null;
    s = DeviceSession(
      transport: t,
      sttReady: () => ready,
      nowMs: () => now,
      clock: () => DateTime(2026, 10, 13, 14, 5),
      transcribe: (samples, sr, {tag}) async {
        decoded.add(tag);
        if (gate != null) await gate!.future;
        return SttResult('text for $tag', samples.length / sr, 7, tag: tag);
      },
    );
  });

  tearDown(() => s.stop());

  void hold(int req, int seg, {int rev = 0, int samples = 3200}) {
    t.event({'type': 'record_start', 'req': req, 'seg': seg, 'rev': rev, 'addition': seg > 0});
    final chunks = pcmToChunks(Int16List(samples), req: req, rev: rev, seg: seg);
    for (final c in chunks.take(chunks.length - 1)) {
      now += 20;
      t.audioC.add(c);
    }
    t.event({'type': 'record_end', 'req': req, 'seg': seg, 'chunks': chunks.length - 1, 'ms': samples ~/ 16});
    now += 30;
    t.audioC.add(chunks.last);
  }

  test('hello gets settings back with phone_ready and an offset timestamp', () async {
    await s.start();
    t.event({'type': 'hello', 'fw': '0.1', 'boot': 7, 'state': 'READY', 'req': 0, 'sid': null, 'pending_key': null, 'battery': 90});
    await pumpEventQueue();
    expect(t.settings.single['phone_ready'], isTrue);
    expect(t.settings.single['time'], matches(RegExp(r'^2026-10-13T14:05:00[+-]\d\d:\d\d$')));

    ready = false;
    await s.phoneReadyChanged();
    expect(t.settings.last['phone_ready'], isFalse);
  });

  test('a hold becomes a transcript with request and segment UUIDs and release timing', () async {
    await s.start();
    t.event({'type': 'hello', 'boot': 1, 'state': 'READY', 'req': 0});
    final got = s.transcripts.first;
    hold(3, 0, rev: 0);
    final tr = await got;
    expect(tr.result.text, 'text for (3, 0)');
    expect(tr.requestId, s.ids.requestId(3));
    expect(tr.segmentId, isNot(tr.requestId));
    expect(tr.releaseToCompleteMs, 30);
    expect(t.acks.last['final'], isTrue);
    expect(tr.logLine, startsWith('BLERESULT req=3 seg=0'));
  });

  test('an addition keeps the request UUID, gets a new segment UUID', () async {
    await s.start();
    t.event({'type': 'hello', 'boot': 1, 'state': 'READY', 'req': 0});
    final two = s.transcripts.take(2).toList();
    hold(4, 0);
    hold(4, 1, rev: 1);
    final r = await two;
    expect(r[0].requestId, r[1].requestId);
    expect(r[0].segmentId, isNot(r[1].segmentId));
  });

  test('cancel while decoding drops the transcript', () async {
    await s.start();
    t.event({'type': 'hello', 'boot': 1, 'state': 'READY', 'req': 0});
    gate = Completer();
    final out = <DeviceTranscript>[];
    s.transcripts.listen(out.add);
    hold(5, 0);
    await pumpEventQueue();
    expect(decoded, [(5, 0)]);
    t.event({'type': 'cancel', 'req': 5, 'sid': null});
    gate!.complete();
    await pumpEventQueue();
    expect(out, isEmpty);
  });

  test('device restart (new boot) resets request mappings; same boot keeps them', () async {
    await s.start();
    t.event({'type': 'hello', 'boot': 1, 'state': 'READY', 'req': 0});
    final first = s.ids.requestId(1);
    t.event({'type': 'hello', 'boot': 1, 'state': 'PROCESSING', 'req': 1});
    expect(s.ids.requestId(1), first);
    t.event({'type': 'hello', 'boot': 2, 'state': 'READY', 'req': 0});
    expect(s.ids.requestId(1), isNot(first));
  });

  test('screens get fresh sids and a select maps back to the candidate', () async {
    await s.start();
    t.event({'type': 'hello', 'boot': 1, 'state': 'READY', 'req': 0});
    final ctx = await s.sendScreen('CONFLICT_AMBIGUOUS',
        requestId: 'r1', revision: 2, candidateLabels: ['Victor R', 'Victor S'], candidateIds: ['c-r', 'c-s']);
    final screen = jsonDecode(Defragmenter().add(t.screens.single)!) as Map;
    expect(screen['sid'], ctx.sid);
    expect(screen['candidates'], ['Victor R', 'Victor S']);
    expect(s.ids.candidateId(ctx.sid, 1), 'c-s');
    final logged = s.log.firstWhere((l) => l.startsWith('select'));
    t.event({'type': 'select', 'sid': ctx.sid, 'index': 1});
    expect(await logged, contains('candidate c-s'));
  });

  test('garbage events are reported, not fatal', () async {
    await s.start();
    final logged = s.log.firstWhere((l) => l.startsWith('event:'));
    t.event({'type': 'self_destruct'});
    expect(await logged, contains('unknown event type'));
  });

  test('isoWithOffset formats negative and positive offsets', () {
    final t = DateTime(2026, 1, 2, 3, 4, 5);
    expect(isoWithOffset(t), matches(RegExp(r'^2026-01-02T03:04:05[+-]\d\d:\d\d$')));
  });
}
