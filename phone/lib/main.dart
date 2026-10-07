import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:path_provider/path_provider.dart';

import 'mic_test_recorder.dart';
import 'stt_worker.dart';

/// Speech to text test bench. Runs fully on the phone: the release manifest
/// declares no INTERNET permission.
///
/// - Run clips: the P0 benchmark over the bundled test clips.
/// - Hold to talk (TEST ONLY): records from the phone mic while held and
///   transcribes on release, mimicking the device's hold gesture. Every
///   recording is saved as a WAV in files/recordings/ with a log.tsv line.

const _clips = [
  'block_3_4_tmrw_gym',
  'calendar_12_2_meeting',
  'delete_meetings_thursday',
  'email_meeting_moved',
  'email_tomai_meeting_sick',
];

// Shorter presses are discarded, like the device's hold threshold (about
// 500 ms, provisional, owned by the device). Approximation for testing only.
const _minHold = Duration(milliseconds: 500);

class Entry {
  Entry(this.label, this.r, {this.releaseToTextMs});
  final String label;
  final SttResult r;
  final int? releaseToTextMs;
}

void main() => runApp(const MaterialApp(home: BenchPage()));

class BenchPage extends StatefulWidget {
  const BenchPage({super.key});
  @override
  State<BenchPage> createState() => _BenchPageState();
}

class _BenchPageState extends State<BenchPage> {
  final _mic = MicTestRecorder();
  final _entries = <Entry>[];
  SttWorker? _stt;
  String? _baseDir;
  String _status = 'Loading model...';
  bool _busy = false;
  bool _recording = false;
  // Pointer events arrive synchronously, but starting the mic is async. These
  // flags make a release during startup stop the recording instead of being lost.
  bool _starting = false;
  bool _fingerDown = false;
  final _holdTimer = Stopwatch();

  @override
  void initState() {
    super.initState();
    _init();
  }

  @override
  void dispose() {
    _stt?.dispose();
    _mic.dispose();
    super.dispose();
  }

  Future<void> _init() async {
    try {
      final ext = await getExternalStorageDirectory();
      _baseDir = ext!.path;
      final modelDir = '$_baseDir/models/parakeet-v3';
      // The app must create this folder itself: on Android 11+ a folder made by
      // `adb shell` is owned by the shell user and its files stay invisible here.
      Directory(modelDir).createSync(recursive: true);
      if (!File('$modelDir/encoder.int8.onnx').existsSync()) {
        final seen = Directory(modelDir).listSync().map((e) => e.path.split('/').last);
        setState(() => _status = 'Model missing. Push it to:\n$modelDir\nSeen: ${seen.join(', ')}');
        return;
      }
      final stt = await SttWorker.start(modelDir);
      // ignore: avoid_print
      print('P0RESULT load=${stt.loadMs}ms threads=${SttWorker.numThreads}');
      setState(() {
        _stt = stt;
        _status = 'Ready. Model load ${stt.loadMs} ms, ${SttWorker.numThreads} threads.';
      });
      await _mic.ensurePermission();
    } catch (e, st) {
      debugPrint('P0ERROR $e\n$st');
      setState(() => _status = 'Error: $e');
    }
  }

  Future<void> _runClips() async {
    final stt = _stt!;
    setState(() {
      _busy = true;
      _status = 'Running clips...';
    });
    try {
      final tmp = await getTemporaryDirectory();
      final wavDir = Directory('${tmp.path}/clips')..createSync(recursive: true);
      for (final n in _clips) {
        final data = await rootBundle.load('assets/clips/$n.wav');
        await File('${wavDir.path}/$n.wav').writeAsBytes(data.buffer.asUint8List(), flush: true);
      }
      // Pass 1 is cold (first decode pays one-time costs), pass 2 is warm.
      for (var pass = 1; pass <= 2; pass++) {
        for (final n in _clips) {
          final r = await stt.transcribeWav('${wavDir.path}/$n.wav');
          // ignore: avoid_print
          print('P0RESULT pass=$pass clip=$n audio=${r.audioSec.toStringAsFixed(2)}s '
              'decode=${r.decodeMs}ms rtf=${r.rtf.toStringAsFixed(2)} text="${r.text}"');
          setState(() => _entries.insert(0, Entry('pass $pass | $n', r)));
        }
      }
      setState(() => _status = 'Clips done.');
    } catch (e, st) {
      debugPrint('P0ERROR $e\n$st');
      setState(() => _status = 'Error: $e');
    } finally {
      setState(() => _busy = false);
    }
  }

