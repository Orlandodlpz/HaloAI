import 'dart:io';
import 'dart:math';

import 'package:flutter_test/flutter_test.dart';
import 'package:haloai_phone/ble/loopback.dart';

void main() {
  test('a real P0 clip survives the full audio path with lost chunks', () {
    final pcm = wavPcm16(File('assets/clips/block_3_4_tmrw_gym.wav').readAsBytesSync());
    expect(pcm.length, greaterThan(16000));
    final r = loopback(pcm);
    expect(r.lost, greaterThan(10));
    expect(r.resendRequests, greaterThan(0));
    expect(r.audio.pcm.length, pcm.length);

    // ADPCM is lossy; speech should still be well above 20 dB SNR.
    double sig = 0, err = 0;
    for (var i = 0; i < pcm.length; i++) {
      sig += pcm[i] * pcm[i].toDouble();
      final d = pcm[i] - r.audio.pcm[i];
      err += d * d.toDouble();
    }
    expect(10 * log(sig / err) / ln10, greaterThan(20));
  });

  test('no losses: no resend requests', () {
    final pcm = wavPcm16(File('assets/clips/email_meeting_moved.wav').readAsBytesSync());
    final r = loopback(pcm, loseEvery: 0);
    expect((r.lost, r.resendRequests), (0, 0));
  });
}
