import 'dart:isolate';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:haloai_phone/stt_worker.dart';

// Fake workers speak the same protocol as the real one, without a model.
// Request: [id, kind, ...]. Reply: ['ok', id, text, audioSec, decodeMs].

/// Echoes the WAV path as the text. Holds 'slow' requests until the next
/// one arrives, then answers both newest first, to prove replies are matched
/// by id and not by order. 'crash' throws, killing the isolate.
void _fakeWorker(List args) {
  final replies = args[0] as SendPort;
  final requests = ReceivePort();
  replies.send([requests.sendPort, 5]);
  List? held;
  requests.listen((msg) {
    final m = msg as List;
    final path = m[2] as String;
    if (path == 'crash') throw StateError('boom');
    if (path == 'slow') {
      held = m;
      return;
    }
    replies.send(['ok', m[0], path, 1.0, 10]);
    if (held != null) {
      replies.send(['ok', held![0], 'slow', 1.0, 10]);
      held = null;
    }
  });
}

void _failingLoad(List args) => (args[0] as SendPort).send('Model load failed: missing');

void main() {
  test('results come back with their tag, matched by id not order', () async {
    final w = await SttWorker.startWith(_fakeWorker, '');
    final slow = w.transcribeWav('slow', tag: 'seg-1');
    final fast = w.transcribeWav('fast', tag: 'seg-2');
    final f = await fast;
    final s = await slow;
    expect((f.text, f.tag), ('fast', 'seg-2'));
    expect((s.text, s.tag), ('slow', 'seg-1'));
    expect(w.loadMs, 5);
    w.dispose();
  });

  test('a crashed worker fails pending and later requests', () async {
    final w = await SttWorker.startWith(_fakeWorker, '');
    expect(w.isAlive, isTrue);
    final pending = w.transcribeWav('slow');
    final crash = w.transcribeWav('crash');
    await expectLater(pending, throwsA(isA<SttWorkerDead>()));
    await expectLater(crash, throwsA(isA<SttWorkerDead>()));
    expect(w.isAlive, isFalse);
    await expectLater(w.transcribeSamples(Float32List(1), 16000), throwsA(isA<SttWorkerDead>()));
  });

  test('a failed model load fails start', () async {
    await expectLater(SttWorker.startWith(_failingLoad, ''),
        throwsA(isA<StateError>().having((e) => e.message, 'message', contains('missing'))));
  });
}