  Future<void> _holdStart() async {
    _fingerDown = true;
    if (_stt == null || _busy || _recording || _starting) return;
    _starting = true;
    try {
      if (!await _mic.ensurePermission()) {
        setState(() => _status = 'Microphone permission denied.');
        return;
      }
      _holdTimer
        ..reset()
        ..start();
      // The mic takes a few hundred ms to start. Vibrate only once it is
      // actually recording, so the buzz means "talk now".
      await _mic.start();
      if (!_fingerDown) {
        // Released while the mic was still starting: a tap, not a hold.
        await _mic.stop();
        setState(() => _status = 'Too short. Hold longer while talking.');
        return;
      }
      HapticFeedback.mediumImpact();
      setState(() {
        _recording = true;
        _status = 'Recording... release to transcribe.';
      });
    } finally {
      _starting = false;
    }
  }

  Future<void> _holdEnd() async {
    _fingerDown = false;
    if (!_recording) return;
    final release = Stopwatch()..start();
    _holdTimer.stop();
    final pcm = await _mic.stop();
    HapticFeedback.lightImpact();
    setState(() {
      _recording = false;
      _busy = true;
    });
    try {
      if (_holdTimer.elapsed < _minHold) {
        setState(() => _status = 'Too short. Hold longer while talking.');
        return;
      }
      setState(() => _status = 'Transcribing...');
      final r = await _stt!.transcribeSamples(pcm16ToFloat(pcm), MicTestRecorder.sampleRate);
      final total = release.elapsedMilliseconds;
      final name = 'rec_${_stamp(DateTime.now())}';
      final dir = '$_baseDir/recordings';
      await writeWav('$dir/$name.wav', pcm, MicTestRecorder.sampleRate);
      await File('$dir/log.tsv').writeAsString(
        '$name.wav\t${r.audioSec.toStringAsFixed(2)}\t${r.decodeMs}\t$total\t${r.text}\n',
        mode: FileMode.append,
        flush: true,
      );
      // ignore: avoid_print
      print('MICRESULT file=$name.wav audio=${r.audioSec.toStringAsFixed(2)}s '
          'decode=${r.decodeMs}ms release_to_text=${total}ms text="${r.text}"');
      setState(() {
        _entries.insert(0, Entry('mic | $name', r, releaseToTextMs: total));
        _status = 'Release to text $total ms. Saved $name.wav';
      });
    } catch (e, st) {
      debugPrint('P0ERROR $e\n$st');
      setState(() => _status = 'Error: $e');
    } finally {
      setState(() => _busy = false);
    }
  }

  static String _stamp(DateTime t) {
    String two(int v) => v.toString().padLeft(2, '0');
    return '${t.year}${two(t.month)}${two(t.day)}_${two(t.hour)}${two(t.minute)}${two(t.second)}';
  }

  @override
  Widget build(BuildContext context) {
    final ready = _stt != null && !_busy;
    return Scaffold(
      appBar: AppBar(title: const Text('HaloAI speech to text test')),
      body: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
          Text(_status),
          const SizedBox(height: 8),
          OutlinedButton(onPressed: ready ? _runClips : null, child: const Text('Run clips')),
          const SizedBox(height: 16),
          // Listener instead of a button: recording starts on finger down and
          // ends on finger up, with no tap or long press delay in between.
          Listener(
            onPointerDown: (_) => _holdStart(),
            onPointerUp: (_) => _holdEnd(),
            onPointerCancel: (_) => _holdEnd(),
            child: Container(
              height: 120,
              alignment: Alignment.center,
              decoration: BoxDecoration(
                color: _recording
                    ? Colors.red
                    : ready
                        ? Theme.of(context).colorScheme.primary
                        : Colors.grey,
                borderRadius: BorderRadius.circular(24),
              ),
              child: Text(
                _recording ? 'Recording... release to send' : 'Hold to talk (test mic)',
                style: const TextStyle(color: Colors.white, fontSize: 20),
              ),
            ),
          ),
          const SizedBox(height: 8),
          Expanded(
            child: ListView(children: [
              for (final e in _entries)
                ListTile(
                  dense: true,
                  title: Text(e.r.text.isEmpty ? '(no speech recognized)' : e.r.text),
                  subtitle: Text('${e.label} | ${e.r.audioSec.toStringAsFixed(1)} s audio | '
                      'decode ${e.r.decodeMs} ms'
                      '${e.releaseToTextMs == null ? '' : ' | release to text ${e.releaseToTextMs} ms'}'),
                ),
            ]),
          ),
        ]),
      ),
    );
  }
}
