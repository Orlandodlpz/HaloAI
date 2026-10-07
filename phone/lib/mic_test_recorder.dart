import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:record/record.dart';

/// TEST ONLY. Records from the phone's own microphone so speech to text can
/// be tested live before the device exists. In the real product, audio
/// comes from the device over BLE (P1); this file is not part of that path.
class MicTestRecorder {
  final _recorder = AudioRecorder();
  final _bytes = BytesBuilder(copy: false);
  StreamSubscription<Uint8List>? _sub;
  Completer<void>? _done;

  static const sampleRate = 16000;

  Future<bool> ensurePermission() => _recorder.hasPermission();

  Future<void> start() async {
    _bytes.clear();
    final stream = await _recorder.startStream(const RecordConfig(
      encoder: AudioEncoder.pcm16bits,
      sampleRate: sampleRate,
      numChannels: 1,
      // voiceRecognition with gain and noise processing off is the closest the
      // phone gets to a raw mic, which is what the device will send.
      autoGain: false,
      echoCancel: false,
      noiseSuppress: false,
      androidConfig: AndroidRecordConfig(audioSource: AndroidAudioSource.voiceRecognition),
    ));
    final done = Completer<void>();
    _done = done;
    _sub = stream.listen(_bytes.add, onDone: done.complete);
  }

  /// Stops and returns the recording as 16-bit little-endian PCM bytes.
  Future<Uint8List> stop() async {
    await _recorder.stop();
    // Wait for the last chunks to arrive; the stream closes after stop.
    await _done?.future.timeout(const Duration(seconds: 1), onTimeout: () {});
    await _sub?.cancel();
    _sub = null;
    return _bytes.takeBytes();
  }

  Future<void> dispose() => _recorder.dispose();
}

/// 16-bit PCM bytes to floats in [-1, 1], the format Parakeet takes.
Float32List pcm16ToFloat(Uint8List pcm) {
  final data = ByteData.sublistView(pcm);
  final n = pcm.length ~/ 2;
  final out = Float32List(n);
  for (var i = 0; i < n; i++) {
    out[i] = data.getInt16(i * 2, Endian.little) / 32768.0;
  }
  return out;
}

/// Writes 16-bit mono PCM as a standard WAV file (the same format as the
/// P0 test clips), so good recordings can be reused as test clips.
Future<void> writeWav(String path, Uint8List pcm, int sampleRate) async {
  final header = ByteData(44);
  void ascii(int offset, String s) {
    for (var i = 0; i < s.length; i++) {
      header.setUint8(offset + i, s.codeUnitAt(i));
    }
  }

  ascii(0, 'RIFF');
  header.setUint32(4, 36 + pcm.length, Endian.little);
  ascii(8, 'WAVE');
  ascii(12, 'fmt ');
  header.setUint32(16, 16, Endian.little); // fmt chunk size
  header.setUint16(20, 1, Endian.little); // PCM
  header.setUint16(22, 1, Endian.little); // mono
  header.setUint32(24, sampleRate, Endian.little);
  header.setUint32(28, sampleRate * 2, Endian.little); // byte rate
  header.setUint16(32, 2, Endian.little); // block align
  header.setUint16(34, 16, Endian.little); // bits per sample
  ascii(36, 'data');
  header.setUint32(40, pcm.length, Endian.little);
  final f = File(path);
  await f.parent.create(recursive: true);
  await f.writeAsBytes([...header.buffer.asUint8List(), ...pcm], flush: true);
}
