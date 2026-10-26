import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:path_provider/path_provider.dart';

import '../ble/device_transport.dart';
import '../ble/loopback.dart';
import '../halo_runtime.dart';

const _clips = [
  'block_3_4_tmrw_gym',
  'calendar_12_2_meeting',
  'delete_meetings_thursday',
  'email_meeting_moved',
  'email_tomai_meeting_sick',
];

/// The test bench from P0 to P3, moved here unchanged:
/// - Run clips: the P0 benchmark over the bundled clips (P0RESULT lines).
/// - Loopback (P1): the clips through the whole BLE audio path without a
///   radio, compared with the original transcript (LOOPRESULT lines).
/// - Test screen (P1): a sample REVIEW (the device takes it only in PROCESSING).
/// - Link, service and wake lock status (P3), the last device log lines, and
///   every transcript with its timings.
class DebugTab extends StatefulWidget {
  const DebugTab({super.key, required this.rt});
  final HaloRuntime rt;
  @override
  State<DebugTab> createState() => _DebugTabState();
}

class _DebugTabState extends State<DebugTab> {
  bool _busy = false;
  HaloRuntime get _rt => widget.rt;
  void _status(String s) {
    _rt.status = s;
    _rt.changed();
  }

  Future<void> _runClips() async {
    final stt = _rt.stt!;
    setState(() => _busy = true);
    _status('Running clips...');
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
          _rt.entries.insert(0, Entry('pass $pass | $n', r));
          _rt.changed();
        }
      }
      _status('Clips done.');
    } catch (e, st) {
      debugPrint('P0ERROR $e\n$st');
      _status('Error: $e');
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  Future<void> _runLoopback() async {
    final stt = _rt.stt!;
    setState(() => _busy = true);
    _status('Loopback...');
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
        _rt.entries.insert(0, Entry('loopback | $n | ${a.text == b.text ? 'same' : 'DIFFERENT'} | lost ${lb.lost}', b));
        _rt.changed();
      }
      _status('Loopback: $same of ${_clips.length} transcripts identical to the original WAV.');
    } catch (e, st) {
      debugPrint('P1ERROR $e\n$st');
      _status('Error: $e');
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  Future<void> _sendTestScreen() async {
    try {
      await _rt.device!.sendScreen('REVIEW',
          requestId: 'test',
          revision: 1,
          proposalHash: 'test',
          screenLines: ['To: victor@example.com', 'Subject: Meeting moved', 'The meeting moved to', 'Thursday at 3 pm.'],
          statusLine: 'Ready to confirm',
          expiresInS: 60);
    } catch (e) {
      _status('Screen write failed: $e');
    }
  }

  @override
  Widget build(BuildContext context) {
    final ready = _rt.stt != null && !_busy;
    final small = Theme.of(context).textTheme.bodySmall;
    return Padding(
      padding: const EdgeInsets.all(16),
      child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
        Text(_rt.status),
        const SizedBox(height: 8),
        Row(children: [
          Expanded(child: OutlinedButton(onPressed: ready ? _runClips : null, child: const Text('Run clips'))),
          const SizedBox(width: 8),
          Expanded(child: OutlinedButton(onPressed: ready ? _runLoopback : null, child: const Text('Loopback'))),
          const SizedBox(width: 8),
          Expanded(
              child: OutlinedButton(
                  onPressed: _rt.device != null && _rt.link.state == LinkState.connected ? _sendTestScreen : null,
                  child: const Text('Test screen'))),
        ]),
        const SizedBox(height: 8),
        Text('Device: ${_rt.link}', style: small),
        Text('${_rt.serviceStatus}${_rt.lock.held ? ' | wake lock (${_rt.lock.reason})' : ''}'
            '${_rt.lock.lastSummary == null ? '' : ' | last lock ${_rt.lock.lastSummary}'}',
            style: small),
        for (final l in _rt.deviceLog.take(3)) Text(l, maxLines: 1, overflow: TextOverflow.ellipsis, style: small),
        const SizedBox(height: 8),
        Expanded(
          child: ListView(children: [
            for (final e in _rt.entries)
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
    );
  }
}
