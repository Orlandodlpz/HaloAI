import 'dart:async';
import 'dart:isolate';

import 'package:flutter/foundation.dart';
import 'package:sherpa_onnx/sherpa_onnx.dart' as sherpa;

/// Parakeet v3 in a long-lived background isolate. The model loads once
/// (about 1.7 s) and stays in memory; each request sends finished audio and
/// gets text back. Audio source does not matter here: the phone mic test
/// button, or decoded BLE ADPCM from the device (P1).
///
/// Each request can carry an opaque [tag] (for example the segment it came
/// from), returned on the result, so the caller can drop a result for a
/// segment that was cancelled or replaced while it was being decoded.
///
/// If the worker isolate dies, every pending request fails with
/// [SttWorkerDead] and later requests fail immediately; [isAlive] turns false.
/// The phone only reports its capture service ready while [isAlive] is true.
class SttWorker {
  SttWorker._(this._isolate, this._requests, this.loadMs);

  final Isolate _isolate;
  final SendPort _requests;
  final int loadMs;
  final _pending = <int, (Completer<SttResult>, Object?)>{};
  int _nextId = 0;
  Object? _deathReason;
  final _ports = <ReceivePort>[];

  static const numThreads = 4;

  bool get isAlive => _deathReason == null;

  static Future<SttWorker> start(String modelDir) =>
      startWith(_workerMain, modelDir);

  /// [entry] must be a top-level function taking `[SendPort, String]`.
  /// Tests pass a fake worker; the app always uses [start].
  @visibleForTesting
  static Future<SttWorker> startWith(
      void Function(List) entry, String modelDir) async {
    final replies = ReceivePort();
    final exits = ReceivePort();
    final errors = ReceivePort();
    final isolate = await Isolate.spawn(entry, [replies.sendPort, modelDir],
        onExit: exits.sendPort, onError: errors.sendPort);
    final ready = Completer<(SendPort, int)>();
    SttWorker? worker;
    Object? deathBeforeWorker; // death between load and the worker object existing

    void die(Object reason) {
      if (!ready.isCompleted) {
        ready.completeError(reason);
      }
      if (worker == null) deathBeforeWorker ??= reason;
      worker?._die(reason);
      for (final p in [replies, exits, errors]) {
        p.close();
      }
    }

    exits.listen((_) => die(SttWorkerDead('worker exited')));
    errors.listen((e) => die(SttWorkerDead('worker crashed: ${(e as List)[0]}')));
    replies.listen((msg) {
      if (!ready.isCompleted) {
        if (msg is String) {
          die(StateError(msg));
        } else {
          final m = msg as List;
          ready.complete((m[0] as SendPort, m[1] as int));
        }
        return;
      }
      final m = msg as List;
      final entry = worker!._pending.remove(m[1] as int);
      if (entry == null) return;
      final (c, tag) = entry;
      if (m[0] == 'error') {
        c.completeError(StateError(m[2] as String));
      } else {
        c.complete(SttResult(m[2] as String, m[3] as double, m[4] as int, tag: tag));
      }
    });
    final (port, loadMs) = await ready.future;
    worker = SttWorker._(isolate, port, loadMs).._ports.addAll([replies, exits, errors]);
    if (deathBeforeWorker != null) worker._die(deathBeforeWorker!);
    return worker;
  }

  /// Transcribes 16 kHz mono float samples in [-1, 1].
  Future<SttResult> transcribeSamples(Float32List samples, int sampleRate, {Object? tag}) =>
      _send(['pcm', samples, sampleRate], tag);

  /// Transcribes a WAV file on disk.
  Future<SttResult> transcribeWav(String path, {Object? tag}) => _send(['wav', path], tag);

  Future<SttResult> _send(List msg, Object? tag) {
    if (_deathReason != null) return Future.error(_deathReason!);
    final id = _nextId++;
    final c = Completer<SttResult>();
    _pending[id] = (c, tag);
    _requests.send([id, ...msg]);
    return c.future;
  }

  void _die(Object reason) {
    _deathReason ??= reason;
    for (final (c, _) in _pending.values) {
      c.completeError(reason);
    }
    _pending.clear();
  }

  void dispose() {
    _die(SttWorkerDead('disposed'));
    _isolate.kill(priority: Isolate.immediate);
    for (final p in _ports) {
      p.close();
    }
  }
}

class SttWorkerDead implements Exception {
  SttWorkerDead(this.message);
  final String message;
  @override
  String toString() => 'SttWorkerDead: $message';
}

class SttResult {
  SttResult(this.text, this.audioSec, this.decodeMs, {this.tag});
  final String text;
  final double audioSec;
  final int decodeMs;
  final Object? tag;
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
  // Message: [id, kind, ...]. Reply: ['ok', id, text, audioSec, decodeMs] or ['error', id, msg].
  requests.listen((msg) {
    final m = msg as List;
    final id = m[0] as int;
    try {
      final Float32List samples;
      final int sampleRate;
      if (m[1] == 'wav') {
        final wave = sherpa.readWave(m[2] as String);
        samples = wave.samples;
        sampleRate = wave.sampleRate;
      } else {
        samples = m[2] as Float32List;
        sampleRate = m[3] as int;
      }
      final sw = Stopwatch()..start();
      final stream = recognizer.createStream();
      stream.acceptWaveform(samples: samples, sampleRate: sampleRate);
      recognizer.decode(stream);
      final text = recognizer.getResult(stream).text;
      sw.stop();
      stream.free();
      replies.send(['ok', id, text, samples.length / sampleRate, sw.elapsedMilliseconds]);
    } catch (e) {
      replies.send(['error', id, '$e']);
    }
  });
}
