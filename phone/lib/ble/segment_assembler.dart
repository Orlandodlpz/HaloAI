import 'dart:typed_data';

import 'audio_chunk.dart';
import 'ima_adpcm.dart';

/// Cumulative acknowledgment written to the Audio Ack characteristic.
/// [upto] is the highest sequence number stored with nothing missing before
/// it (-1 when chunk 0 has not arrived). [resendFrom] asks the device to
/// resend from that sequence number. [isFinal] acknowledges the final marker:
/// the whole segment is stored and the device can leave FINALIZE AUDIO.
class AudioAck {
  AudioAck(this.req, this.seg, this.upto, {this.resendFrom, this.isFinal = false});
  final int req, seg, upto;
  final int? resendFrom;
  final bool isFinal;

  Map<String, Object> toJson() => {
        'req': req,
        'seg': seg,
        'upto': upto,
        'resend_from': ?resendFrom,
        if (isFinal) 'final': true,
      };

  @override
  String toString() => 'AudioAck${toJson()}';
}

/// A complete, decoded voice segment.
class SegmentAudio {
  SegmentAudio(this.req, this.seg, this.rev, this.pcm, this.stats);
  final int req, seg, rev;
  final Int16List pcm; // 16 kHz mono
  final SegmentStats stats;

  double get seconds => pcm.length / 16000;

  /// Samples in [-1, 1] for Parakeet (same scaling as `pcm16ToFloat`).
  Float32List toFloat() {
    final f = Float32List(pcm.length);
    for (var i = 0; i < pcm.length; i++) {
      f[i] = pcm[i] / 32768.0;
    }
    return f;
  }
}

class SegmentStats {
  int chunks = 0, duplicates = 0, resendRequests = 0, acks = 0;
  int? firstChunkMs, finalMarkerMs, completeMs;

  /// Final marker received to segment complete. Equals the BLE part of the
  /// latency budget when nothing was missing at the end of the hold.
  int? get finalizeMs => completeMs == null ? null : completeMs! - finalMarkerMs!;

  @override
  String toString() => 'chunks=$chunks dup=$duplicates resend=$resendRequests acks=$acks '
      'stream_ms=${finalMarkerMs != null && firstChunkMs != null ? finalMarkerMs! - firstChunkMs! : null} '
      'finalize_ms=$finalizeMs';
}

class SegmentError implements Exception {
  SegmentError(this.message);
  final String message;
  @override
  String toString() => 'SegmentError: $message';
}

/// Collects the chunks of one segment (one req and seg pair). Pure logic:
/// the caller passes the time in milliseconds and calls [tick] about every
/// 50 ms, so the 200 ms ack timer is testable without real time.
class SegmentAssembler {
  SegmentAssembler(this.req, this.seg, this.rev);

  static const ackEveryChunks = 10;
  static const ackEveryMs = 200;

  final int req, seg, rev;
  final stats = SegmentStats();
  final _chunks = <int, AudioChunk>{};
  int _upto = -1;
  int _maxSeq = -1;
  int? _totalChunks, _totalSamples;
  int _sinceAck = 0;
  int _lastAckMs = 0;
  bool _changed = false;
  int? _lastResendFrom;
  int _lastResendMs = 0;
  SegmentAudio? _result;

  SegmentAudio? get result => _result;
  bool get isComplete => _result != null;

  /// Adds a chunk or the final marker. Returns an ack to write now, or null.
  /// Throws [SegmentError] if the final marker contradicts the chunks.
  AudioAck? add(AudioChunk c, int nowMs) {
    if (c.isFinal) {
      // A repeated final marker after completion means our final ack was lost.
      if (_result != null) return _ack(nowMs, isFinal: true);
      stats.finalMarkerMs ??= nowMs;
      _totalChunks = c.totalChunks;
      _totalSamples = c.totalSamples;
      if (_maxSeq >= _totalChunks!) {
        throw SegmentError('chunk $_maxSeq beyond final count $_totalChunks');
      }
      return _afterChange(nowMs, force: true);
    }
    if (_result != null || _chunks.containsKey(c.seq)) {
      stats.duplicates++;
      return null;
    }
    if (_totalChunks != null && c.seq >= _totalChunks!) {
      throw SegmentError('chunk ${c.seq} beyond final count $_totalChunks');
    }
    stats.firstChunkMs ??= nowMs;
    stats.chunks++;
    _chunks[c.seq] = c;
    if (c.seq > _maxSeq) _maxSeq = c.seq;
    while (_chunks.containsKey(_upto + 1)) {
      _upto++;
    }
    _sinceAck++;
    _changed = true;
    return _afterChange(nowMs, force: _sinceAck >= ackEveryChunks);
  }

