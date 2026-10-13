import 'dart:convert';
import 'dart:math';

import 'package:flutter_test/flutter_test.dart';
import 'package:haloai_phone/ble/ble_messages.dart';
import 'package:haloai_phone/ble/id_map.dart';

void main() {
  group('IdMap', () {
    test('same boot keeps mappings, new boot drops them', () {
      final m = IdMap(random: Random(1));
      expect(m.onHello(111), isTrue);
      final r = m.requestId(5);
      final s = m.segmentId(5, 0);
      expect(m.requestId(5), r);
      expect(m.segmentId(5, 0), s);
      expect(m.segmentId(5, 1), isNot(s));
      expect(RegExp(r'^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$').hasMatch(r), isTrue);

      final screen = m.newScreen(requestId: r, revision: 2, proposalHash: 'h', candidateIds: ['c1', 'c2']);
      expect(m.onHello(111), isFalse, reason: 'reconnect, same boot');
      expect(m.requestId(5), r);
      expect(m.candidateId(screen.sid, 1), 'c2');

      expect(m.onHello(222), isTrue, reason: 'device restarted');
      expect(m.requestId(5), isNot(r));
      expect(m.screen(screen.sid), isNull);
    });

    test('sid binds an event to what was drawn; unknown index is null', () {
      final m = IdMap(random: Random(1))..onHello(1);
      final a = m.newScreen(requestId: 'r', revision: 3, proposalHash: 'hash3');
      final b = m.newScreen(requestId: 'r', revision: 4, proposalHash: 'hash4');
      expect(b.sid, isNot(a.sid));
      expect(m.screen(a.sid)!.proposalHash, 'hash3');
      expect(m.candidateId(a.sid, 0), isNull);
      expect(m.candidateId(999, 0), isNull);
    });

    test('old screens are forgotten after keepScreens', () {
      final m = IdMap(random: Random(1))..onHello(1);
      final first = m.newScreen();
      for (var i = 0; i < IdMap.keepScreens; i++) {
        m.newScreen();
      }
      expect(m.screen(first.sid), isNull);
    });

    test('revision low byte resolves against the latest full revision', () {
      expect(IdMap.resolveRevision(3, 3), 3);
      expect(IdMap.resolveRevision(2, 3), 2);
      expect(IdMap.resolveRevision(255, 257), 255); // shown 255, latest went to 257
      expect(IdMap.resolveRevision(1, 257), 257);
      expect(IdMap.resolveRevision(5, 3), isNull);
    });
  });

  group('messages', () {
    test('events parse; unknown or typeless events are rejected', () {
      final e = DeviceEvent.parse('{"type":"select","sid":7,"index":1}');
      expect((e.type, e.sid, e.index), ('select', 7, 1));
      final h = DeviceEvent.parse('{"type":"hello","fw":"0.1","boot":42,"state":"READY","req":3,"sid":null,"pending_key":null,"battery":80}');
      expect((h.boot, h.req, h.pendingKey), (42, 3, null));
      expect(() => DeviceEvent.parse('{"type":"explode"}'), throwsFormatException);
      expect(() => DeviceEvent.parse('{"sid":1}'), throwsFormatException);
    });

    test('screen JSON leaves out absent fields', () {
      final s = ScreenMessage(sid: 4, screen: 'PROCESSING', statusLine: 'Email to Victor');
      expect(s.toJson(), {'sid': 4, 'screen': 'PROCESSING', 'screen_lines': [], 'status_line': 'Email to Victor', 'candidates': []});
      expect(settingsJson(phoneReady: true), {'phone_ready': true});
    });

    test('fragment and join: one packet and many packets', () {
      final small = {'sid': 1, 'screen': 'READY'};
      final one = fragment(small, 9, 253);
      expect(one, hasLength(1));
      expect(one.single.sublist(0, 2), [9, 0x80]);
      expect(jsonDecode(Defragmenter().add(one.single)!), small);

      final big = ScreenMessage(sid: 2, screen: 'REVIEW', screenLines: List.filled(20, 'x' * 40)).toJson();
      final parts = fragment(big, 10, 100);
      expect(parts.length, greaterThan(5));
      expect(parts.every((p) => p.length <= 100), isTrue);
      final d = Defragmenter();
      String? out;
      for (final p in parts) {
        out = d.add(p);
      }
      expect(jsonDecode(out!), big);
    });

    test('a lost fragment drops the message, the next message still arrives', () {
      final big = ScreenMessage(sid: 2, screen: 'REVIEW', screenLines: List.filled(10, 'y' * 40)).toJson();
      final parts = fragment(big, 1, 100);
      final d = Defragmenter();
      String? out;
      for (final p in [parts[0], ...parts.skip(2)]) {
        out = d.add(p);
      }
      expect(out, isNull);
      expect(d.add(fragment({'sid': 3, 'screen': 'READY'}, 2, 100).single), isNotNull);
    });
  });
}
