import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_timezone/flutter_timezone.dart';
import 'package:path_provider/path_provider.dart';

import 'backend/backend_client.dart';
import 'backend/request_controller.dart';
import 'ble/device_session.dart';
import 'ble/device_transport.dart';
import 'ble/fbp_transport.dart';
import 'ble/loopback.dart';
import 'mic_test_recorder.dart';
import 'stt_worker.dart';

/// HaloAI phone test bench. Speech to text runs fully on the phone; only
/// text goes to the backend (P2), never audio.
///
/// - Run clips: the P0 benchmark over the bundled test clips.
/// - Hold to talk (TEST ONLY): records from the phone mic while held and
///   transcribes on release, mimicking the device's hold gesture. Every
///   recording is saved as a WAV in files/recordings/ with a log.tsv line.
/// - Device (P1): connects to the HaloAI device over BLE, receives audio,
///   transcribes each finished segment (BLERESULT lines in logcat), and can
///   send a test screen.
/// - Loopback (P1): the test clips through the whole BLE audio path (encode,
///   chunks with some lost on purpose, receiver, decode) without a radio,
///   compared with the original WAV's transcript (LOOPRESULT lines).
/// - Backend (P2): typed text (Send, Add) or a mic hold goes to the backend;
///   the panel shows the screen exactly as the device gets it, with phone
///   side Approve, Continue, Cancel and candidate buttons (APIRESULT lines).
///   Needs `--dart-define-from-file=config/backend.json`.

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

  DeviceSession? _device;
  LinkStatus _link = const LinkStatus(LinkState.off);
  final _deviceLog = <String>[];
  final _deviceSubs = <StreamSubscription>[];

  RequestController? _ctl;
  ScreenView? _view;
  final _apiLog = <String>[];
  final _text = TextEditingController();

  @override
  void initState() {
    super.initState();
    _initBackend();
    _init();
  }

  Future<void> _initBackend() async {
    final config = BackendConfig.fromEnvironment();
    if (config == null) {
      setState(() => _apiLog.insert(0, 'Backend off: build with --dart-define-from-file=config/backend.json'));
      return;
    }
    var tz = 'America/Chicago';
    try {
      tz = (await FlutterTimezone.getLocalTimezone()).identifier;
    } catch (e) {
      debugPrint('APILOG timezone lookup failed, using $tz: $e');
    }
    final ctl = RequestController(api: BackendClient(config), timezone: tz);
    ctl.views.listen((v) => setState(() => _view = v));
    ctl.log.listen((l) {
      debugPrint('APILOG $l');
      setState(() {
        _apiLog.insert(0, l);
        if (_apiLog.length > 4) _apiLog.removeLast();
      });
    });
    _device?.timezone = tz;
    ctl.attachDevice(_device);
    setState(() => _ctl = ctl);
  }

  /// A hold while a proposal or question is on screen adds to it, as on the
  /// device; otherwise it starts a new request.
  Future<void> _sendSpoken(String text) async {
    final ctl = _ctl;
    if (ctl == null) return;
    const open = {'review', 'conflict_ambiguous', 'conflict_schedule', 'processing'};
    if (open.contains(ctl.current?.state)) {
      await ctl.add(text);
    } else {
      await ctl.say(text);
    }
  }

  @override
  void dispose() {
    for (final s in _deviceSubs) {
      s.cancel();
    }
    _device?.stop();
    _ctl?.dispose();
    _text.dispose();
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
      await _device?.phoneReadyChanged();
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
      unawaited(_sendSpoken(r.text));
    } catch (e, st) {
      debugPrint('P0ERROR $e\n$st');
      setState(() => _status = 'Error: $e');
    } finally {
      setState(() => _busy = false);
    }
  }

  Future<void> _toggleDevice() async {
    if (_device != null) {
      for (final s in _deviceSubs) {
        await s.cancel();
      }
      _deviceSubs.clear();
      _ctl?.attachDevice(null);
      await _device!.stop();
      setState(() {
        _device = null;
        _link = const LinkStatus(LinkState.off);
      });
      return;
    }
    final session = DeviceSession(
      transport: FbpTransport(),
      sttReady: () => _stt?.isAlive ?? false,
      transcribe: (samples, sr, {tag}) {
        final stt = _stt;
        if (stt == null) throw StateError('speech model not loaded');
        return stt.transcribeSamples(samples, sr, tag: tag);
      },
    );
    _deviceSubs
      ..add(session.log.listen((l) {
        debugPrint('BLELOG $l');
        setState(() {
          _link = session.status;
          _deviceLog.insert(0, l);
          if (_deviceLog.length > 6) _deviceLog.removeLast();
        });
      }))
      ..add(session.transcripts.listen((t) => setState(() {
            _entries.insert(0, Entry('device | req ${t.req} seg ${t.seg} rev ${t.rev}', t.result,
                releaseToTextMs: t.releaseToTextMs));
            _status = 'Device: release to text ${t.releaseToTextMs} ms '
                '(BLE ${t.releaseToCompleteMs} ms, ${t.stats.resendRequests} resend requests)';
          })));
    session.timezone = _ctl?.timezone;
    _ctl?.attachDevice(session);
    setState(() => _device = session);
    await session.start();
  }

  Future<void> _sendTestScreen() async {
    try {
      await _device!.sendScreen('REVIEW',
          requestId: 'test',
          revision: 1,
          proposalHash: 'test',
          screenLines: ['To: victor@example.com', 'Subject: Meeting moved', 'The meeting moved to', 'Thursday at 3 pm.'],
          statusLine: 'Ready to confirm',
          expiresInS: 60);
    } catch (e) {
      setState(() => _status = 'Screen write failed: $e');
    }
  }

  Future<void> _runLoopback() async {
    final stt = _stt!;
    setState(() {
      _busy = true;
      _status = 'Loopback...';
    });
    var same = 0;
    try {
      for (final n in _clips) {
        final data = await rootBundle.load('assets/clips/$n.wav');
        final pcm = wavPcm16(data.buffer.asUint8List(data.offsetInBytes, data.lengthInBytes));
        final orig = Float32List.fromList([for (final v in pcm) v / 32768.0]);
        final sw = Stopwatch()..start();
        final lb = loopback(pcm);
        final pathMs = sw.elapsedMilliseconds;
        final a = await stt.transcribeSamples(orig, 16000);
        final b = await stt.transcribeSamples(lb.audio.toFloat(), 16000);
        if (a.text == b.text) same++;
        // ignore: avoid_print
        print('LOOPRESULT clip=$n same=${a.text == b.text} lost=${lb.lost} resend=${lb.resendRequests} '
            'path=${pathMs}ms decode=${b.decodeMs}ms original="${a.text}" loopback="${b.text}"');
        setState(() => _entries.insert(0, Entry('loopback | $n | ${a.text == b.text ? 'same' : 'DIFFERENT'} '
            '| lost ${lb.lost}', b)));
      }
      setState(() => _status = 'Loopback: $same of ${_clips.length} transcripts identical to the original WAV.');
    } catch (e, st) {
      debugPrint('P1ERROR $e\n$st');
      setState(() => _status = 'Error: $e');
    } finally {
      setState(() => _busy = false);
    }
  }

  Widget _backendPanel(BuildContext context) {
    final ctl = _ctl;
    final small = Theme.of(context).textTheme.bodySmall;
    final v = _view;
    final state = ctl?.current?.state;
    void run(Future<void> Function() f) => unawaited(f());
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(8),
        child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
          Row(children: [
            Expanded(
              child: TextField(
                controller: _text,
                enabled: ctl != null,
                decoration: const InputDecoration(isDense: true, hintText: 'Type a command'),
              ),
            ),
            TextButton(
                onPressed: ctl == null ? null : () => run(() => ctl.say(_text.text)), child: const Text('Send')),
            TextButton(
                onPressed: ctl?.currentRequestId == null ? null : () => run(() => ctl!.add(_text.text)),
                child: const Text('Add')),
          ]),
          if (v != null) ...[
            const SizedBox(height: 4),
            Text('${v.screen}  rev ${v.revision}${v.expiresInS == null ? '' : '  ${v.expiresInS} s'}'
                '${v.pendingKey == null ? '' : '  key'}',
                style: Theme.of(context).textTheme.labelLarge),
            if (v.statusLine != null) Text(v.statusLine!, style: small),
            for (final l in v.lines) Text(l, style: const TextStyle(fontFamily: 'monospace')),
          ],
          Wrap(spacing: 4, children: [
            for (var i = 0; i < (v?.candidates.length ?? 0); i++)
              OutlinedButton(
                  onPressed: state == 'conflict_ambiguous' ? () => run(() => ctl!.select(i)) : null,
                  child: Text(v!.candidates[i], style: small)),
            FilledButton(
                onPressed: state == 'review' ? () => run(ctl!.approve) : null, child: const Text('Approve')),
            OutlinedButton(
                onPressed: state == 'conflict_schedule' ? () => run(ctl!.continueAnyway) : null,
                child: const Text('Continue')),
            OutlinedButton(
                onPressed: ctl?.currentRequestId == null ? null : () => run(ctl!.cancel), child: const Text('Cancel')),
          ]),
          for (final l in _apiLog.take(2)) Text(l, maxLines: 1, overflow: TextOverflow.ellipsis, style: small),
        ]),
      ),
    );
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
          Row(children: [
            Expanded(child: OutlinedButton(onPressed: ready ? _runClips : null, child: const Text('Run clips'))),
            const SizedBox(width: 8),
            Expanded(child: OutlinedButton(onPressed: ready ? _runLoopback : null, child: const Text('Loopback'))),
          ]),
          const SizedBox(height: 8),
          Row(children: [
            Expanded(child: Text('Device: $_link', style: Theme.of(context).textTheme.bodySmall)),
            TextButton(
                onPressed: _device != null && _link.state == LinkState.connected ? _sendTestScreen : null,
                child: const Text('Test screen')),
            FilledButton.tonal(
                onPressed: _toggleDevice, child: Text(_device == null ? 'Connect' : 'Disconnect')),
          ]),
          for (final l in _deviceLog.take(3))
            Text(l, maxLines: 1, overflow: TextOverflow.ellipsis, style: Theme.of(context).textTheme.bodySmall),
          const SizedBox(height: 8),
          // Listener instead of a button: recording starts on finger down and
          // ends on finger up, with no tap or long press delay in between.
          Listener(
            onPointerDown: (_) => _holdStart(),
            onPointerUp: (_) => _holdEnd(),
            onPointerCancel: (_) => _holdEnd(),
            child: Container(
              height: 96,
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
          _backendPanel(context),
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
