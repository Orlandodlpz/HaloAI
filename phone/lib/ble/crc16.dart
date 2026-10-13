import 'dart:typed_data';

/// CRC-16/CCITT-FALSE: polynomial 0x1021, initial value 0xFFFF, no reflection,
/// no final XOR. Check value for ASCII "123456789" is 0x29B1.
/// The audio chunk CRC runs over header bytes 0 to 9, then the payload.
int crc16CcittFalse(Iterable<Uint8List> parts) {
  var crc = 0xFFFF;
  for (final part in parts) {
    for (final byte in part) {
      crc ^= byte << 8;
      for (var i = 0; i < 8; i++) {
        crc = (crc & 0x8000) != 0 ? ((crc << 1) ^ 0x1021) & 0xFFFF : (crc << 1) & 0xFFFF;
      }
    }
  }
  return crc;
}
