import 'package:flutter_test/flutter_test.dart';
import 'package:haloai_phone/backend/sse.dart';

void main() {
  test('events in the server format, split at arbitrary points', () {
    const wire = 'event: accepted\ndata: {"request_id":"r","revision":1,"call_id":"c"}\n\n'
        ': ping\n\n'
        'event: status\ndata: {"request_id":"r","revision":1,"status_line":"Email to Victor"}\n\n'
        'event: result\ndata: {"state":"review","request_id":"r","revision":1}\n\n';
    for (final size in [1, 3, 7, 1000]) {
      final p = SseParser();
      final got = <(String, String)>[];
      for (var i = 0; i < wire.length; i += size) {
        got.addAll(p.add(wire.substring(i, i + size > wire.length ? wire.length : i + size)));
      }
      expect(got.map((e) => e.$1), ['accepted', 'status', 'result'], reason: 'chunk size $size');
      expect(SseParser.json(got[1].$2)['status_line'], 'Email to Victor');
    }
  });

  test('CRLF endings and a data line without a space', () {
    final got = SseParser().add('event: slow\r\ndata:{"revision":2}\r\n\r\n');
    expect(got, [('slow', '{"revision":2}')]);
  });

  test('pings alone produce nothing, and an unfinished event waits', () {
    final p = SseParser();
    expect(p.add(': ping\n\n: ping\n\nevent: result\ndata: {}'), isEmpty);
    expect(p.add('\n\n'), [('result', '{}')]);
  });
}
