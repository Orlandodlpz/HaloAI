import 'dart:convert';
import 'dart:io';
import 'dart:math';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:haloai_phone/ble/audio_chunk.dart';
import 'package:haloai_phone/ble/crc16.dart';
import 'package:haloai_phone/ble/ima_adpcm.dart';
import 'package:haloai_phone/ble/segment_assembler.dart';

Uint8List _hex(String h) =>
    Uint8List.fromList([for (var i = 0; i < h.length; i += 2) int.parse(h.substring(i, i + 2), radix: 16)]);

/// Shared with tools/audio_reference/make_golden.py: chunk bytes from the Python
/// encoder and the PCM its reference decoder produced.
final _golden = jsonDecode(File('test/golden/adpcm_chunks.json').readAsStringSync()) as Map;
final _goldenChunks = [for (final h in _golden['chunks_hex'] as List) _hex(h as String)];
final _goldenInput = Int16List.fromList((_golden['input_pcm'] as List).cast<int>());
final _goldenDecoded = (_golden['decoded_pcm'] as List).cast<int>();

class _Harness {
  final acks = <AudioAck>[];
  final segments = <SegmentAudio>[];
  final errors = <Object>[];
  late final rx = AudioReceiver(onAck: acks.add, onSegment: segments.add, onError: errors.add);
  int now = 0;

  void send(Uint8List b, {int dt = 20}) {
    now += dt;
    rx.onNotification(b, now);
  }

  void wait(int ms) {
    for (var t = 0; t < ms; t += 50) {
      now += 50;
      rx.tick(now);
    }
  }
}

