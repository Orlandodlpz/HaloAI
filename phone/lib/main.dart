import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:path_provider/path_provider.dart';
import 'package:sherpa_onnx/sherpa_onnx.dart' as sherpa;

/// P0: offline Parakeet v3 transcription benchmark. No network permission is
/// declared in the release manifest, so any success here is on-device only.

const _clips = [
  'block_3_4_tmrw_gym',
  'calendar_12_2_meeting',
  'delete_meetings_thursday',
  'email_meeting_moved',
  'email_tomai_meeting_sick',
];
const _numThreads = 4;

class ClipResult {
  ClipResult(this.name, this.pass, this.text, this.audioSec, this.decodeMs);
  final String name;
  final int pass;
  final String text;
  final double audioSec;
  final int decodeMs;
  double get rtf => decodeMs / 1000 / audioSec;
}

class BenchResult {
  BenchResult(this.loadMs, this.clips);
  final int loadMs;
  final List<ClipResult> clips;
}

class BenchArgs {
  BenchArgs(this.modelDir, this.wavDir);
  final String modelDir;
  final String wavDir;
}

// Runs in a background isolate so loading a ~650 MB model never blocks the UI.
BenchResult runBench(BenchArgs a) {
  sherpa.initBindings();
  final load = Stopwatch()..start();
  final recognizer = sherpa.OfflineRecognizer(
    sherpa.OfflineRecognizerConfig(
      model: sherpa.OfflineModelConfig(
        transducer: sherpa.OfflineTransducerModelConfig(
          encoder: '${a.modelDir}/encoder.int8.onnx',
          decoder: '${a.modelDir}/decoder.int8.onnx',
          joiner: '${a.modelDir}/joiner.int8.onnx',
        ),
        tokens: '${a.modelDir}/tokens.txt',
        modelType: 'nemo_transducer',
        numThreads: _numThreads,
        debug: false,
      ),
    ),
  );
  load.stop();
  final out = <ClipResult>[];
  // Pass 1 is cold (first decode pays one-time costs), pass 2 is warm.
  for (var pass = 1; pass <= 2; pass++) {
    for (final name in _clips) {
      final wave = sherpa.readWave('${a.wavDir}/$name.wav');
      final sw = Stopwatch()..start();
      final stream = recognizer.createStream();
      stream.acceptWaveform(samples: wave.samples, sampleRate: wave.sampleRate);
      recognizer.decode(stream);
      final text = recognizer.getResult(stream).text;
      sw.stop();
      stream.free();
      final r = ClipResult(name, pass, text, wave.samples.length / wave.sampleRate,
          sw.elapsedMilliseconds);
      out.add(r);
      // ignore: avoid_print
      print('P0RESULT pass=$pass clip=$name audio=${r.audioSec.toStringAsFixed(2)}s '
          'decode=${r.decodeMs}ms rtf=${r.rtf.toStringAsFixed(2)} text="$text"');
    }
  }
  recognizer.free();
  // ignore: avoid_print
  print('P0RESULT load=${load.elapsedMilliseconds}ms threads=$_numThreads');
  return BenchResult(load.elapsedMilliseconds, out);
}

void main() => runApp(const MaterialApp(home: BenchPage()));

class BenchPage extends StatefulWidget {
  const BenchPage({super.key});
  @override
  State<BenchPage> createState() => _BenchPageState();
}

class _BenchPageState extends State<BenchPage> {
  String _status = 'Tap Run to transcribe the test clips.';
  BenchResult? _result;
  bool _busy = false;

  Future<void> _run() async {
    setState(() {
      _busy = true;
      _status = 'Running...';
    });
    try {
      final ext = await getExternalStorageDirectory();
      final modelDir = '${ext!.path}/models/parakeet-v3';
      // The app must create this folder itself: on Android 11+ a folder made by
      // `adb shell` is owned by the shell user and its files stay invisible here.
      Directory(modelDir).createSync(recursive: true);
      if (!File('$modelDir/encoder.int8.onnx').existsSync()) {
        final seen = Directory(modelDir).listSync().map((e) => e.path.split('/').last);
        setState(() => _status = 'Model missing. Push it to:\n$modelDir\nSeen: ${seen.join(', ')}');
        return;
      }
      final tmp = await getTemporaryDirectory();
      final wavDir = Directory('${tmp.path}/clips')..createSync(recursive: true);
      for (final n in _clips) {
        final data = await rootBundle.load('assets/clips/$n.wav');
        await File('${wavDir.path}/$n.wav')
            .writeAsBytes(data.buffer.asUint8List(), flush: true);
      }
      // compute() takes a top-level function, so no widget state is captured
      // and sent to the background isolate.
      final r = await compute(runBench, BenchArgs(modelDir, wavDir.path));
      setState(() {
        _result = r;
        _status = 'Done. Model load ${r.loadMs} ms, $_numThreads threads.';
      });
    } catch (e, st) {
      debugPrint('P0ERROR $e\n$st');
      setState(() => _status = 'Error: $e');
    } finally {
      setState(() => _busy = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    final clips = _result?.clips ?? const <ClipResult>[];
    return Scaffold(
      appBar: AppBar(title: const Text('HaloAI P0: Parakeet v3')),
      body: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Text(_status),
          const SizedBox(height: 8),
          FilledButton(onPressed: _busy ? null : _run, child: const Text('Run')),
          const SizedBox(height: 8),
          Expanded(
            child: ListView(children: [
              for (final c in clips)
                ListTile(
                  dense: true,
                  title: Text(c.text),
                  subtitle: Text('pass ${c.pass} | ${c.name} | ${c.audioSec.toStringAsFixed(1)} s audio | '
                      '${c.decodeMs} ms | RTF ${c.rtf.toStringAsFixed(2)}'),
                ),
            ]),
          ),
        ]),
      ),
    );
  }
}
