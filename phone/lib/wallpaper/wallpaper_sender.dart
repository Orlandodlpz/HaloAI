import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import '../ble/ble_messages.dart';
import '../ble/crc16.dart';
import '../ble/device_transport.dart';

/// Sends a packed 1 bit wallpaper (wallpaper/dither.dart) over the Wallpaper
/// characteristic (P4).
///
/// **The wire format is a proposal** (docs/ble_fields_draft.md, P4 section),
/// within the signed-off contract wording "JSON chunks with offset and
/// checksum, image data base64". The firmware does not handle it yet, so the
/// app keeps Send off until the device's `hello` says `wallpaper: true`. If
/// Victor changes the format, only [chunkMessage] and [send] change.
///
/// Each write: `{"wp", "w", "h", "off", "data"}` (base64), with the usual 2 byte
/// fragment prefix and sized to fit one write. The last one adds `"crc"`,
/// CRC-16/CCITT-FALSE over the whole image. Writes go without response, so the
/// device answers on Events: `{"type":"wallpaper","wp","ok":true}`, or
/// `"ok":false` with `"resend_from"`, and the phone resends from there.
class WallpaperSender {
  WallpaperSender(this.transport, Stream<DeviceEvent> events,
      {this.answerTimeout = const Duration(seconds: 10), this.maxAttempts = 3})
      : _events = events;

  final DeviceTransport transport;
  final Stream<DeviceEvent> _events;
  final Duration answerTimeout;
  final int maxAttempts;
  int _id = 0;
  int _messageId = 0;

  /// Raw image bytes per write: the most whose JSON still fits one write.
  /// Leaves room for the field names, the largest numbers and the crc.
  static int bytesPerWrite(int maxWrite) {
    const overhead = '{"wp":255,"w":800,"h":800,"off":48000,"data":"","crc":65535}';
    final room = maxWrite - 2 - overhead.length;
    return (room ~/ 4) * 3; // base64: 4 characters per 3 bytes
  }

  static Map<String, Object?> chunkMessage(int wp, int w, int h, int off, Uint8List part, {int? crc}) => {
        'wp': wp,
        'w': w,
        'h': h,
        'off': off,
        'data': base64Encode(part),
        'crc': ?crc,
      };

  /// Returns when the device confirms the image, or throws.
  Future<void> send(Uint8List bits, int w, int h) async {
    if (bits.length != w * h ~/ 8) throw ArgumentError('${bits.length} bytes is not a $w x $h 1 bit image');
    final wp = _id++ & 0xFF;
    final crc = crc16CcittFalse([bits]);
    final per = bytesPerWrite(transport.maxWrite);
    var from = 0;
    for (var attempt = 1; attempt <= maxAttempts; attempt++) {
      // Listen before writing so a fast answer is not missed; the timeout
      // only starts after the last chunk, however long the writes took.
      final answer = _events.where((e) => e.type == 'wallpaper' && e.fields['wp'] == wp).first;
      for (var off = from; off < bits.length; off += per) {
        final end = (off + per).clamp(0, bits.length);
        final msg = chunkMessage(wp, w, h, off, Uint8List.sublistView(bits, off, end),
            crc: end == bits.length ? crc : null);
        for (final f in fragment(msg, _messageId++ & 0xFF, transport.maxWrite)) {
          await transport.writeWallpaper(f);
        }
      }
      final e = await answer.timeout(answerTimeout, onTimeout: () => throw TimeoutException('no wallpaper answer'));
      if (e.fields['ok'] == true) return;
      final resend = e.fields['resend_from'];
      if (resend is! int || resend < 0 || resend >= bits.length) throw StateError('device refused the wallpaper: $e');
      from = resend - resend % per; // resend whole chunks
    }
    throw StateError('wallpaper not confirmed after $maxAttempts attempts');
  }
}
