import 'dart:async';
import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:haloai_phone/backend/api.dart';
import 'package:haloai_phone/backend/backend_client.dart';
import 'package:http/http.dart' as http;
import 'package:http/testing.dart';

http.StreamedResponse sse(String body, {int status = 200}) =>
    http.StreamedResponse(Stream.value(utf8.encode(body)), status);

http.StreamedResponse json(Object body, {int status = 200, Map<String, String> headers = const {}}) =>
    http.StreamedResponse(Stream.value(utf8.encode(jsonEncode(body))), status, headers: headers);

const review = '{"state":"review","request_id":"r","revision":1,"screen_lines":["Gym"],'
    '"proposal_hash":"h","execution_key":"abcdefghijklmnopqrstuvwxyz"}';
const okStream = 'event: accepted\ndata: {"request_id":"r","revision":1,"call_id":"c"}\n\n'
    'event: result\ndata: $review\n\n';

void main() {
  final config = BackendConfig(Uri.parse('https://api.example'), 'k');
  late List<Map<String, Object?>> bodies;
  late List<Duration> slept;

  BackendClient client(Future<http.StreamedResponse> Function(int n, http.BaseRequest r) handler,
      {Duration idle = const Duration(seconds: 25)}) {
    var n = 0;
    return BackendClient(config,
        idleTimeout: idle,
        sleep: (d) async => slept.add(d),
        client: MockClient.streaming((req, body) async {
          final text = await body.bytesToString();
          bodies.add(text.isEmpty ? {} : (jsonDecode(text) as Map).cast<String, Object?>());
          expect(req.headers['Authorization'], 'Bearer k');
          return handler(n++, req);
        }));
  }

  setUp(() {
    bodies = [];
    slept = [];
  });

  Stream<StreamEvent> start(BackendClient c) => c.start(
      requestId: 'r', segmentId: 's', text: 'gym', sentAt: '2026-10-20T10:00:00-05:00', timezone: 'America/Chicago');

  test('start sends the contract fields and yields accepted then the result', () async {
    final events = await start(client((_, _) async => sse(okStream))).toList();
    expect(events.first, isA<Accepted>());
    final r = (events.last as ResultEvent).result;
    expect((r.state, r.proposalHash, r.executionKey), ('review', 'h', 'abcdefghijklmnopqrstuvwxyz'));
    expect(bodies.single.keys.toSet(),
        {'call_id', 'request_id', 'segment_id', 'text', 'sent_at', 'timezone', 'language', 'orientation'});
  });

  test('a stream that goes silent is retried with the same call_id', () async {
    final c = client((n, _) async {
      if (n == 0) return http.StreamedResponse(StreamController<List<int>>().stream, 200); // never speaks
      return sse(okStream);
    }, idle: const Duration(milliseconds: 50));
    final events = await start(c).toList();
    expect(events.last, isA<ResultEvent>());
    expect(bodies, hasLength(2));
    expect(bodies[1]['call_id'], bodies[0]['call_id']);
  });

  test('a stream that ends without a result is retried with the same call_id', () async {
    final c = client((n, _) async => n == 0 ? sse('event: accepted\ndata: {"request_id":"r","revision":1}\n\n') : sse(okStream));
    final events = await start(c).toList();
    expect(events.whereType<ResultEvent>(), hasLength(1));
    expect(bodies[1]['call_id'], bodies[0]['call_id']);
  });

  test('429 waits Retry-After and retries with the same call_id', () async {
    final c = client((n, _) async => n == 0
        ? json({'error': {'code': 'rate_limited', 'message': 'slow down', 'current': null}},
            status: 429, headers: {'retry-after': '3'})
        : json(jsonDecode(review)));
    final r = await c.approve('r', revision: 1, proposalHash: 'h');
    expect(r.state, 'review');
    expect(slept, [const Duration(seconds: 3)]);
    expect(bodies[1]['call_id'], bodies[0]['call_id']);
  });

  test('an error answer is not retried and carries the current result', () async {
    final c = client((_, _) async => json({
          'error': {'code': 'stale_revision', 'message': 'old', 'current': jsonDecode(review)}
        }, status: 409));
    await expectLater(
        c.approve('r', revision: 1, proposalHash: 'h'),
        throwsA(isA<ApiError>()
            .having((e) => e.code, 'code', 'stale_revision')
            .having((e) => e.current?.revision, 'current revision', 1)));
    expect(bodies, hasLength(1));
  });

  test('a network error on approve is retried with the same call_id', () async {
    final c = client((n, _) async {
      if (n == 0) throw http.ClientException('connection reset');
      return json(jsonDecode(review));
    });
    await c.approve('r', revision: 1, proposalHash: 'h');
    expect(bodies, hasLength(2));
    expect(bodies[1]['call_id'], bodies[0]['call_id']);
  });

  test('touch and GETs carry no call_id', () async {
    final c = client((_, _) async => json(jsonDecode(review)));
    await c.touch('r', revision: 1);
    await c.executionStatus('abcdefghijklmnopqrstuvwxyz');
    expect(bodies[0], {'revision': 1});
    expect(bodies[1], isEmpty);
  });
}
