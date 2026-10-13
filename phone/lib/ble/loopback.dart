import 'dart:typed_data';

import 'ima_adpcm.dart';
import 'segment_assembler.dart';

/// On phone check of the whole audio path without a radio: PCM is encoded
/// and chunked the way the device does it, sent through the same
/// [AudioReceiver] the BLE link uses (losing some chunks on purpose so the
/// resend path runs), and the decoded audio comes back for Parakeet.
/// If the transcript matches the original WAV's, the phone side of P1 is
/// right, and any difference on the real link is the link's or firmware's.
class LoopbackResult {
  LoopbackResult(this.audio, this.lost, this.resendRequests);
  final SegmentAudio audio;
  final int lost, resendRequests;
}

LoopbackResult loopback(Int16List pcm, {int loseEvery = 9}) {
  final chunks = pcmToChunks(pcm, req: 1, rev: 0, seg: 0);
  final audioCount = chunks.length - 1;
  SegmentAudio? done;
  int? rewindTo;
  final rx = AudioReceiver(
    onAck: (a) {
      if (a.resendFrom != null) rewindTo = a.resendFrom;
    },
    onSegment: (s) => done = s,
    onError: (e) => throw StateError('loopback: $e'),
  );
  final sentOnce = <int>{};
  var lost = 0, now = 0, next = 0, finalSends = 0;
  while (done == null) {
    if (next < audioCount) {
      final first = sentOnce.add(next);
      now += 20;
      if (first && loseEvery > 0 && next % loseEvery == loseEvery - 1) {
        lost++;
      } else {
        rx.onNotification(chunks[next], now);
      }
      next++;
    } else {
      if (++finalSends > 50) throw StateError('loopback: segment never completed');
      now += 20;
      rx.onNotification(chunks.last, now);
    }
    rx.tick(now);
    if (rewindTo != null) {
      next = rewindTo! < next ? rewindTo! : next;
      rewindTo = null;
    }
  }
  return LoopbackResult(done!, lost, done!.stats.resendRequests);
}

/// 16 bit mono PCM from a WAV file's `data` chunk.
Int16List wavPcm16(Uint8List wav) {
  final b = ByteData.sublistView(wav);
  var o = 12;
  while (o + 8 <= wav.length) {
    final id = String.fromCharCodes(wav.sublist(o, o + 4));
    final size = b.getUint32(o + 4, Endian.little);
    if (id == 'data') {
      final pcm = Int16List(size ~/ 2);
      for (var i = 0; i < pcm.length; i++) {
        pcm[i] = b.getInt16(o + 8 + i * 2, Endian.little);
      }
      return pcm;
    }
    o += 8 + size + (size & 1);
  }
  throw FormatException('WAV has no data chunk');
}
