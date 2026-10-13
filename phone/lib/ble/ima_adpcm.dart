import 'dart:typed_data';

import 'audio_chunk.dart';

/// IMA ADPCM decoder for one audio chunk. Every chunk carries the decoder
/// state at its start (predictor and step index), so chunks decode on their
/// own and a lost chunk does not corrupt the ones after it.
///
/// Nibble order: low nibble first, then high nibble (standard IMA order).
/// Each payload byte becomes 2 samples.
Int16List decodeImaAdpcm(Uint8List payload, int predictor, int stepIndex) {
  final out = Int16List(payload.length * 2);
  var pred = predictor;
  var index = stepIndex.clamp(0, 88);
  var o = 0;
  for (final byte in payload) {
    for (final nibble in [byte & 0x0F, byte >> 4]) {
      final step = imaStepTable[index];
      var diff = step >> 3;
      if (nibble & 4 != 0) diff += step;
      if (nibble & 2 != 0) diff += step >> 1;
      if (nibble & 1 != 0) diff += step >> 2;
      pred = nibble & 8 != 0 ? pred - diff : pred + diff;
      pred = pred.clamp(-32768, 32767);
      index = (index + imaIndexTable[nibble]).clamp(0, 88);
      out[o++] = pred;
    }
  }
  return out;
}

/// Encoder with state carried across chunks, like the device's stream. The
/// phone never encodes in the real flow; this exists for the loopback test
/// (P0 clips through the full BLE audio path without a radio) and to prove
/// it matches the Python reference (`tools/audio_reference/`) byte for byte.
class ImaAdpcmEncoder {
  int predictor = 0;
  int stepIndex = 0;

  /// Encodes an even number of samples into length / 2 bytes.
  Uint8List encode(Int16List samples) {
    assert(samples.length.isEven);
    final out = Uint8List(samples.length ~/ 2);
    for (var i = 0; i < samples.length; i += 2) {
      out[i ~/ 2] = _one(samples[i]) | (_one(samples[i + 1]) << 4);
    }
    return out;
  }

  int _one(int sample) {
    var step = imaStepTable[stepIndex];
    var diff = sample - predictor;
    var nibble = 0;
    if (diff < 0) {
      nibble = 8;
      diff = -diff;
    }
    var vpdiff = step >> 3;
    if (diff >= step) {
      nibble |= 4;
      diff -= step;
      vpdiff += step;
    }
    step >>= 1;
    if (diff >= step) {
      nibble |= 2;
      diff -= step;
      vpdiff += step;
    }
    step >>= 1;
    if (diff >= step) {
      nibble |= 1;
      vpdiff += step;
    }
    predictor = (nibble & 8 != 0 ? predictor - vpdiff : predictor + vpdiff).clamp(-32768, 32767);
    stepIndex = (stepIndex + imaIndexTable[nibble]).clamp(0, 88);
    return nibble;
  }
}

/// Splits 16 kHz mono PCM into audio chunks plus the final marker, the way
/// the device does (and `tools/audio_reference/chunks.py`). The last chunk is
/// zero padded to an even sample count; the final marker carries the real
/// total. Used by tests and the on-phone loopback check.
List<Uint8List> pcmToChunks(Int16List pcm, {required int req, required int rev, required int seg}) {
  final enc = ImaAdpcmEncoder();
  final out = <Uint8List>[];
  var seq = 0;
  for (var start = 0; start < pcm.length; start += 320) {
    final end = (start + 320).clamp(0, pcm.length);
    final part = Int16List((end - start).isEven ? end - start : end - start + 1)
      ..setRange(0, end - start, pcm, start);
    final (p, i) = (enc.predictor, enc.stepIndex);
    out.add(AudioChunk(format: AudioChunk.formatImaAdpcm16kMono, isFinal: false, req: req, rev: rev,
            seg: seg, seq: seq++, predictor: p, stepIndex: i, payload: enc.encode(part))
        .toBytes());
  }
  final totals = ByteData(6)
    ..setUint16(0, seq, Endian.little)
    ..setUint32(2, pcm.length, Endian.little);
  out.add(AudioChunk(format: AudioChunk.formatImaAdpcm16kMono, isFinal: true, req: req, rev: rev,
          seg: seg, seq: seq, predictor: 0, stepIndex: 0, payload: totals.buffer.asUint8List())
      .toBytes());
  return out;
}

const imaIndexTable =<int>[-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8];

const imaStepTable = <int>[
  7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
  50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
  253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
  1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
  3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
  11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
  32767,
];