  /// Timer: returns an ack if 200 ms passed since the last one and there is
  /// something new to report or a gap to ask for again.
  AudioAck? tick(int nowMs) {
    if (_result != null || nowMs - _lastAckMs < ackEveryMs) return null;
    if (!_changed && _missingFrom() == null) return null;
    return _ack(nowMs);
  }

  AudioAck? _afterChange(int nowMs, {required bool force}) {
    if (_tryComplete(nowMs)) return _ack(nowMs, isFinal: true);
    final missing = _missingFrom();
    // A new gap is reported at once; a known gap waits for the timer.
    final newGap = missing != null &&
        (missing != _lastResendFrom || nowMs - _lastResendMs >= ackEveryMs);
    if (force || newGap) return _ack(nowMs);
    return null;
  }

  /// First missing sequence number, if anything after it has arrived or the
  /// final marker says more chunks exist.
  int? _missingFrom() {
    final next = _upto + 1;
    if (_maxSeq >= next) return next;
    if (_totalChunks != null && next < _totalChunks!) return next;
    return null;
  }

  AudioAck _ack(int nowMs, {bool isFinal = false}) {
    final missing = isFinal ? null : _missingFrom();
    if (missing != null) {
      stats.resendRequests++;
      _lastResendFrom = missing;
      _lastResendMs = nowMs;
    }
    _sinceAck = 0;
    _lastAckMs = nowMs;
    _changed = false;
    stats.acks++;
    return AudioAck(req, seg, _upto, resendFrom: missing, isFinal: isFinal);
  }

  bool _tryComplete(int nowMs) {
    final total = _totalChunks;
    if (_result != null || total == null || _upto != total - 1) return false;
    final parts = <Int16List>[];
    var n = 0;
    for (var s = 0; s < total; s++) {
      final c = _chunks[s]!;
      final pcm = decodeImaAdpcm(c.payload, c.predictor, c.stepIndex);
      parts.add(pcm);
      n += pcm.length;
    }
    if (n < _totalSamples!) {
      throw SegmentError('final marker says $_totalSamples samples, chunks hold $n');
    }
    final pcm = Int16List(_totalSamples!);
    var o = 0;
    for (final p in parts) {
      final take = (pcm.length - o).clamp(0, p.length);
      pcm.setRange(o, o + take, p);
      o += take;
    }
    stats.completeMs = nowMs;
    _chunks.clear();
    _result = SegmentAudio(req, seg, rev, pcm, stats);
    return true;
  }
}

/// Routes Audio notifications to one assembler per (req, seg), reports acks
/// and finished segments, and counts chunks it had to reject.
class AudioReceiver {
  AudioReceiver({required this.onAck, required this.onSegment, this.onError});

  /// Finished segments kept so a repeated final marker gets re-acknowledged.
  static const keepSegments = 8;

  final void Function(AudioAck ack) onAck;
  final void Function(SegmentAudio audio) onSegment;
  final void Function(Object error)? onError;
  final _segments = <(int, int), SegmentAssembler>{};
  int rejectedChunks = 0;

  void onNotification(Uint8List bytes, int nowMs) {
    final AudioChunk c;
    try {
      c = AudioChunk.parse(bytes);
    } on ChunkError catch (e) {
      rejectedChunks++; // treated as missing; the gap triggers resend_from
      onError?.call(e);
      return;
    }
    final key = (c.req, c.seg);
    final a = _segments.putIfAbsent(key, () => SegmentAssembler(c.req, c.seg, c.rev));
    final wasComplete = a.isComplete;
    try {
      final ack = a.add(c, nowMs);
      if (ack != null) onAck(ack);
    } on SegmentError catch (e) {
      _segments.remove(key);
      onError?.call(e);
      return;
    }
    if (!wasComplete && a.isComplete) onSegment(a.result!);
    _trim();
  }

  void tick(int nowMs) {
    for (final a in _segments.values) {
      final ack = a.tick(nowMs);
      if (ack != null) onAck(ack);
    }
  }

  /// Drops every unfinished segment of a request (cancel, or a new request).
  void dropRequest(int req) => _segments.removeWhere((k, a) => k.$1 == req && !a.isComplete);

  int get activeSegments => _segments.values.where((a) => !a.isComplete).length;

  void _trim() {
    final done = _segments.entries.where((e) => e.value.isComplete).map((e) => e.key).toList();
    for (final k in done.take((done.length - keepSegments).clamp(0, done.length))) {
      _segments.remove(k);
    }
  }
}
