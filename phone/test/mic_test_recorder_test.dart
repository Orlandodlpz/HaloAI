import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:haloai_phone/mic_test_recorder.dart';

void main() {
  test('pcm16ToFloat maps int16 range to [-1, 1]', () {
    final pcm = Uint8List.fromList([0x00, 0x00, 0xFF, 0x7F, 0x00, 0x80]); // 0, 32767, -32768
    final f = pcm16ToFloat(pcm);
    expect(f.length, 3);
    expect(f[0], 0.0);
    expect(f[1], closeTo(1.0, 1e-4));
    expect(f[2], -1.0);
  });

  test('writeWav writes a 16 kHz mono 16-bit header matching the P0 clips', () async {
    final dir = await Directory.systemTemp.createTemp();
    final path = '${dir.path}/t.wav';
    await writeWav(path, Uint8List(3200), 16000);
    final b = ByteData.sublistView(await File(path).readAsBytes());
    expect(b.lengthInBytes, 44 + 3200);
    expect(String.fromCharCodes(b.buffer.asUint8List(0, 4)), 'RIFF');
    expect(b.getUint16(22, Endian.little), 1); // mono
    expect(b.getUint32(24, Endian.little), 16000);
    expect(b.getUint16(34, Endian.little), 16);
    expect(b.getUint32(40, Endian.little), 3200);
    await dir.delete(recursive: true);
  });
}