void main() {
  test('CRC-16/CCITT-FALSE check value', () {
    expect(crc16CcittFalse([Uint8List.fromList(utf8.encode('123456789'))]), 0x29B1);
  });

  test('chunk round trip, 176 bytes, and corrupted bytes are rejected', () {
    final b = _goldenChunks.first;
    expect(b.length, 176);
    final c = AudioChunk.parse(b);
    expect((c.req, c.rev, c.seg, c.seq, c.isFinal), (513, 2, 3, 0, false));
    expect(c.toBytes(), b);

    final bad = Uint8List.fromList(b)..[100] ^= 1;
    expect(() => AudioChunk.parse(bad), throwsA(isA<ChunkError>()));
    expect(() => AudioChunk.parse(Uint8List.sublistView(b, 0, 170)), throwsA(isA<ChunkError>()));
  });

  test('Dart encoder makes the same bytes as the Python reference', () {
    final mine = pcmToChunks(_goldenInput, req: 513, rev: 2, seg: 3);
    expect(mine.length, _goldenChunks.length);
    for (var i = 0; i < mine.length; i++) {
      expect(mine[i], _goldenChunks[i], reason: 'packet $i');
    }
  });

  test('in order: decodes exactly what the Python decoder got, acks every 10, final ack', () {
    final h = _Harness();
    for (final b in _goldenChunks) {
      h.send(b);
    }
    expect(h.errors, isEmpty);
    expect(h.segments, hasLength(1));
    expect(h.segments.single.pcm, _goldenDecoded);
    expect(h.segments.single.rev, 2);
    final last = h.acks.last;
    expect((last.isFinal, last.upto, last.resendFrom), (true, 25, null));
    expect(h.acks.where((a) => !a.isFinal).map((a) => a.upto), [9, 19]);
    expect(last.toJson(), {'req': 513, 'seg': 3, 'upto': 25, 'final': true});
  });

  test('a lost chunk is asked for again and nothing after it is corrupted', () {
    final h = _Harness();
    final lost = 7;
    for (var i = 0; i < _goldenChunks.length; i++) {
      if (i != lost) h.send(_goldenChunks[i]);
    }
    expect(h.segments, isEmpty);
    expect(h.acks.any((a) => a.resendFrom == lost), isTrue);
    expect(h.acks.last.upto, lost - 1);

    // Device resends from 7 (go back N): 7 is new, the rest are duplicates.
    for (var i = lost; i < _goldenChunks.length; i++) {
      h.send(_goldenChunks[i]);
    }
    expect(h.segments.single.pcm, _goldenDecoded);
    expect(h.acks.last.isFinal, isTrue);
    expect(h.segments.single.stats.duplicates, _goldenChunks.length - 1 - lost - 1);
  });

  test('a corrupted chunk counts as missing', () {
    final h = _Harness();
    for (var i = 0; i < _goldenChunks.length; i++) {
      final b = i == 3 ? (Uint8List.fromList(_goldenChunks[i])..[50] ^= 0xFF) : _goldenChunks[i];
      h.send(b);
    }
    expect(h.rx.rejectedChunks, 1);
    expect(h.segments, isEmpty);
    expect(h.acks.last.resendFrom, 3);
    h.send(_goldenChunks[3]);
    expect(h.segments.single.pcm, _goldenDecoded);
  });

  test('shuffled and duplicated delivery still gives the same audio', () {
    final h = _Harness();
    final order = [for (var i = 0; i < _goldenChunks.length - 1; i++) i]..shuffle(Random(1));
    for (final i in [...order, ...order.take(5)]) {
      h.send(_goldenChunks[i]);
    }
    h.send(_goldenChunks.last);
    expect(h.segments.single.pcm, _goldenDecoded);
    // Duplicates mid stream are not counted as new chunks and do not speed up acks.
    expect(h.segments.single.stats.chunks, _goldenChunks.length - 1);
    expect(h.segments.single.stats.duplicates, 5);
  });

  test('final marker before the last chunk waits for it', () {
    final h = _Harness();
    final n = _goldenChunks.length;
    for (var i = 0; i < n - 2; i++) {
      h.send(_goldenChunks[i]);
    }
    h.send(_goldenChunks[n - 1]); // final marker
    expect(h.segments, isEmpty);
    expect(h.acks.last.resendFrom, n - 2);
    h.send(_goldenChunks[n - 2]);
    expect(h.segments, hasLength(1));
    expect(h.acks.last.isFinal, isTrue);
  });

  test('200 ms timer acks progress, and keeps asking for a gap', () {
    final h = _Harness();
    for (var i = 0; i < 3; i++) {
      h.send(_goldenChunks[i]);
    }
    expect(h.acks, isEmpty);
    h.wait(200);
    expect(h.acks.single.upto, 2);
    h.wait(400);
    expect(h.acks, hasLength(1), reason: 'nothing new, no gap: no ack');

    h.send(_goldenChunks[5]); // 3 and 4 missing
    expect(h.acks.last.resendFrom, 3);
    final before = h.acks.length;
    h.send(_goldenChunks[6]); // same gap, not repeated at once
    expect(h.acks.length, before);
    h.wait(250);
    expect(h.acks.last.resendFrom, 3);
  });

  test('a repeated final marker is acknowledged again, segment reported once', () {
    final h = _Harness();
    for (final b in _goldenChunks) {
      h.send(b);
    }
    h.send(_goldenChunks.last);
    expect(h.segments, hasLength(1));
    expect(h.acks.where((a) => a.isFinal), hasLength(2));
  });

  test('final marker that contradicts the chunks is an error, not partial audio', () {
    final h = _Harness();
    for (var i = 0; i < _goldenChunks.length - 1; i++) {
      h.send(_goldenChunks[i]);
    }
    final tooFew = pcmToChunks(Int16List(320 * 5), req: 513, rev: 2, seg: 3).last; // claims 5
    h.send(tooFew);
    expect(h.errors.single, isA<SegmentError>());
    expect(h.segments, isEmpty);
  });

  test('two segments of one request assemble independently; cancel drops the open one', () {
    final h = _Harness();
    final a = pcmToChunks(Int16List(640), req: 9, rev: 0, seg: 0);
    final b = pcmToChunks(Int16List(640), req: 9, rev: 1, seg: 1);
    h.send(a[0]);
    h.send(b[0]);
    for (final x in a.skip(1)) {
      h.send(x);
    }
    expect(h.segments.single.seg, 0);
    expect(h.rx.activeSegments, 1);
    h.rx.dropRequest(9);
    expect(h.rx.activeSegments, 0);
  });

  test('ADPCM keeps speech level quality (SNR > 20 dB on the golden sweep)', () {
    double sig = 0, err = 0;
    for (var i = 0; i < 6000; i++) {
      if (i >= 3000 && i < 3600) continue; // skip the clipped noise burst
      sig += _goldenInput[i] * _goldenInput[i].toDouble();
      final d = _goldenInput[i] - _goldenDecoded[i];
      err += d * d.toDouble();
    }
    expect(10 * log(sig / err) / ln10, greaterThan(20));
  });
}
