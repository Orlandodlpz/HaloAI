import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../halo_runtime.dart';
import '../mic_test_recorder.dart';

// Shorter presses are discarded, like the device's hold threshold (about
// 500 ms, provisional, owned by the device). Approximation for testing only.
const _minHold = Duration(milliseconds: 500);

/// Hold to talk on the phone's own mic (TEST ONLY, and the I2 fallback if the
/// badge is late): records while held, transcribes on release, sends the text
/// like a device hold (an addition while a proposal is open). Every recording
/// is saved as a WAV in files/recordings/ with a log.tsv line.
class MicHoldButton extends StatefulWidget {
  const MicHoldButton({super.key, required this.rt});
  final HaloRuntime rt;
  @override
  State<MicHoldButton> createState() => _MicHoldButtonState();
}

class _MicHoldButtonState extends State<MicHoldButton> {
  final _mic = MicTestRecorder();
  bool _busy = false;
  bool _recording = false;
  // Pointer events arrive synchronously, but starting the mic is async. These
  // flags make a release during startup stop the recording instead of being lost.
  bool _starting = false;
  bool _fingerDown = false;
  final _holdTimer = Stopwatch();

  HaloRuntime get _rt => widget.rt;
  void _status(String s) {
    _rt.status = s;
    _rt.changed();
  }

  @override
  void initState() {
    super.initState();
    unawaited(_mic.ensurePermission().catchError((Object e) {
      debugPrint('P0ERROR mic permission: $e');
      return false;
    }));
  }

  @override
  void dispose() {
    _mic.dispose();
    super.dispose();
  }

  Future<void> _holdStart() async {
    _fingerDown = true;
    if (_rt.stt == null || _busy || _recording || _starting) return;
    _starting = true;
    try {
      if (!await _mic.ensurePermission()) {
        _status('Microphone permission denied.');
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
        _status('Too short. Hold longer while talking.');
        return;
      }
      HapticFeedback.mediumImpact();
      setState(() => _recording = true);
      _status('Recording... release to send.');
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
        _status('Too short. Hold longer while talking.');
        return;
      }
      _status('Transcribing...');
      final r = await _rt.stt!.transcribeSamples(pcm16ToFloat(pcm), MicTestRecorder.sampleRate);
      final total = release.elapsedMilliseconds;
      final name = 'rec_${_stamp(DateTime.now())}';
      final dir = '${_rt.baseDir}/recordings';
      await writeWav('$dir/$name.wav', pcm, MicTestRecorder.sampleRate);
      await File('$dir/log.tsv').writeAsString(
        '$name.wav\t${r.audioSec.toStringAsFixed(2)}\t${r.decodeMs}\t$total\t${r.text}\n',
        mode: FileMode.append,
        flush: true,
      );
      _rt.record('MICRESULT file=$name.wav audio=${r.audioSec.toStringAsFixed(2)}s '
          'decode=${r.decodeMs}ms release_to_text=${total}ms text="${r.text}"');
      _rt.entries.insert(0, Entry('mic | $name', r, releaseToTextMs: total));
      _status('Release to text $total ms. Saved $name.wav');
      unawaited(_rt.sendSpoken(r.text));
    } catch (e, st) {
      debugPrint('P0ERROR $e\n$st');
      _status('Error: $e');
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  static String _stamp(DateTime t) {
    String two(int v) => v.toString().padLeft(2, '0');
    return '${t.year}${two(t.month)}${two(t.day)}_${two(t.hour)}${two(t.minute)}${two(t.second)}';
  }

  @override
  Widget build(BuildContext context) {
    final ready = _rt.stt != null && !_busy;
    final scheme = Theme.of(context).colorScheme;
    // Listener instead of a button: recording starts on finger down and ends
    // on finger up, with no tap or long press delay in between.
    return Listener(
      onPointerDown: (_) => _holdStart(),
      onPointerUp: (_) => _holdEnd(),
      onPointerCancel: (_) => _holdEnd(),
      child: Container(
        height: 72,
        alignment: Alignment.center,
        decoration: BoxDecoration(
          color: _recording
              ? scheme.error
              : ready
                  ? scheme.primary
                  : scheme.surfaceContainerHighest,
          borderRadius: BorderRadius.circular(20),
        ),
        child: Row(mainAxisSize: MainAxisSize.min, children: [
          Icon(_recording ? Icons.mic : Icons.mic_none, color: ready || _recording ? scheme.onPrimary : null),
          const SizedBox(width: 8),
          Text(
            _recording ? 'Recording... release to send' : 'Hold to talk (phone mic)',
            style: TextStyle(color: ready || _recording ? scheme.onPrimary : null, fontSize: 18),
          ),
        ]),
      ),
    );
  }
}
