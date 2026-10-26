import 'dart:typed_data';

import 'package:image/image.dart' as img;

/// Wallpaper preparation (P4): any photo to the exact bits the panel shows.
///
/// The panel is driven 1 bit (BRIEFING section 2, Screen text): 800 x 480 in
/// landscape, 480 x 800 in portrait. Packed the same way as both panels'
/// framebuffers (CLAUDE.md, Device): 1 bit per pixel, rows top to bottom, most
/// significant bit leftmost, **1 means white**. 800 x 480 is 48,000 bytes.

const landscape = (w: 800, h: 480);
const portrait = (w: 480, h: 800);

/// How a photo meets the panel's shape.
/// - [fill]: scale until the panel is covered and crop the overflow, centred.
///   No borders, but the edges of the photo are lost.
/// - [fit]: scale until the whole photo fits, on white bars. Nothing is lost.
enum WallpaperFit { fill, fit }

/// Decodes [encoded] (JPEG, PNG, ...), fits it to the panel (see
/// [WallpaperFit]), dithers and packs it. Top-level so it can run in
/// `compute()` (a closure would capture widget state and fail as unsendable).
Uint8List prepareWallpaper((Uint8List encoded, int w, int h, WallpaperFit fit) args) {
  final (encoded, w, h, mode) = args;
  final src = img.decodeImage(encoded) ?? (throw const FormatException('not an image this app can read'));
  final oriented = img.bakeOrientation(src); // phone photos carry EXIF rotation
  final fitted = mode == WallpaperFit.fill ? _cover(oriented, w, h) : _contain(oriented, w, h);
  final gray = Uint8List(w * h);
  for (var y = 0; y < h; y++) {
    for (var x = 0; x < w; x++) {
      gray[y * w + x] = img.getLuminance(fitted.getPixel(x, y)).round().clamp(0, 255);
    }
  }
  return pack(floydSteinberg(gray, w, h), w, h);
}

/// Centre crop to the target aspect, then resize to exactly [w] x [h].
img.Image _cover(img.Image src, int w, int h) {
  final scale = (w / src.width) > (h / src.height) ? w / src.width : h / src.height;
  final cw = (w / scale).round().clamp(1, src.width), ch = (h / scale).round().clamp(1, src.height);
  final crop = img.copyCrop(src, x: (src.width - cw) ~/ 2, y: (src.height - ch) ~/ 2, width: cw, height: ch);
  return img.copyResize(crop, width: w, height: h, interpolation: img.Interpolation.average);
}

/// The whole photo scaled to fit inside [w] x [h], centred on white.
img.Image _contain(img.Image src, int w, int h) {
  final scale = (w / src.width) < (h / src.height) ? w / src.width : h / src.height;
  final sw = (src.width * scale).round().clamp(1, w), sh = (src.height * scale).round().clamp(1, h);
  final resized = img.copyResize(src, width: sw, height: sh, interpolation: img.Interpolation.average);
  final canvas = img.Image(width: w, height: h, numChannels: 3)..clear(img.ColorRgb8(255, 255, 255));
  return img.compositeImage(canvas, resized, dstX: (w - sw) ~/ 2, dstY: (h - sh) ~/ 2);
}

/// Floyd–Steinberg error diffusion: 8 bit gray in, one bool per pixel out
/// (true = white). Error goes 7/16 right, 3/16 down left, 5/16 down,
/// 1/16 down right.
List<bool> floydSteinberg(Uint8List gray, int w, int h) {
  final err = Float32List(w * h);
  for (var i = 0; i < gray.length; i++) {
    err[i] = gray[i].toDouble();
  }
  final out = List<bool>.filled(w * h, false);
  for (var y = 0; y < h; y++) {
    for (var x = 0; x < w; x++) {
      final i = y * w + x;
      final old = err[i];
      final white = old >= 128;
      out[i] = white;
      final e = old - (white ? 255 : 0);
      if (x + 1 < w) err[i + 1] += e * 7 / 16;
      if (y + 1 < h) {
        if (x > 0) err[i + w - 1] += e * 3 / 16;
        err[i + w] += e * 5 / 16;
        if (x + 1 < w) err[i + w + 1] += e * 1 / 16;
      }
    }
  }
  return out;
}

/// 1 bit per pixel, most significant bit leftmost, 1 = white. [w] must be a
/// multiple of 8 (800 and 480 both are).
Uint8List pack(List<bool> white, int w, int h) {
  assert(w % 8 == 0);
  final out = Uint8List(w * h ~/ 8);
  for (var i = 0; i < w * h; i++) {
    if (white[i]) out[i >> 3] |= 0x80 >> (i & 7);
  }
  return out;
}

/// The packed bits as a PNG for the preview, so the phone shows exactly what
/// would be sent. No crop line: the CrowPanel squeezes the whole canvas into
/// its 272 rows rather than cutting it (Victor, 2026-10-25), and the badge
/// shows all of it.
Uint8List previewPng((Uint8List packed, int w, int h) args) {
  final (packed, w, h) = args;
  final im = img.Image(width: w, height: h, numChannels: 3);
  for (var y = 0; y < h; y++) {
    for (var x = 0; x < w; x++) {
      final i = y * w + x;
      final v = packed[i >> 3] & (0x80 >> (i & 7)) != 0 ? 255 : 0;
      im.setPixelRgb(x, y, v, v, v);
    }
  }
  return img.encodePng(im);
}
