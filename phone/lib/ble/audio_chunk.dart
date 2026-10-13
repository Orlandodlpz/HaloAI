import 'dart:typed_data';

import 'crc16.dart';

/// One notification on the Audio characteristic (BRIEFING section 2):
/// a 16 byte little endian header, then the payload.
///
/// | 0 format | 1 flags (bit 0 final) | 2-3 req | 4 rev | 5 seg | 6-7 seq |
/// | 8-9 payload length | 10-11 CRC16 | 12-13 predictor | 14 step index | 15 reserved |
class AudioChunk {
  AudioChunk({
    required this.format,
    required this.isFinal,
    required this.req,
    required this.rev,
    required this.seg,
    required this.seq,
    required this.predictor,
    required this.stepIndex,
    required this.payload,
  });

  static const headerBytes = 16;
  static const audioPayloadBytes = 160; // 320 samples, 20 ms at 16 kHz
  static const finalPayloadBytes = 6; // u16 total chunks, u32 total samples
  static const formatImaAdpcm16kMono = 1;

  final int format;
  final bool isFinal;
  final int req; // device local request counter (u16)
  final int rev; // low byte of the revision displayed when the hold started
  final int seg; // device local segment counter (u8)
  final int seq;
  final int predictor;
  final int stepIndex;
  final Uint8List payload;

  /// Final marker only: total audio chunks in the segment.
  int get totalChunks => ByteData.sublistView(payload).getUint16(0, Endian.little);

  /// Final marker only: total samples in the segment.
  int get totalSamples => ByteData.sublistView(payload).getUint32(2, Endian.little);

  /// Parses and checks one notification. Throws [ChunkError] on a short
  /// packet, a length mismatch, a bad CRC, an unknown format or a malformed
  /// final marker. The caller treats a rejected chunk as missing.
  static AudioChunk parse(Uint8List bytes) {
    if (bytes.length < headerBytes) throw ChunkError('short packet: ${bytes.length} bytes');
    final h = ByteData.sublistView(bytes, 0, headerBytes);
    final len = h.getUint16(8, Endian.little);
    if (bytes.length != headerBytes + len) {
      throw ChunkError('length field $len, packet has ${bytes.length - headerBytes}');
    }
    final payload = Uint8List.sublistView(bytes, headerBytes);
    final crc = crc16CcittFalse([Uint8List.sublistView(bytes, 0, 10), payload]);
    if (crc != h.getUint16(10, Endian.little)) throw ChunkError('bad CRC');
    final format = h.getUint8(0);
    if (format != formatImaAdpcm16kMono) throw ChunkError('unknown format $format');
    final isFinal = h.getUint8(1) & 1 != 0;
    if (isFinal && len != finalPayloadBytes) throw ChunkError('final marker payload $len bytes');
    if (!isFinal && len > audioPayloadBytes) throw ChunkError('audio payload $len bytes');
    return AudioChunk(
      format: format,
      isFinal: isFinal,
      req: h.getUint16(2, Endian.little),
      rev: h.getUint8(4),
      seg: h.getUint8(5),
      seq: h.getUint16(6, Endian.little),
      predictor: h.getInt16(12, Endian.little),
      stepIndex: h.getUint8(14),
      payload: Uint8List.fromList(payload),
    );
  }

  /// Builds the wire bytes (CRC filled in). Used by tests; the device builds
  /// the same layout in firmware (Victor, ESP32-S3).
  Uint8List toBytes() {
    final out = Uint8List(headerBytes + payload.length);
    final h = ByteData.sublistView(out, 0, headerBytes)
      ..setUint8(0, format)
      ..setUint8(1, isFinal ? 1 : 0)
      ..setUint16(2, req, Endian.little)
      ..setUint8(4, rev)
      ..setUint8(5, seg)
      ..setUint16(6, seq, Endian.little)
      ..setUint16(8, payload.length, Endian.little)
      ..setInt16(12, predictor, Endian.little)
      ..setUint8(14, stepIndex);
    out.setRange(headerBytes, out.length, payload);
    h.setUint16(10, crc16CcittFalse([Uint8List.sublistView(out, 0, 10), payload]), Endian.little);
    return out;
  }
}

class ChunkError implements Exception {
  ChunkError(this.message);
  final String message;
  @override
  String toString() => 'ChunkError: $message';
}
