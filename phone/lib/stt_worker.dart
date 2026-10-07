import 'dart:async';
import 'dart:isolate';
import 'dart:typed_data';

import 'package:sherpa_onnx/sherpa_onnx.dart' as sherpa;

/// Parakeet v3 in a long-lived background isolate. The model loads once
/// (about 1.7 s) and stays in memory; each request sends finished audio and
/// gets text back. Audio source does not matter here: the phone mic test
/// button today, decoded BLE ADPCM from the device in P1.
class SttWorker {
  SttWorker._(this._isolate, this._requests, this.loadMs);

  final Isolate _isolate;
  final SendPort _requests;
  final int loadMs;
  final _pending = <Completer<SttResult>>[];

  static const numThreads = 4;

  static Future<SttWorker> start(String modelDir) async {
    final replies = ReceivePort();
    final isolate = await Isolate.spawn(_workerMain, [replies.sendPort, modelDir]);
    final ready = Completer<(SendPort, int)>();
    late SttWorker worker;
    replies.listen((msg) {
      if (!ready.isCompleted) {
        if (msg is String) {
          ready.completeError(StateError(msg));
        } else {
          final m = msg as List;
          ready.complete((m[0] as SendPort, m[1] as int));
        }
        return;
      }
      // Requests are handled one at a time in order, so replies match FIFO.
      final c = worker._pending.removeAt(0);
      final m = msg as List;
      if (m[0] == 'error') {
        c.completeError(StateError(m[1] as String));
      } else {
        c.complete(SttResult(m[1] as String, m[2] as double, m[3] as int));
      }
    });
    final (port, loadMs) = await ready.future;
    worker = SttWorker._(isolate, port, loadMs);
    return worker;
  }

  /// Transcribes 16 kHz mono float samples in [-1, 1].
  Future<SttResult> transcribeSamples(Float32List samples, int sampleRate) =>
      _send(['pcm', samples, sampleRate]);

  /// Transcribes a WAV file on disk.
  Future<SttResult> transcribeWav(String path) => _send(['wav', path]);

  Future<SttResult> _send(List msg) {
    final c = Completer<SttResult>();
    _pending.add(c);
    _requests.send(msg);
    return c.future;
  }

  void dispose() => _isolate.kill();
}

class SttResult {
  SttResult(this.text, this.audioSec, this.decodeMs);
  final String text;
  final double audioSec;
  final int decodeMs;
  double get rtf => decodeMs / 1000 / audioSec;
}

void _workerMain(List args) {
  final replies = args[0] as SendPort;
  final modelDir = args[1] as String;
  final sherpa.OfflineRecognizer recognizer;
  final load = Stopwatch()..start();
  try {
    sherpa.initBindings();
    recognizer = sherpa.OfflineRecognizer(
      sherpa.OfflineRecognizerConfig(
        model: sherpa.OfflineModelConfig(
          transducer: sherpa.OfflineTransducerModelConfig(
            encoder: '$modelDir/encoder.int8.onnx',
            decoder: '$modelDir/decoder.int8.onnx',
            joiner: '$modelDir/joiner.int8.onnx',
          ),
          tokens: '$modelDir/tokens.txt',
          modelType: 'nemo_transducer',
          numThreads: SttWorker.numThreads,
          debug: false,
        ),
      ),
    );
  } catch (e) {
    replies.send('Model load failed: $e');
    return;
  }
  load.stop();

  final requests = ReceivePort();
  replies.send([requests.sendPort, load.elapsedMilliseconds]);
  requests.listen((msg) {
    final m = msg as List;
    try {
      final Float32List samples;
      final int sampleRate;
      if (m[0] == 'wav') {
        final wave = sherpa.readWave(m[1] as String);
        samples = wave.samples;
        sampleRate = wave.sampleRate;
      } else {
        samples = m[1] as Float32List;
        sampleRate = m[2] as int;
      }
      final sw = Stopwatch()..start();
      final stream = recognizer.createStream();
      stream.acceptWaveform(samples: samples, sampleRate: sampleRate);
      recognizer.decode(stream);
      final text = recognizer.getResult(stream).text;
      sw.stop();
      stream.free();
      replies.send(['ok', text, samples.length / sampleRate, sw.elapsedMilliseconds]);
    } catch (e) {
      replies.send(['error', '$e']);
    }
  });
}
