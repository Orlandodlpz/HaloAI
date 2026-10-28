import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:haloai_phone/wallpaper/dither.dart';
import 'package:image/image.dart' as img;

Uint8List solid(int w, int h, int r, int g, int b) {
  final im = img.Image(width: w, height: h);
  img.fill(im, color: img.ColorRgb8(r, g, b));
  return img.encodePng(im);
}

void main() {
  test('800 x 480 packs to 48,000 bytes; white is 1 and black is 0', () {
    final white = prepareWallpaper((solid(1200, 900, 255, 255, 255), 800, 480, WallpaperFit.fill));
    expect(white.length, 48000);
    expect(white.every((b) => b == 0xFF), isTrue);
    final black = prepareWallpaper((solid(640, 480, 0, 0, 0), 800, 480, WallpaperFit.fill));
    expect(black.every((b) => b == 0x00), isTrue);
  });

  test('portrait is 480 x 800, the same byte count', () {
    expect(prepareWallpaper((solid(300, 500, 255, 255, 255), 480, 800, WallpaperFit.fit)).length, 48000);
  });

  test('the leftmost pixel is the most significant bit', () {
    final white = List<bool>.filled(16, false)..[0] = true..[9] = true;
    expect(pack(white, 16, 1), [0x80, 0x40]);
  });

  test('50% gray dithers to about half white, and the same input gives the same bits', () {
    final gray = Uint8List(64 * 64)..fillRange(0, 64 * 64, 128);
    final a = floydSteinberg(gray, 64, 64), b = floydSteinberg(gray, 64, 64);
    final whites = a.where((w) => w).length / a.length;
    expect(whites, closeTo(0.5, 0.05));
    expect(a, b);
  });

  test('fill crops a wide black photo to cover; fit keeps it whole on white bars', () {
    // A black photo twice as wide as tall: 2:1 against the panel's 5:3.
    final wide = solid(1000, 500, 0, 0, 0);
    final fill = prepareWallpaper((wide, 800, 480, WallpaperFit.fill));
    expect(fill.every((b) => b == 0x00), isTrue, reason: 'covered edge to edge, no bars');
    final fit = prepareWallpaper((wide, 800, 480, WallpaperFit.fit));
    // Scaled to 800 x 400: 40 white rows above and below, black between.
    bool white(int x, int y) => fit[(y * 800 + x) >> 3] & (0x80 >> ((y * 800 + x) & 7)) != 0;
    expect([white(400, 0), white(400, 39), white(400, 40), white(400, 240), white(400, 439), white(400, 440)],
        [true, true, false, false, false, true]);
  });

  test('the preview is a PNG of the panel size', () {
    final bits = prepareWallpaper((solid(800, 480, 255, 255, 255), 800, 480, WallpaperFit.fill));
    final png = img.decodePng(previewPng((bits, 800, 480)))!;
    expect((png.width, png.height), (800, 480));
    expect(png.getPixel(10, 10).r, 255);
    expect(png.getPixel(10, 272).g, 255, reason: 'no crop line: the CrowPanel squeezes, it does not cut');
  });
}
