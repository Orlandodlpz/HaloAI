import 'package:flutter_test/flutter_test.dart';
import 'package:haloai_phone/ble/badge_choice.dart';

void main() {
  const mine = 'E8:3D:C1:F6:FF:4E';
  const other = '14:C1:9F:29:CF:36';

  test('a remembered badge is the only one ever connected, however strong another is', () {
    final d = decideBadge(mine, const [FoundBadge(other, -40)], windowOver: true);
    expect(d, isA<KeepScanning>());
  });

  test('a remembered badge connects as soon as it is heard, without waiting', () {
    final d = decideBadge(mine, const [FoundBadge(other, -40), FoundBadge(mine, -70)], windowOver: false);
    expect((d as ConnectTo).id, mine);
  });

  test('nothing remembered: wait out the window before settling on the first badge', () {
    expect(decideBadge(null, const [FoundBadge(other, -50)], windowOver: false), isA<KeepScanning>());
    expect((decideBadge(null, const [FoundBadge(other, -50)], windowOver: true) as ConnectTo).id, other);
  });

  test('nothing remembered and several badges: ask, strongest first', () {
    final d = decideBadge(null, const [FoundBadge(other, -80), FoundBadge(mine, -45)], windowOver: true);
    expect((d as AskUser).badges.map((b) => b.id), [mine, other]);
  });

  test('nothing heard: keep scanning', () {
    expect(decideBadge(null, const [], windowOver: true), isA<KeepScanning>());
    expect(decideBadge(mine, const [], windowOver: true), isA<KeepScanning>());
  });
}
